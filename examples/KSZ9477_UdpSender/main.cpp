/*
  KSZ9477_UdpSender — STM32 transmits UDP packets out through the switch.

  Brings up the KSZ9477, starts STM32Ethernet at 192.168.1.100, and sends
  a UDP packet containing a counter + millis() timestamp once per second
  to 192.168.1.255:5001 (subnet broadcast, so any PC on 192.168.1.0/24
  will receive it regardless of its specific IP).

  How to receive on your PC (PC at 192.168.1.x/24, plugged into any KSZ
  front port):

      python -c "import socket s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
      s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
      s.bind(('',5001))
      while True: print(s.recvfrom(512))"

  Or with netcat:   nc -ul 5001
*/

#include <Arduino.h>
#include <KSZ9477.h>

// ---- Link speed -----------------------------------------------------------
// Set to 1 to cap PHY ports 1..5 at 100 Mbps full-duplex. Auto-negotiation
// still runs, but the 1000BASE-T advertisement is withdrawn. In tri-color
// dual-LED mode this lights LEDx_0 for link/activity; at 1000 Mbps LEDx_0
// stays off, so forcing 100 is a useful visual confirmation that the LED_0
// pins and their magjack wiring are healthy.
#define FORCE_100_MBPS 1

static void capPortTo100Mbps(KSZ9477 &sw, uint8_t port)
{
  const uint32_t base = (uint32_t)port << 12;
  uint16_t ctrl1000 = sw.readReg16(base | 0x0112);
  ctrl1000 &= ~((1u << 9) | (1u << 8));
  sw.writeReg16(base | 0x0112, ctrl1000);
  uint16_t bmcr = sw.readReg16(base | 0x0100);
  bmcr |= (1u << 9);
  sw.writeReg16(base | 0x0100, bmcr);
}

// ---- KSZ9477 SPI + control pins -------------------------------------------
#define KSZ_SCK    PA5
#define KSZ_MISO   PA6
#define KSZ_MOSI   PD7
#define KSZ_CS     PB2
#define KSZ_RST    PE7
#define KSZ_INT    PB0

// ---- User LEDs ------------------------------------------------------------
#define LED_R1     PD1
#define LED_G1     PD2
#define LED_R2     PD3
#define LED_G2     PD4

// ---- Network --------------------------------------------------------------
static uint8_t   mac[]   = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x45 };
static IPAddress ip      (192, 168, 1, 100);
static IPAddress gw      (192, 168, 1,   1);
static IPAddress mask    (255, 255, 255, 0);
static IPAddress dstIp   (192, 168, 1, 255);   // subnet broadcast
static const uint16_t DST_PORT = 5001;

KSZ9477     sw;
EthernetUDP udp;

void setup()
{
  pinMode(LED_R1, OUTPUT); pinMode(LED_G1, OUTPUT);
  pinMode(LED_R2, OUTPUT); pinMode(LED_G2, OUTPUT);

  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println();
  Serial.println("=== KSZ9477 UDP Sender ===");

  sw.setPins(KSZ_SCK, KSZ_MISO, KSZ_MOSI, KSZ_CS, KSZ_RST, KSZ_INT);
  if (!sw.begin()) {
    Serial.println("KSZ did not respond.");
    digitalWrite(LED_R1, HIGH);
    while (1) {}
  }
  Serial.print("Chip ID   : 0x"); Serial.println(sw.chipId(), HEX);

  sw.configureCpuPort();
  sw.startSwitch();
  sw.configureLeds(KSZ9477::LED_DUAL_TRICOLOR);

#if FORCE_100_MBPS
  for (uint8_t p = 1; p <= 5; p++) capPortTo100Mbps(sw, p);
  Serial.println("PHY ports 1..5 capped to 100 Mbps FD");
#endif

  Ethernet.begin(mac, ip, gw, gw, mask);
  udp.begin(DST_PORT);      // needed to allocate a pcb for sending
  sw.reinitSpi();           // HAL_ETH_MspInit may re-mux PA5/PA6

  Serial.print("Sending from ");
  Serial.print(Ethernet.localIP());
  Serial.print(" -> ");
  Serial.print(dstIp);
  Serial.print(":");
  Serial.println(DST_PORT);
}

void loop()
{
  static uint32_t lastSend = 0;
  static uint32_t counter  = 0;
  uint32_t now = millis();

  if (now - lastSend >= 1000) {
    lastSend = now;
    counter++;

    char msg[64];
    int n = snprintf(msg, sizeof(msg), "hello #%lu t=%lums\n",
                     (unsigned long)counter, (unsigned long)now);

    udp.beginPacket(dstIp, DST_PORT);
    udp.write((const uint8_t*)msg, n);
    int ok = udp.endPacket();

    digitalWrite(LED_G1, !digitalRead(LED_G1));
    Serial.print("[tx] "); Serial.print(ok ? "OK " : "FAIL ");
    Serial.print(n); Serial.print(" B: ");
    Serial.print(msg);
  }

  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 250) {
    lastBeat = now;
    digitalWrite(LED_R2, !digitalRead(LED_R2));
  }
}
