/*
  KSZ9477.h — Arduino driver for the Microchip KSZ9477 managed Ethernet switch.

  This is part of the STM32 CornellRacing Ethernet library. A single header
  bundle: including this pulls in <STM32Ethernet.h> (TCP/IP + LwIP) as well,
  so sketches only need one #include to get the full stack up.

  Usage pattern:

      #include <KSZ9477.h>

      KSZ9477 sw;
      EthernetUDP udp;

      void setup() {
        sw.setPins(PA5, PA6, PD7, PB2, PE7, PB0);  // sck,miso,mosi,cs,rst,int
        if (!sw.begin()) { while (1) {} }           // chip not responding
        sw.configureCpuPort();                      // port 6 RMII 100 FD, KSZ drives REFCLKO
        sw.startSwitch();

        Ethernet.begin(mac, ip);
        udp.begin(5000);
      }

  Notes:
    - Pin map is fully runtime-configurable via setPins(). You are *not*
      locked into any particular STM32 pinout.
    - CS is a software-driven GPIO (not an SPI hardware NSS).
    - The reset pin performs a hardware reset (RST_N pulse). The chip has
      ~100 ms of POR boot time that this driver honours.
    - The interrupt pin is optional. If omitted, hasInterrupt() always
      returns false; you can still poll globalInterruptStatus() manually.
*/

#pragma once

#include <Arduino.h>
#include <SPI.h>

// Pull in the TCP/IP API so users only need `#include <KSZ9477.h>` for both
// switch control and network I/O.
#include <STM32Ethernet.h>

class KSZ9477 {
public:
  // --------- enums ----------------------------------------------------------
  enum Interface : uint8_t {
    IF_RGMII = 0,
    IF_RMII  = 1,
    IF_MII   = 2,
  };
  enum Speed : uint8_t {
    SPEED_10   = 0,
    SPEED_100  = 1,
    SPEED_1000 = 2,
  };
  enum Duplex : uint8_t {
    DUPLEX_HALF = 0,
    DUPLEX_FULL = 1,
  };
  // Per-port LED behavior (MMD 2, register 0x00, bit 4).
  //   SINGLE        = default in this driver. LEDx_1 = Link (any speed),
  //                   LEDx_0 = Activity. Matches magjacks with separate
  //                   Link and Activity LEDs (KSZ9477S Table 4-3).
  //   DUAL_TRICOLOR = LEDx_1 = 1000/10 link+activity,
  //                   LEDx_0 = 100/10 link+activity.
  //                   Matches magjacks with a green+yellow speed/activity
  //                   LED pair (KSZ9477S Table 4-4). This is the chip's
  //                   hardware default, but many hobby magjacks are wired
  //                   for the Link/Act split, so the driver defaults to
  //                   SINGLE.
  enum LedMode : uint8_t {
    LED_DUAL_TRICOLOR = 0,
    LED_SINGLE        = 1,
  };

  // --------- construction ---------------------------------------------------
  // Pass in a specific SPIClass instance if you are not using the default
  // Arduino `SPI` (e.g. on multi-SPI boards). The CS pin is software GPIO.
  explicit KSZ9477(SPIClass &spi = SPI) : _spi(spi) {}

  // --------- configuration (call before begin) ------------------------------
  // All pins are project-configurable. Pass PNUM_NOT_DEFINED for `interrupt`
  // if you haven't wired the KSZ INTRP_N to the MCU.
  // Pin parameters are `uint32_t` — the STM32duino core's pin number type.
  // (ArduinoCore-API's `pin_size_t` isn't defined on this core.)
  void setPins(uint32_t sck,
               uint32_t miso,
               uint32_t mosi,
               uint32_t cs,
               uint32_t reset,
               uint32_t interrupt = PNUM_NOT_DEFINED);

  // SPI bit clock. Datasheet max is 50 MHz; 10 MHz is a safe default.
  void setSpiClock(uint32_t hz) { _spiHz = hz; }

