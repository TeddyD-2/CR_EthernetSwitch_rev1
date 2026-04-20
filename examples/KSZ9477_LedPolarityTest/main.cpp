/*
  KSZ9477_LedPolarityTest

  Full LED + PHY diagnostic sketch for the Cornell Racing STM32H753VIT6 +
  KSZ9477 board. Runs phases in order:

    1. Prints per-port MMD 2/register 0 to confirm every PHY is in Single-LED
       mode (value 0x11 = bit 4 set + reserved bits[3:0]=0b0001).
    2. Prints raw BMSR on every port.
    3. CABLE UNPLUGGED observation window.
    4. LINK POLLING window.
    5. POLARITY WALK on one port.
*/

#include <Arduino.h>
#include <KSZ9477.h>

#define KSZ_SCK    PA5
#define KSZ_MISO   PA6
#define KSZ_MOSI   PD7
#define KSZ_CS     PB2
#define KSZ_RST    PE7
#define KSZ_INT    PB0

static const uint8_t WALK_PORT = 5;

KSZ9477 sw;

static uint16_t readBmsr(uint8_t port)
{
  uint32_t addr = (uint32_t)port * 0x1000UL + 0x0102UL;
  (void)sw.readReg16(addr);
  return sw.readReg16(addr);
}

static void announce(uint8_t port, uint8_t led, uint8_t level)
{
  Serial.print("Port "); Serial.print(port);
  Serial.print(" LED"); Serial.print(led);
  Serial.print("  pin=");
  Serial.print(level == HIGH ? "HIGH" : "LOW ");
  Serial.print("  -> LED should be ");
  Serial.println(level == LOW ? "ON " : "OFF");
}

void setup()
{
  Serial.begin(115200);
  while (!Serial && millis() < 3000) {}

  sw.setPins(KSZ_SCK, KSZ_MISO, KSZ_MOSI, KSZ_CS, KSZ_RST, KSZ_INT);
  if (!sw.begin()) {
    Serial.println("KSZ did not respond. Check SPI wiring / RST pulse.");
    while (1) {}
  }

  Serial.println();
  Serial.println("=== KSZ9477 LED / PHY diagnostic ===");
  Serial.print("Chip ID   : 0x"); Serial.println(sw.chipId(), HEX);
  Serial.print("Revision  : 0x"); Serial.println(sw.revision(), HEX);
  Serial.print("LED Override (0x0120): 0x");
  Serial.println(sw.ledOverrideRegister(), HEX);
  Serial.print("LED Output   (0x0124): 0x");
  Serial.println(sw.ledOutputRegister(), HEX);
  Serial.println("(Both should be 0 - configureLeds() cleared them.)");

  Serial.println();
  Serial.println("---- [Phase 1] Per-port LED mode (MMD 2 / reg 0) ----");
  for (uint8_t p = 1; p <= 5; p++) {
    uint16_t v = sw.readPhyMmd(p, 2, 0);
    Serial.print("  P"); Serial.print(p);
    Serial.print(" MMD 2/0 = 0x");
    Serial.print(v, HEX);
    Serial.print("  -> ");
    if ((v & 0x0010) == 0x0010) Serial.println("Single-LED mode OK");
    else                        Serial.println("*** NOT Single-LED ***");
  }

  Serial.println();
  Serial.println("---- [Phase 2] Raw BMSR (cable may be unplugged) ----");
  Serial.println("Fresh PHY, no link: 0x7949 is the expected value.");
  for (uint8_t p = 1; p <= 5; p++) {
    uint16_t bmsr = readBmsr(p);
    Serial.print("  P"); Serial.print(p);
    Serial.print(" BMSR = 0x");
    Serial.print(bmsr, HEX);
    Serial.print("  link=");
    Serial.println((bmsr & 0x0004) ? "UP" : "down");
  }

  Serial.println();
  Serial.println("---- [Phase 3] CABLE UNPLUGGED (8 s) ----");
  Serial.println("Unplug the Ethernet cable now. Watch the magjack LEDs.");
  Serial.println("Expected: BOTH LEDs OFF on every port.");
  delay(8000);

  Serial.println();
  Serial.println("---- [Phase 4] Plug cable in - polling link for 20 s ----");
  uint32_t t0 = millis();
  bool seenUp[6] = { false, false, false, false, false, false };
  while (millis() - t0 < 20000) {
    delay(250);
    for (uint8_t p = 1; p <= 5; p++) {
      bool up = (readBmsr(p) & 0x0004) != 0;
      if (up && !seenUp[p]) {
        seenUp[p] = true;
        Serial.print("  *** P"); Serial.print(p);
        Serial.print(" LINK UP at t=");
        Serial.print((millis() - t0) / 1000.0f, 2);
        Serial.println(" s ***");
      }
    }
  }

  Serial.println();
  Serial.print("---- [Phase 5] LED polarity walk on port ");
  Serial.print(WALK_PORT);
  Serial.println(" ----");
  const uint32_t HOLD_MS = 1500;

  for (int cycle = 0; cycle < 2; cycle++) {
    Serial.print("-- cycle "); Serial.print(cycle + 1); Serial.println(" --");

    for (uint8_t led = 0; led <= 1; led++) {
      announce(WALK_PORT, led, LOW);
      sw.ledForce(WALK_PORT, led, LOW);
      delay(HOLD_MS);

      announce(WALK_PORT, led, HIGH);
      sw.ledForce(WALK_PORT, led, HIGH);
      delay(HOLD_MS);

      sw.ledRelease(WALK_PORT, led);
    }

    Serial.println("Back to PHY control for a sec...");
    delay(HOLD_MS);
  }

  Serial.println();
  Serial.println("Done. Final register state:");
  Serial.print("  LED Override: 0x"); Serial.println(sw.ledOverrideRegister(), HEX);
  Serial.print("  LED Output  : 0x"); Serial.println(sw.ledOutputRegister(), HEX);
}

void loop() {}
