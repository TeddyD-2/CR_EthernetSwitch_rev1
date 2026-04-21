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
static const uint16_t DST_PORT = 5001;

// Throughput config — the KSZ9477 has no LED activity-stretch circuit
// (datasheet §5.4.5 is the only LED register; just the mode bit). So magjack
// LEDx_0 visibility is purely a wire-time duty cycle problem. Rough math at
// 100 Mbps:
//     80 B packet     ≈  6 µs on the wire
//   1400 B packet     ≈ 112 µs on the wire
// To see LEDx_0 clearly blink you want >~1% duty cycle. The defaults below
// (MTU-sized packets, no inter-send delay) saturate the link hard enough to
// make activity obvious. For periodic-log use, set PAYLOAD_SIZE=32 and
// SEND_PERIOD_MS=1000.
static const size_t   PAYLOAD_SIZE   = 1400;   // bytes in UDP payload
static const uint32_t SEND_PERIOD_MS = 0;      // 0 = send as fast as loop runs

// ---- PHY link -------------------------------------------------------------
// Max speed advertised on ports 1..5 during auto-negotiation.
// One of: 10, 100, 1000.
#define LINK_SPEED_CAP 100

// Magjack LED behavior (KSZ9477S datasheet 4.2):
//   KSZ9477::LED_SINGLE        — LEDx_1 = Link, LEDx_0 = Activity
//   KSZ9477::LED_DUAL_TRICOLOR — 1000=LEDx_1, 100=LEDx_0, 10=both
#define LED_MODE  KSZ9477::LED_SINGLE

// ---- KSZ9477 SPI + control pins (board-specific) --------------------------
#define KSZ_SCK   PA5
#define KSZ_MISO  PA6
#define KSZ_MOSI  PD7   // PA7 is ETH_CRS_DV, so MOSI is remapped
#define KSZ_CS    PB2
#define KSZ_RST   PE7
#define KSZ_INT   PB0

// ---- Status LEDs (board-specific) -----------------------------------------
// LED_HEARTBEAT toggles every HEARTBEAT_MS — liveness indicator.
// LED_TX        toggles on every UDP send    — TX activity indicator.
#define LED_HEARTBEAT PD3
#define LED_TX        PD2
static const uint32_t HEARTBEAT_MS = 250;

// ============================================================================

KSZ9477     sw;
EthernetUDP udp;

void setup()
{
  pinMode(LED_HEARTBEAT, OUTPUT);
  pinMode(LED_TX,        OUTPUT);

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

  // Give auto-neg a moment to settle, then report which ports have link.
  delay(1500);
  Serial.print("Front-port link:");
  for (uint8_t p = 1; p <= 5; p++) {
    Serial.print(" P"); Serial.print(p);
    Serial.print(sw.linkUp(p) ? "=UP" : "=dn");
  }
  Serial.println();

  Serial.print("Sending from ");
  Serial.print(Ethernet.localIP());
  Serial.print(" -> ");
  Serial.print(dstIp); Serial.print(":"); Serial.println(DST_PORT);
  Serial.print("Payload ");  Serial.print(PAYLOAD_SIZE);
  Serial.print(" B, period "); Serial.print(SEND_PERIOD_MS);
  Serial.println(" ms");
}

void loop()
{
  uint32_t now = millis();

  // Heartbeat
  static uint32_t lastBeat = 0;
  if (now - lastBeat >= HEARTBEAT_MS) {
    lastBeat = now;
    digitalWrite(LED_HEARTBEAT, !digitalRead(LED_HEARTBEAT));
  }

  // Periodic UDP send
  static uint32_t lastSend = 0;
  static uint32_t counter  = 0;
  static uint32_t failCount = 0;
  if (SEND_PERIOD_MS && (now - lastSend < SEND_PERIOD_MS)) return;
  lastSend = now;
  counter++;

  static uint8_t payload[1500];
  size_t n = PAYLOAD_SIZE;
  if (n > sizeof(payload)) n = sizeof(payload);
  // First bytes identify the counter; rest is arbitrary.
  snprintf((char*)payload, n, "#%lu ", (unsigned long)counter);

  udp.beginPacket(dstIp, DST_PORT);
  udp.write(payload, n);
  int ok = udp.endPacket();
  if (!ok) failCount++;
  digitalWrite(LED_TX, !digitalRead(LED_TX));

  // Rate-limit Serial to ~1 Hz regardless of send rate
  static uint32_t lastLog = 0;
  if (now - lastLog >= 1000) {
    lastLog = now;
    Serial.print("[tx] sent=");   Serial.print(counter);
    Serial.print(" fail=");       Serial.print(failCount);
    Serial.print(" size=");       Serial.println(n);
  }
}
