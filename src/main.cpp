/*
  CR_EthernetSwitch_rev1 — starter sketch.

  Placeholder: mirrors examples/KSZ9477_UdpEcho so a fresh flash gets you
  a working UDP echo on 192.168.1.100:5000. Rewrite freely.
*/

#include <Arduino.h>
#include <KSZ9477.h>

#define KSZ_SCK    PA5
#define KSZ_MISO   PA6
#define KSZ_MOSI   PD7
#define KSZ_CS     PB2
#define KSZ_RST    PE7
#define KSZ_INT    PB0

#define LED_R1     PD1
#define LED_G1     PD2
#define LED_R2     PD3
#define LED_G2     PD4

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

  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  Serial.println("=== CR_EthernetSwitch_rev1 starter ===");

  sw.setPins(KSZ_SCK, KSZ_MISO, KSZ_MOSI, KSZ_CS, KSZ_RST, KSZ_INT);
  if (!sw.begin()) {
    Serial.println("KSZ did not respond.");
    digitalWrite(LED_R1, HIGH);
    while (1) {}
  }

  sw.configureCpuPort();
  sw.startSwitch();
  sw.configureLeds(KSZ9477::LED_SINGLE);

  Ethernet.begin(mac, ip, gw, gw, mask);
  udp.begin(LOCAL_PORT);
  sw.reinitSpi();

  Serial.print("Listening on udp://");
  Serial.print(Ethernet.localIP());
  Serial.print(":");
  Serial.println(LOCAL_PORT);
}

void loop()
{
  uint32_t now = millis();

  static uint32_t lastBeat = 0;
  if (now - lastBeat >= 250) {
    lastBeat = now;
    digitalWrite(LED_R2, !digitalRead(LED_R2));
  }

  int size = udp.parsePacket();
  if (size > 0) {
    int n = udp.read(packetBuf, sizeof(packetBuf));
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(packetBuf, n);
    udp.endPacket();

    digitalWrite(LED_G1, HIGH);
    digitalWrite(LED_G2, HIGH);
    Serial.print("[udp] "); Serial.print(n);
    Serial.print(" B from "); Serial.print(udp.remoteIP());
    Serial.print(":"); Serial.println(udp.remotePort());
  } else {
    digitalWrite(LED_G1, LOW);
    digitalWrite(LED_G2, LOW);
  }
}
