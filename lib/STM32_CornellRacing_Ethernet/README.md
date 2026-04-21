# STM32 CornellRacing Ethernet

Arduino library bundling everything the Cornell Racing **STM32H753VIT6 + KSZ9477** board needs to talk Ethernet:

1. **`KSZ9477.h`** — driver for the Microchip KSZ9477 managed switch over SPI (chip-ID check, hardware reset, port-6 RMII bring-up, per-port enable/disable, link status, LED control, interrupts, raw register escape hatch). Pin map is fully runtime-configurable via `setPins()`.
2. **`STM32Ethernet.h`** — fork of [stm32duino/STM32Ethernet](https://github.com/stm32duino/STM32Ethernet) with a second low-level driver targeted at STM32H7 in **MAC-to-MAC** mode (no MDIO; the KSZ drives the RMII REFCLK and forces the link). The legacy F4/F7 driver is preserved unchanged behind `#if !defined(STM32H7xx)`, so non-H7 boards still build.

Including `KSZ9477.h` transitively pulls in the TCP/IP stack, so sketches only need a single `#include`.

## Quick start

```cpp
#include <KSZ9477.h>     // also pulls in STM32Ethernet + LwIP

KSZ9477       sw;
EthernetUDP   udp;
static uint8_t mac[] = { 0x02, 0x00, 0x11, 0x22, 0x33, 0x44 };
static IPAddress ip(192, 168, 1, 100);

void setup() {
  // sck, miso, mosi, cs, reset, [interrupt]
  sw.setPins(PA5, PA6, PD7, PB2, PE7, PB0);
  if (!sw.begin()) { while (1) {} }   // chip didn't respond
  sw.configureCpuPort();              // RMII 100 FD, KSZ drives REFCLKO6
  sw.startSwitch();

  Ethernet.begin(mac, ip);
  udp.begin(5000);
}

void loop() {
  if (udp.parsePacket() > 0) {
    char buf[256];
    int n = udp.read(buf, sizeof(buf));
    udp.beginPacket(udp.remoteIP(), udp.remotePort());
    udp.write(buf, n);
    udp.endPacket();
  }
}
```

See `examples/` for two complete sketches:

- **`KSZ9477_UdpEcho/`** — minimal UDP echo server on port 5000 with link/heartbeat LEDs.
- **`KSZ9477_LedPolarityTest/`** — diagnostic walk for the magjack LED wiring (Single-LED mode readback, BMSR per port, link polling, GPO polarity sweep).

## Board package

The board variant + linker script live alongside this library under `board/variants/H753VIT6_Custom/` in the parent PlatformIO project. `platformio.ini` wires them in via `board_build.variants_dir` and `board_build.ldscript`; no install step.

## STM32H7 + KSZ9477 driver notes

The H7 driver lives in `src/utility/ethernetif_h7.cpp` and is selected automatically when building for an STM32H7 target.

### Topology

- **Clocking** — The KSZ produces the 50 MHz RMII REFCLK on `REFCLKO6`. The STM32 consumes it on `PA1` (`ETH_REF_CLK`). No MCO hack is needed.
- **RMII wiring** — Straight (not crossover): `STM32 TXD0/TXD1/TX_EN` → `KSZ RXD0/RXD1/CRS_DV`; `KSZ TXD0/TXD1/TX_EN` → `STM32 RXD0/RXD1/CRS_DV`. Series-terminate all four data lines with 22–33 Ω.
- **No MDIO** — the STM32 never talks PHY. Don't wire MDC/MDIO from STM32 to the KSZ.

### KSZ port-6 register sequence (handled by `KSZ9477::configureCpuPort()` + `startSwitch()`)

| Reg    | Value                          | Meaning                            |
|--------|--------------------------------|------------------------------------|
| 0x6301 | `(existing & ~0x07) \| 0x01`   | Port-6 XMII_CTRL_1: RMII, KSZ drives REFCLK |
| 0x6300 | `existing \| 0x50`             | Port-6 XMII_CTRL_0: 100 Mbps + FD  |
| 0x{P}B04 | `existing \| 0x06`           | Per-port MSTP State: TX+RX enable  |
| 0x0300 | `0x01`                         | Global switch enable (START_SWITCH)|

### DMA buffer placement

DMA descriptors and RX/TX buffers must live in D2 SRAM at `0x30040000`, marked non-cacheable. The custom variant's `ldscript.ld` provides the `.lwip_sec` block; the driver's `ethernetif_h7_mpu_config()` (declared `__weak`) configures the matching MPU region. Both are mandatory and both are handled for you when you select the Cornell Racing board.

If you need to override the MPU config (e.g. you already use D2 SRAM for other DMAs), provide your own non-weak `ethernetif_h7_mpu_config()` — but keep the D2SRAM3 clock enabled and the first 64 KB at `0x30040000` non-cacheable.

### Forced speed / duplex

Define before including `STM32Ethernet.h` (or as `-D` entries in `platformio.ini → build_flags`) to override the defaults:

```cpp
#define ETHERNETIF_H7_FORCED_SPEED   ETH_SPEED_10M       // default: ETH_SPEED_100M
#define ETHERNETIF_H7_FORCED_DUPLEX  ETH_HALFDUPLEX_MODE // default: ETH_FULLDUPLEX_MODE
```

The KSZ port-6 settings written by `configureCpuPort()` must match.

## LwIP configuration

LwIP options come from `lwipopts_default.h` in the library. Override either by:

- adding a `STM32lwipopts.h` to your sketch (full replacement), or
- adding a `lwipopts_extra.h` (extends the defaults).

An idle task drives the LwIP stack from a 1 ms timer callback (`stm32_eth_scheduler()`). The default timer is `TIM14` and can be redefined per-variant via `DEFAULT_ETHERNET_TIMER`. Don't lock the system in IRQ-disabled regions for long; if you need a manual tick, call `Ethernet::schedule()`.

## Bring-up checklist

1. `pio run -e example_udpecho -t upload`. Serial should print `Chip ID : 0x9477` and `Listening on udp://192.168.1.100:5000`.
2. From a PC on `192.168.1.50/24` plugged into any KSZ front port, `ping 192.168.1.100` should reply within 2–3 s of boot.
3. Send a UDP packet to port 5000 — the sketch echoes it back and pulses the green status LEDs.

If LEDs misbehave, run `examples/KSZ9477_LedPolarityTest` — it prints per-port LED-mode register state, BMSR for each PHY, watches link-up timing, and walks LED polarity so you can confirm magjack wiring.

## Gotchas

- **`HAL_ETH_SetMACConfig`** must be called between `HAL_ETH_Init` and `HAL_ETH_Start`. The driver does this; if you add your own MAC reconfig after link-up it will silently no-op.
- **DTCM is off-limits** for ETH buffers (`0x20000000`). The ETH DMA can't reach it.
- **D2 SRAM clock + MPU** are both mandatory for H7 — overriding one without the other will drop you into either silent corruption or a hard fault.

## Credits

Forked from [stm32duino/STM32Ethernet](https://github.com/stm32duino/STM32Ethernet) (LGPL-2.1+). Upstream author list in `AUTHORS`. License terms in `LICENSE`.
