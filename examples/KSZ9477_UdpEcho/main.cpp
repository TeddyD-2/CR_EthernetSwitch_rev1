/*
  KSZ9477_UdpEcho — minimal end-to-end example for the Cornell Racing
  STM32H753VIT6 + KSZ9477 board.

  Brings up the KSZ9477 over SPI, programs the port-6 RMII link, starts
  the STM32Ethernet stack with a static IP, and runs a UDP echo server
  on port 5000.

  How to test (PC at 192.168.1.50/24, plugged into any KSZ front port):
      ping 192.168.1.100
      python -c "import socket;s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM);
                 s.sendto(b'hello',('192.168.1.100',5000));s.settimeout(2);print(s.recv(512))"
*/

#include <Arduino.h>
#include <KSZ9477.h>     // also pulls in STM32Ethernet + LwIP

// ---- KSZ9477 SPI + control pins -------------------------------------------
// SPI1 (MOSI remapped to PD7 because PA7 is ETH_CRS_DV)
#define KSZ_SCK    PA5
#define KSZ_MISO   PA6
#define KSZ_MOSI   PD7
#define KSZ_CS     PB2
#define KSZ_RST    PE7
#define KSZ_INT    PB0

// ---- User LEDs (board-specific) -------------------------------------------
#define LED_R1     PD1
#define LED_G1     PD2
#define LED_R2     PD3
#define LED_G2     PD4

// ---- Network --------------------------------------------------------------
static uint8_t   mac[] = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };
static IPAddress ip   (192, 168, 1, 100);
static IPAddress gw   (192, 168, 1,   1);
static IPAddress mask (255, 255, 255, 0);
static const uint16_t LOCAL_PORT = 5000;

KSZ9477       sw;
EthernetUDP   udp;
static uint8_t packetBuf[512];

void setup()
{
  pinMode(LED_R1, OUTPUT); pinMode(LED_G1, OUTPUT);
  pinMode(LED_R2, OUTPUT); pinMode(LED_G2, OUTPUT);
  digitalWrite(LED_R1, LOW); digitalWrite(LED_G1, LOW);
  digitalWrite(LED_R2, LOW); digitalWrite(LED_G2, LOW);

  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println();
  Serial.println("=== KSZ9477 UDP Echo (minimal) ===");

  // --- Bring up the switch ---
  sw.setPins(KSZ_SCK, KSZ_MISO, KSZ_MOSI, KSZ_CS, KSZ_RST, KSZ_INT);
  if (!sw.begin()) {
    Serial.println("KSZ did not respond.");
    digitalWrite(LED_R1, HIGH);
    while (1) {}
  }
  Serial.print("Chip ID   : 0x"); Serial.println(sw.chipId(), HEX);

  sw.configureCpuPort();   // RMII 100 FD, KSZ drives REFCLKO6
  sw.startSwitch();

  sw.configureLeds(KSZ9477::LED_SINGLE);

  Serial.print("LED Override (0x0120): 0x"); Serial.println(sw.ledOverrideRegister(), HEX);
  Serial.print("LED Output   (0x0124): 0x"); Serial.println(sw.ledOutputRegister(), HEX);
  for (uint8_t p = 1; p <= 5; p++) {
    uint16_t m = sw.readPhyMmd(p, /*dev*/ 2, /*reg*/ 0x0000);
    Serial.print("  P"); Serial.print(p);
    Serial.print(" MMD2/0 = 0x"); Serial.print(m, HEX);
    Serial.print(((m & 0x0010) ? "  (Single-LED OK)" : "  (*** NOT Single-LED ***)"));
    Serial.println();
  }

  uint16_t bmcr  = sw.readReg16(0x5100);
  uint16_t bmsr  = sw.readReg16(0x5102);
  uint16_t phy1f = sw.readReg16(0x511E);
  uint8_t  spd   = (phy1f >> 4) & 0x03;
  Serial.print("P5 BMCR=0x");  Serial.print(bmcr, HEX);
  Serial.print(" BMSR=0x");    Serial.print(bmsr, HEX);
  Serial.print(" PHYCTRL=0x"); Serial.print(phy1f, HEX);
  Serial.print("  link=");     Serial.print((bmsr & 0x0004) ? "UP" : "DOWN");
  Serial.print(" speed=");
  Serial.println(spd == 2 ? "1000" : spd == 1 ? "100" : spd == 0 ? "10" : "?");

  Serial.println("About to call Ethernet.begin() ...");

  Ethernet.begin(mac, ip, /*dns*/ gw, gw, mask);
  udp.begin(LOCAL_PORT);

  // HAL_ETH_MspInit may re-mux PA5/PA6 — re-assert SPI1 pin mux.
  sw.reinitSpi();

  Serial.println("After Ethernet.begin():");
  Serial.print("  chipId = 0x"); Serial.println(sw.chipId(), HEX);
  for (uint8_t p = 1; p <= 5; p++) {
    uint16_t m = sw.readPhyMmd(p, 2, 0);
    Serial.print("  P"); Serial.print(p);
    Serial.print(" MMD2/0 = 0x"); Serial.println(m, HEX);
  }

  Serial.print("Listening on udp://");
  Serial.print(Ethernet.localIP());
  Serial.print(":");
  Serial.println(LOCAL_PORT);
  Serial.println("Ready - try `ping 192.168.1.100` now.");
}

void loop()
{
  uint32_t now = millis();

  static const uint32_t UDP_HOLD_MS = 1000;
  static uint32_t       udpHoldUntil = 0;
  bool udpActive = (int32_t)(udpHoldUntil - now) > 0;

  digitalWrite(LED_G1, udpActive ? HIGH : LOW);

  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 250) {
    lastBeat = now;
    digitalWrite(LED_R2, !digitalRead(LED_R2));
  }

  static uint32_t lastReapply = 0;
  if (now - lastReapply >= 2000) {
    lastReapply = now;
    sw.setPortLedMode(5, KSZ9477::LED_DUAL_TRICOLOR);

    uint16_t bmsr5 = sw.readReg16(0x5102);
    uint16_t phy1f = sw.readReg16(0x511E);
    uint16_t m     = sw.readPhyMmd(5, 2, 0);
    Serial.print("[p5] BMSR=0x");     Serial.print(bmsr5, HEX);
    Serial.print(" PHYCTRL=0x");      Serial.print(phy1f, HEX);
    Serial.print(" link=");           Serial.print((bmsr5 & 4) ? "UP" : "DN");
    Serial.print(" MMD2/0=0x");       Serial.println(m, HEX);
  }

  int size = udp.parsePacket();
  if (size > 0) {
    int n = udp.read(packetBuf, sizeof(packetBuf));
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(packetBuf, n);
    udp.endPacket();
    udpHoldUntil = now + UDP_HOLD_MS;

    Serial.print("[udp] ");
    Serial.print(n);
    Serial.print(" B from ");
    Serial.print(udp.remoteIP());
    Serial.print(":");
    Serial.println(udp.remotePort());
  }
}