  // --------- lifecycle ------------------------------------------------------
  // Configures CS/RST as outputs, pulses RST_N, spins up SPI on the chosen
  // pins, and reads the chip ID. Returns true if the chip responded with the
  // KSZ9477 signature (family 0x9477).
  bool begin();

  // Hardware reset: RST_N low for 10 ms, then 100 ms wait for POR. Any
  // previously programmed register state is lost — caller must redo
  // configureCpuPort() / startSwitch() / etc. afterward.
  void reset();

  // Re-assert the SPI pin mux and CS GPIO without touching the KSZ chip.
  // Use this after Ethernet.begin() on STM32 variants where HAL_ETH_MspInit
  // iterates PinMap_Ethernet and silently reconfigures pins that overlap
  // SPI1 (e.g. some H7 variants list PA5 / PA6 as MII_CK / MII_RX_ER in
  // their ETH pinmap even in RMII mode — after Ethernet init those pins
  // switch to AF11 and stop clocking SPI). Chip register state is preserved
  // (no hardware reset, no configureLeds / configureCpuPort redo needed).
  void reinitSpi();

  // Global Switch Control 0 (0x0300) bit 0. Must be set for frames to flow.
  void startSwitch();
  void stopSwitch();

  // --------- CPU (port 6) configuration -------------------------------------
  // Programs the port-6 MII/RMII/RGMII interface. Defaults match the most
  // common MAC-to-MAC case: RMII, 100 Mbps, full duplex, KSZ9477 drives the
  // 50 MHz reference clock out on REFCLKO6 (the host MCU's RMII REF_CLK).
  bool configureCpuPort(Interface iface       = IF_RMII,
                        Speed     speed       = SPEED_100,
                        Duplex    duplex      = DUPLEX_FULL,
                        bool      kszDrivesRefClk = true);

  // --------- per-port control ----------------------------------------------
  // Ports 1..5 are PHY (front-panel RJ45), 6 is the CPU port, 7 is SGMII.
  // enable/disable drive the Port MSTP State Register TX+RX enable bits.
  void enablePort(uint8_t port);
  void disablePort(uint8_t port);

  // PHY link status from the Basic Status Register (BMSR, bit 2). Only
  // meaningful for ports 1..5 (the ports with internal PHYs).
  // Double-reads internally to get past the IEEE 802.3 latch-low quirk.
  bool linkUp(uint8_t port);

  // Cap the link speed advertised by a single PHY port during auto-negotiation.
  // `maxMbps` must be 10, 100, or 1000. Auto-neg still runs — the advertisement
  // is just restricted so the link partner picks a speed at or below the cap.
  // Touches PHY reg 9 (1000BASE-T Control, 0xN112) and reg 4 (ANAR, 0xN108),
  // then restarts auto-neg via BMCR (reg 0, 0xN100) bit 9. Datasheet section
  // 4.1 auto-negotiation and the PHY register map in section 5.2.
  //
  // 10-FD/HD advertisement is always left on so there is always a fallback
  // speed the partner can negotiate to.
  void setPortSpeedCap(uint8_t port, uint16_t maxMbps);

  // Convenience: apply setPortSpeedCap() to all five PHY ports (1..5).
  void setSpeedCap(uint16_t maxMbps);

  // --------- LED control ---------------------------------------------------
  // Program the magjack LEDs on all 5 PHY ports to a known-good default
  // (Single-LED mode: LEDx_1 = Link, LEDx_0 = Activity). Also clears the
  // LED Override (0x0120) and LED2 Source (0x0128) registers so every LED
  // pin is guaranteed to be acting as an LED (not a GPO or a PTP trigger
  // output). Called automatically from begin().
  //
  // Hardware note: KSZ LED outputs are active-low, 8 mA open-drain-ish. Wire
  // the LED anode to VDDIO via a 220-470 Ω resistor, cathode to the LEDx_y
  // pin.
  void configureLeds(LedMode mode = LED_SINGLE);

  // Change the LED mode for a single PHY port (1..5). Use configureLeds()
  // if you just want "default for all magjacks".
  void setPortLedMode(uint8_t port, LedMode mode);

