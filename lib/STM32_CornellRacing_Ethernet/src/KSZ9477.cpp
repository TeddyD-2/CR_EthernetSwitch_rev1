/*
  KSZ9477.cpp — implementation of the KSZ9477 managed switch driver.

  Register address reference is the Microchip KSZ9477S datasheet
  (DS00002392C). All register numbers in the comments below follow that
  document's convention: port base = port_number * 0x1000 (so port 5 is
  0x5000, port 6 is 0x6000, etc.).
*/

#include "KSZ9477.h"

// ---------------------------------------------------------------------------
// pin / clock configuration
// ---------------------------------------------------------------------------
void KSZ9477::setPins(uint32_t sck,
                      uint32_t miso,
                      uint32_t mosi,
                      uint32_t cs,
                      uint32_t reset,
                      uint32_t interrupt)
{
  _sck      = sck;
  _miso     = miso;
  _mosi     = mosi;
  _cs       = cs;
  _reset    = reset;
  _intr     = interrupt;
  _pinsSet  = true;
}

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
bool KSZ9477::begin()
{
  if (!_pinsSet) {
    // No pin map configured — don't touch SPI or GPIO.
    return false;
  }

  // CS and RST are software GPIOs. Drive CS high (inactive) before anything
  // else so we don't accidentally clock in a half-formed command during
  // reset.
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);

  pinMode(_reset, OUTPUT);
  digitalWrite(_reset, HIGH);

  // Optional INT pin — tied to an open-drain output on the switch, so pull
  // it up internally.
  if (_intr != PNUM_NOT_DEFINED) {
    pinMode(_intr, INPUT_PULLUP);
  }

  // Hardware reset pulse + POR boot delay.
  reset();

  // Bind SPI to the configured pins and bring up the peripheral.
  _spi.setMOSI(_mosi);
  _spi.setMISO(_miso);
  _spi.setSCLK(_sck);
  _spi.begin();

  // Verify the chip responded. CIDER at 0x0000..0x0003 returns
  //   0x00 0x94 0x77 <revision>
  uint16_t hi = readReg16(0x0000);
  uint16_t lo = readReg16(0x0002);

  bool dead = ((hi == 0x0000) && (lo == 0x0000)) ||
              ((hi == 0xFFFF) && (lo == 0xFFFF));
  if (dead) { return false; }

  bool match = (hi == 0x0094) && ((lo & 0xFF00) == 0x7700);
  if (!match) { return false; }

  // Program the PHY LEDs on ports 1..5 to Single-LED mode (LEDx_1 = Link,
  // LEDx_0 = Activity — the typical split-LED magjack convention). Also
  // clears the LED Override and LED2-Source registers to guarantee every
  // LED pin is acting as an LED — so stray strap bits can't leave a pin
  // in GPO mode.
  configureLeds(LED_SINGLE);
  return true;
}

void KSZ9477::reset()
{
  if (_reset == PNUM_NOT_DEFINED) return;

  digitalWrite(_reset, LOW);
  delay(10);
  digitalWrite(_reset, HIGH);
  delay(100);                         // datasheet POR boot: ~100 ms
}

void KSZ9477::reinitSpi()
{
  if (!_pinsSet) return;

  // Close and reopen SPI with the configured pin map. This is the minimum
  // required to undo an AF-change done by HAL_ETH_MspInit on an overlapping
  // pin. CS is re-driven high so a half-formed command can't be clocked in.
  _spi.end();
  _spi.setMOSI(_mosi);
  _spi.setMISO(_miso);
  _spi.setSCLK(_sck);
  _spi.begin();

  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
}

void KSZ9477::startSwitch()
{
  // Global Switch Control 0 (0x0300) bit 0 = START_SWITCH
  uint8_t v = readReg8(0x0300);
  writeReg8(0x0300, v | 0x01);
}

void KSZ9477::stopSwitch()
{
  uint8_t v = readReg8(0x0300);
  writeReg8(0x0300, v & ~0x01);
}

