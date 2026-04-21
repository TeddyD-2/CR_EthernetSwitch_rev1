/*
  KSZ9477_UdpSender — STM32 transmits UDP packets out through the switch.

  Brings up the switch, starts the TCP/IP stack, and sends a UDP packet
  containing a counter + millis() timestamp once per SEND_PERIOD_MS to
  DST_IP:DST_PORT.

  Default DST_IP is the subnet broadcast (192.168.1.255) so any PC on
  192.168.1.0/24 will receive it. Listen on the PC:

      python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);\
                 s.bind(('',5001));\
                 while True: print(s.recvfrom(512))"
*/

#include <Arduino.h>
#include <KSZ9477.h>

// ============================================================================
// User configuration
// ============================================================================

// ---- Network --------------------------------------------------------------
static uint8_t   mac[]    = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x45 };
static IPAddress localIp  (192, 168, 1, 100);
static IPAddress gateway  (192, 168, 1,   1);
static IPAddress netmask  (255, 255, 255, 0);
static IPAddress dstIp    (192, 168, 1, 255);   // subnet broadcast
static const uint16_t DST_PORT       = 5001;
static const uint32_t SEND_PERIOD_MS = 1000;

// ---- PHY link -------------------------------------------------------------
// Max speed advertised on ports 1..5 during auto-negotiation.
// One of: 10, 100, 1000.
#define LINK_SPEED_CAP 100

// Magjack LED behavior (KSZ9477S datasheet 4.2):
//   KSZ9477::LED_SINGLE        — LEDx_1 = Link, LEDx_0 = Activity
//   KSZ9477::LED_DUAL_TRICOLOR — 1000=LEDx_1, 100=LEDx_0, 10=both
#define LED_MODE  KSZ9477::LED_DUAL_TRICOLOR

// ---- KSZ9477 SPI + control pins (board-specific) --------------------------
#define KSZ_SCK   PA5
#define KSZ_MISO  PA6
#define KSZ_MOSI  PD7   // PA7 is ETH_CRS_DV, so MOSI is remapped
#define KSZ_CS    PB2
#define KSZ_RST   PE7
#define KSZ_INT   PB0

// ============================================================================

KSZ9477     sw;
EthernetUDP udp;

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println("\n=== KSZ9477 UDP Sender ===");

  sw.setPins(KSZ_SCK, KSZ_MISO, KSZ_MOSI, KSZ_CS, KSZ_RST, KSZ_INT);
  if (!sw.begin()) {
    Serial.println("KSZ9477 not responding — halting.");
    while (1) {}
  }

  sw.configureCpuPort();                  // RMII 100 FD, KSZ drives REFCLKO6
  sw.startSwitch();
  sw.configureLeds(LED_MODE);
  sw.setSpeedCap(LINK_SPEED_CAP);

  Ethernet.begin(mac, localIp, gateway, gateway, netmask);
  udp.begin(DST_PORT);                    // allocates a pcb for sending
  sw.reinitSpi();                         // HAL_ETH_MspInit may re-mux PA5/PA6

  Serial.print("Sending from ");
  Serial.print(Ethernet.localIP());
  Serial.print(" -> ");
  Serial.print(dstIp); Serial.print(":"); Serial.println(DST_PORT);
}

void loop()
{
  static uint32_t lastSend = 0;
  static uint32_t counter  = 0;

  uint32_t now = millis();
  if (now - lastSend < SEND_PERIOD_MS) return;
  lastSend = now;
  counter++;

  char msg[64];
  int n = snprintf(msg, sizeof(msg), "hello #%lu t=%lums\n",
                   (unsigned long)counter, (unsigned long)now);

  udp.beginPacket(dstIp, DST_PORT);
  udp.write((const uint8_t*)msg, n);
  int ok = udp.endPacket();

  Serial.print("[tx] "); Serial.print(ok ? "OK " : "FAIL ");
  Serial.print(n);       Serial.print(" B: ");
  Serial.print(msg);
}