  // ---- LED diagnostics (polarity / wiring test) --------------------------
  // Force a single LED pin into GPO mode and drive it HIGH or LOW. Lets you
  // prove polarity: if the LED lights when `level=LOW`, the board is wired
  // active-low (correct for KSZ). If it lights when `level=HIGH`, the board
  // is wired active-high and either the wiring is swapped or you need to
  // stay in GPO mode with inverted drive as a software workaround.
  //
  //   port     : 1..5 (PHY port number)
  //   ledIndex : 0 (LEDx_0, Activity in Single-LED mode) or
  //              1 (LEDx_1, Link in Single-LED mode)
  //   level    : HIGH or LOW
  //
  // Use ledRelease() to hand control back to the PHY's LED function.
  void ledForce(uint8_t port, uint8_t ledIndex, uint8_t level);
  void ledRelease(uint8_t port, uint8_t ledIndex);

  // Raw access to the two switch-level LED registers. Mostly useful for
  // printf-style debugging: 0x0120 = LED Override (per-pin GPO enable),
  // 0x0124 = LED Output (per-pin GPO level when in GPO mode).
  uint32_t ledOverrideRegister()  { return readReg32(0x0120); }
  uint32_t ledOutputRegister()    { return readReg32(0x0124); }

  // --------- diagnostics ----------------------------------------------------
  // hasInterrupt() returns true if the INT pin is currently asserted low.
  // Requires an interrupt pin to have been configured via setPins(); otherwise
  // always returns false.
  bool hasInterrupt();

  // Raw reads of the Global Interrupt Status Register (0x0010) and the
  // Port Interrupt Status Register (0x{port}01B). Clear-on-read semantics
  // follow the datasheet per-bit.
  uint8_t globalInterruptStatus();
  uint8_t portInterruptStatus(uint8_t port);

  // Chip identification
  uint16_t chipId();        // 0x9477 on a healthy KSZ9477
  uint8_t  revision();

  // --------- raw register escape hatch -------------------------------------
  // For anything not wrapped by the class above. 24-bit byte-addressed
  // register map per the KSZ9477 datasheet.
  uint8_t  readReg8(uint32_t addr);
  uint16_t readReg16(uint32_t addr);
  uint32_t readReg32(uint32_t addr);
  void     writeReg8(uint32_t addr, uint8_t val);
  void     writeReg16(uint32_t addr, uint16_t val);
  void     writeReg32(uint32_t addr, uint32_t val);

  // PHY MMD (MDIO-Manageable Device) indirect register access. Used for
  // registers that live behind Clause-45 MMD space — notably the per-port
  // LED Mode register at MMD device 2 / register 0. Sequence is the 4-write
  // portal pattern described in KSZ9477S datasheet section 5.4.
  //   port : 1..5
  //   dev  : MMD device address (0..31)
  //   reg  : MMD register address (0..65535)
  void     writePhyMmd(uint8_t port, uint8_t dev, uint16_t reg, uint16_t val);
  uint16_t readPhyMmd (uint8_t port, uint8_t dev, uint16_t reg);

private:
  // 32-bit SPI command phase: [op:3][addr:24][zero:5]
  static constexpr uint32_t KSZ_OP_WR = 0x40000000UL;   // 010 << 29
  static constexpr uint32_t KSZ_OP_RD = 0x60000000UL;   // 011 << 29

  SPIClass  &_spi;
  uint32_t   _spiHz = 10000000UL;                       // 10 MHz default

  uint32_t _sck   = PNUM_NOT_DEFINED;
  uint32_t _miso  = PNUM_NOT_DEFINED;
  uint32_t _mosi  = PNUM_NOT_DEFINED;
  uint32_t _cs    = PNUM_NOT_DEFINED;
  uint32_t _reset = PNUM_NOT_DEFINED;
  uint32_t _intr  = PNUM_NOT_DEFINED;

  bool       _pinsSet = false;

  // Multi-byte transfer helpers. Internal; most callers go through the
  // readRegN / writeRegN API above.
  void readBytes (uint32_t addr, uint8_t *buf, size_t n);
  void writeBytes(uint32_t addr, const uint8_t *buf, size_t n);
};