// ---------------------------------------------------------------------------
// port 6 (CPU port) MAC interface
// ---------------------------------------------------------------------------
bool KSZ9477::configureCpuPort(Interface iface,
                               Speed     speed,
                               Duplex    duplex,
                               bool      kszDrivesRefClk)
{
  // XMII Port Control 1 (0x6301):
  //   bits 1:0 = interface type (00 RGMII, 01 RMII, 10/11 MII)
  //   bit  2   = RMII clock direction. 0 = KSZ generates REFCLK on REFCLKO6,
  //              1 = KSZ receives REFCLK at RXC. Default comes from the
  //              RXD6_[3:2] strap, which is per-board — we force it here.
  uint8_t x1 = readReg8(0x6301);
  x1 &= ~0x07;
  x1 |= (iface & 0x03);
  if (!kszDrivesRefClk) { x1 |= 0x04; }
  writeReg8(0x6301, x1);

  // XMII Port Control 0 (0x6300):
  //   bit 6 = MAC Port Duplex      (1 = FD)
  //   bit 4 = MAC Port Speed 10/100 (1 = 100 Mbps)
  // Other bits (flow control, reserved) left at their defaults.
  uint8_t x0 = readReg8(0x6300);
  x0 &= ~((1 << 6) | (1 << 4));
  if (duplex == DUPLEX_FULL)  { x0 |= (1 << 6); }
  if (speed  == SPEED_100)    { x0 |= (1 << 4); }
  writeReg8(0x6300, x0);

  return true;
}

// ---------------------------------------------------------------------------
// per-port enable / link status
// ---------------------------------------------------------------------------
static inline uint32_t portBase(uint8_t port) {
  // Port N registers live at 0xN000..0xNFFF.
  return (uint32_t)port * 0x1000UL;
}

void KSZ9477::enablePort(uint8_t port)
{
  // Port MSTP State Register at 0xNB04:
  //   bit 2 = Port Transmit Enable
  //   bit 1 = Port Receive Enable
  //   bit 0 = Port Learning Disable (leave as-is)
  uint32_t addr = portBase(port) + 0x0B04;
  uint8_t v = readReg8(addr);
  v |= 0x06;
  writeReg8(addr, v);
}

void KSZ9477::disablePort(uint8_t port)
{
  uint32_t addr = portBase(port) + 0x0B04;
  uint8_t v = readReg8(addr);
  v &= ~0x06;
  writeReg8(addr, v);
}

bool KSZ9477::linkUp(uint8_t port)
{
  if (port < 1 || port > 5) return false;

  // PHY Basic Status Register (BMSR) at 0xN102/0xN103. Bit 2 = Link Status.
  // BMSR link-status is latching-low per IEEE 802.3; double-read to clear
  // the latch and get the actual current state.
  uint32_t addr = portBase(port) + 0x0102;
  (void)readReg16(addr);
  uint16_t bmsr = readReg16(addr);
  return (bmsr & 0x0004) != 0;
}

// ---------------------------------------------------------------------------
// Auto-negotiation speed cap (KSZ9477S datasheet section 4.1, Table 5-10)
//
//   PHY Basic Control          (BMCR)  @ 0xN100 — bit 9 = restart AN
//   PHY Auto-Neg Advertisement (ANAR)  @ 0xN108 — bits 8,7 = 100-FD/HD adv
//                                                 bits 6,5 = 10-FD/HD  adv
//   PHY 1000BASE-T Control              @ 0xN112 — bits 9,8 = 1000-FD/HD adv
// ---------------------------------------------------------------------------
void KSZ9477::setPortSpeedCap(uint8_t port, uint16_t maxMbps)
{
  if (port < 1 || port > 5) return;

  const uint32_t base = portBase(port);

  // 1000BASE-T Control: advertise 1000-FD/HD only if cap >= 1000.
  uint16_t ctrl1000 = readReg16(base + 0x0112);
  if (maxMbps >= 1000) ctrl1000 |=  ((1u << 9) | (1u << 8));
  else                 ctrl1000 &= ~((1u << 9) | (1u << 8));
  writeReg16(base + 0x0112, ctrl1000);

  // ANAR: advertise 100-FD/HD only if cap >= 100. Always advertise 10-FD/HD
  // so there is always a fallback speed to negotiate to.
  uint16_t anar = readReg16(base + 0x0108);
  if (maxMbps >= 100) anar |=  ((1u << 8) | (1u << 7));
  else                anar &= ~((1u << 8) | (1u << 7));
  anar |= (1u << 6) | (1u << 5);
  writeReg16(base + 0x0108, anar);

  // Restart auto-negotiation so the new advertisement takes effect.
  uint16_t bmcr = readReg16(base + 0x0100);
  bmcr |= (1u << 9);
  writeReg16(base + 0x0100, bmcr);
}

