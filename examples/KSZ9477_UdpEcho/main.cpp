/*
  KSZ9477_UdpEcho — minimal UDP echo server on the Cornell Racing KSZ9477
  board (STM32H753VIT6 + KSZ9477S).

  Brings up the switch, starts the TCP/IP stack with a static IP, and
  bounces any UDP packet received on LOCAL_PORT back to its sender.

  Test from a PC on the same subnet (e.g. 192.168.1.50/24, plugged into
  any front-panel KSZ port):

      ping 192.168.1.100
      python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);\
                 s.sendto(b'hello',('192.168.1.100',5000));\
                 s.settimeout(2);print(s.recv(512))"
*/

#include <Arduino.h>
#include <KSZ9477.h>

// ============================================================================
// User configuration
// ============================================================================

// ---- Network --------------------------------------------------------------
static uint8_t   mac[]    = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };
static IPAddress localIp  (192, 168, 1, 100);
static IPAddress gateway  (192, 168, 1,   1);
static IPAddress netmask  (255, 255, 255, 0);
static const uint16_t LOCAL_PORT = 5000;

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
static uint8_t packetBuf[512];

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}
  Serial.println("\n=== KSZ9477 UDP Echo ===");

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
  udp.begin(LOCAL_PORT);
  sw.reinitSpi();                         // HAL_ETH_MspInit may re-mux PA5/PA6

  Serial.print("Listening on udp://");
  Serial.print(Ethernet.localIP());
  Serial.print(":");
  Serial.println(LOCAL_PORT);
}

void loop()
{
  int size = udp.parsePacket();
  if (size <= 0) return;

  int n = udp.read(packetBuf, sizeof(packetBuf));
  udp.beginPacket(udp.remoteIP(), udp.remotePort());
  udp.write(packetBuf, n);
  udp.endPacket();

  Serial.print("[echo] ");  Serial.print(n);
  Serial.print(" B from "); Serial.print(udp.remoteIP());
  Serial.print(":");        Serial.println(udp.remotePort());
}