void KSZ9477::setSpeedCap(uint16_t maxMbps)
{
  for (uint8_t port = 1; port <= 5; port++) {
    setPortSpeedCap(port, maxMbps);
  }
}

// ---------------------------------------------------------------------------
// LED configuration
//
// Background (KSZ9477S datasheet section 4.2, tables 4-3 and 4-4):
//   Each PHY port (1..5) has two LED pins (LEDx_0, LEDx_1). Per-port behavior
//   is one of two presets, chosen by MMD device 2 register 0x00 bit 4:
//
//     Single-LED (bit 4 = 1, DRIVER DEFAULT):           Table 4-3
//        LEDx_1 = Link (any speed, solid)
//        LEDx_0 = Activity (blinks on TX/RX)
//        Matches magjacks with a separate Link LED and Activity LED — the
//        most common hobby/FSAE magjack pinout.
//
//     Tri-Color Dual-LED (bit 4 = 0, chip power-on default): Table 4-4
//        LEDx_1 = 1000 link solid / 1000 activity blink / 10 link solid+other
//        LEDx_0 = 100  link solid / 100  activity blink / 10 link solid+other
//        Both ON together = 10 Mbps link. Matches speed/activity magjacks
//        with a bi-color (green+yellow) LED per port.
//
//   Outputs are active-low. There is no polarity override register.
//
//   Two switch-level registers can *disable* LED function:
//     0x0120 LED Override   — bit N set => pin N becomes a GPO.
//     0x0128 LED2 Source    — bits 2/3 set => LED2 pins reassigned to PTP.
//   We clear both so every LED pin is guaranteed to be acting as an LED.
// ---------------------------------------------------------------------------
void KSZ9477::configureLeds(LedMode mode)
{
  // Force every LED pin to LED-function mode (not GPO, not PTP trigger).
  writeReg32(0x0120, 0x00000000UL);   // LED Override
  writeReg32(0x0128, 0x00000000UL);   // LED2_0/LED2_1 Source

  // Set the per-port LED mode on all 5 PHY ports.
  for (uint8_t port = 1; port <= 5; port++) {
    setPortLedMode(port, mode);
  }
}

void KSZ9477::setPortLedMode(uint8_t port, LedMode mode)
{
  if (port < 1 || port > 5) return;

  // MMD device 2, register 0x00, bit 4 = LED mode.
  //   0 = Tri-Color Dual-LED (default)
  //   1 = Single-LED
  // Lower nibble of this register has a reserved default of 0b0001 per
  // datasheet; we OR that in so we don't trample it.
  uint16_t val = 0x0001;
  if (mode == LED_SINGLE) { val |= (1u << 4); }
  writePhyMmd(port, /*dev*/ 2, /*reg*/ 0x0000, val);
}

// ---- LED diagnostics -------------------------------------------------------
//
// Bit layout of 0x0120 (LED Override) and 0x0124 (LED Output), bits [9:0]:
//
//   bit 0 = LED1_0   bit 1 = LED1_1
//   bit 2 = LED2_0   bit 3 = LED2_1
//   bit 4 = LED3_0   bit 5 = LED3_1
//   bit 6 = LED4_0   bit 7 = LED4_1
//   bit 8 = LED5_0   bit 9 = LED5_1
//
// i.e. bit index for (port P, led L) = (P - 1) * 2 + L, with P in 1..5 and
// L in {0, 1}. 0x0120 bit set => pin is a GPO. 0x0124 bit is the GPO level.
static inline uint8_t ledBitIndex(uint8_t port, uint8_t ledIndex)
{
  return (uint8_t)((port - 1) * 2 + ledIndex);
}

void KSZ9477::ledForce(uint8_t port, uint8_t ledIndex, uint8_t level)
{
  if (port < 1 || port > 5) return;
  if (ledIndex > 1)          return;

  uint32_t mask = (1UL << ledBitIndex(port, ledIndex));

  // Set the level bit in LED Output *before* flipping the override bit, so
  // when the pin switches from LED-function to GPO it lands on the right
  // level rather than glitching through whatever was last latched.
  uint32_t out = readReg32(0x0124);
  if (level) { out |=  mask; }
  else       { out &= ~mask; }
  writeReg32(0x0124, out);

  uint32_t ovr = readReg32(0x0120);
  ovr |= mask;
  writeReg32(0x0120, ovr);
}

void KSZ9477::ledRelease(uint8_t port, uint8_t ledIndex)
{
  if (port < 1 || port > 5) return;
  if (ledIndex > 1)          return;

  uint32_t mask = (1UL << ledBitIndex(port, ledIndex));
  uint32_t ovr  = readReg32(0x0120);
  ovr &= ~mask;
  writeReg32(0x0120, ovr);
}

// ---------------------------------------------------------------------------
// PHY MMD (MDIO-Manageable Device) indirect register access
//
// The KSZ9477 exposes a two-register "portal" per PHY for Clause-45 MMD
// access: PHY MMD Setup at 0xNN1A and PHY MMD Data at 0xNN1C (N = port 1..5).
//
//   Setup register (16-bit):
//     bits [15:14] = MMD Function
//                     00 = address  (Data register selects MMD register addr)
//                     01 = data     (Data register is the value, no autoinc)
//                     10 = data     (Data register is the value, post-inc)
//                     11 = data     (Data register is the value, post-inc wr)
//     bits  [4:0]  = MMD Device Address (0..31)
//
// Per-MMD-write sequence (datasheet 5.4 example):
//   1) Setup  = 0x0000 | dev         ; select "address" mode for this device
//   2) Data   = reg_addr             ; latch the target register's address
//   3) Setup  = 0x4000 | dev         ; switch to "data, no autoinc" mode
//   4) Data   = val                  ; read or write the register
// ---------------------------------------------------------------------------
void KSZ9477::writePhyMmd(uint8_t port, uint8_t dev, uint16_t reg, uint16_t val)
{
  if (port < 1 || port > 5) return;

  uint32_t setupAddr = portBase(port) + 0x011A;
  uint32_t dataAddr  = portBase(port) + 0x011C;

  writeReg16(setupAddr, (uint16_t)(0x0000 | (dev & 0x1F)));
  writeReg16(dataAddr,  reg);
  writeReg16(setupAddr, (uint16_t)(0x4000 | (dev & 0x1F)));
  writeReg16(dataAddr,  val);
}

uint16_t KSZ9477::readPhyMmd(uint8_t port, uint8_t dev, uint16_t reg)
{
  if (port < 1 || port > 5) return 0xFFFF;

  uint32_t setupAddr = portBase(port) + 0x011A;
  uint32_t dataAddr  = portBase(port) + 0x011C;

  writeReg16(setupAddr, (uint16_t)(0x0000 | (dev & 0x1F)));
  writeReg16(dataAddr,  reg);
  writeReg16(setupAddr, (uint16_t)(0x4000 | (dev & 0x1F)));
  return readReg16(dataAddr);
}

// ---------------------------------------------------------------------------
// interrupts / diagnostics
// ---------------------------------------------------------------------------
bool KSZ9477::hasInterrupt()
{
  if (_intr == PNUM_NOT_DEFINED) return false;
  return digitalRead(_intr) == LOW;   // INT is active-low, open-drain
}

uint8_t KSZ9477::globalInterruptStatus()
{
  // Global Interrupt Status Register at 0x0010
  return readReg8(0x0010);
}

uint8_t KSZ9477::portInterruptStatus(uint8_t port)
{
  return readReg8(portBase(port) + 0x001B);
}

uint16_t KSZ9477::chipId()
{
  // CIDER bytes 0x0000..0x0001 hold the 16-bit family ID (0x9477).
  // Return it in the natural 0x9477 form (not the 0x0094 / 0x77xx
  // big-endian split of the two 16-bit halves).
  uint16_t hi = readReg16(0x0000);          // 0x00 | 0x94
  uint16_t lo = readReg16(0x0002);          // 0x77 | rev
  return (uint16_t)(((hi & 0x00FF) << 8) | ((lo & 0xFF00) >> 8));
}

uint8_t KSZ9477::revision()
{
  return readReg8(0x0003);
}

// ---------------------------------------------------------------------------
// raw register access
// ---------------------------------------------------------------------------
uint8_t KSZ9477::readReg8(uint32_t addr)
{
  uint8_t v;
  readBytes(addr, &v, 1);
  return v;
}

uint16_t KSZ9477::readReg16(uint32_t addr)
{
  uint8_t b[2];
  readBytes(addr, b, 2);
  return (uint16_t)((b[0] << 8) | b[1]);
}

uint32_t KSZ9477::readReg32(uint32_t addr)
{
  uint8_t b[4];
  readBytes(addr, b, 4);
  return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
         ((uint32_t)b[2] <<  8) |  (uint32_t)b[3];
}

void KSZ9477::writeReg8(uint32_t addr, uint8_t val)
{
  writeBytes(addr, &val, 1);
}

void KSZ9477::writeReg16(uint32_t addr, uint16_t val)
{
  uint8_t b[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFF) };
  writeBytes(addr, b, 2);
}

void KSZ9477::writeReg32(uint32_t addr, uint32_t val)
{
  uint8_t b[4] = {
    (uint8_t)(val >> 24), (uint8_t)(val >> 16),
    (uint8_t)(val >>  8), (uint8_t)(val & 0xFF),
  };
  writeBytes(addr, b, 4);
}

// ---------------------------------------------------------------------------
// low-level SPI I/O. 32-bit command phase [op:3][addr:24][zero:5] followed
// by N bytes. CS stays asserted for the entire address + data burst.
// ---------------------------------------------------------------------------
void KSZ9477::readBytes(uint32_t addr, uint8_t *buf, size_t n)
{
  uint32_t cmd = KSZ_OP_RD | ((addr & 0xFFFFFFUL) << 5);

  _spi.beginTransaction(SPISettings(_spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer((uint8_t)(cmd >> 24));
  _spi.transfer((uint8_t)(cmd >> 16));
  _spi.transfer((uint8_t)(cmd >>  8));
  _spi.transfer((uint8_t)(cmd      ));
  for (size_t i = 0; i < n; i++) {
    buf[i] = _spi.transfer(0x00);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
}

void KSZ9477::writeBytes(uint32_t addr, const uint8_t *buf, size_t n)
{
  uint32_t cmd = KSZ_OP_WR | ((addr & 0xFFFFFFUL) << 5);

  _spi.beginTransaction(SPISettings(_spiHz, MSBFIRST, SPI_MODE0));
  digitalWrite(_cs, LOW);
  _spi.transfer((uint8_t)(cmd >> 24));
  _spi.transfer((uint8_t)(cmd >> 16));
  _spi.transfer((uint8_t)(cmd >>  8));
  _spi.transfer((uint8_t)(cmd      ));
  for (size_t i = 0; i < n; i++) {
    _spi.transfer(buf[i]);
  }
  digitalWrite(_cs, HIGH);
  _spi.endTransaction();
}
