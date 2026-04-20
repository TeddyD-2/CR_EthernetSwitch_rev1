# CR_EthernetSwitch_rev1

PlatformIO project for the Cornell Racing **STM32H753VIT6 + KSZ9477** Ethernet switch board.

Single self-contained folder: variant, board definition, library, examples, and firmware all live here. No Arduino IDE, no `boards.local.txt`, no install scripts.

## Layout

```
CR_EthernetSwitch_rev1/
├── platformio.ini                              # build envs + flags
├── board/
│   ├── cr_eth_switch.json                      # PIO board definition
│   └── variants/
│       └── H753VIT6_Custom/                    # variant (owned by this repo)
│           ├── ldscript.ld                     # .lwip_sec at D2 SRAM 0x30040000
│           ├── variant_generic.h               # SPI1 MOSI → PD7
│           ├── variant_generic.cpp
│           ├── PeripheralPins.c
│           ├── PinNamesVar.h
│           └── generic_clock.c
├── src/
│   └── main.cpp                                # your firmware
├── lib/
│   └── STM32_CornellRacing_Ethernet/           # KSZ9477 + STM32Ethernet fork
└── examples/
    ├── KSZ9477_UdpEcho/main.cpp
    └── KSZ9477_LedPolarityTest/main.cpp
```

## Build / flash

```bash
# main firmware (src/main.cpp)
pio run -e firmware -t upload

# examples
pio run -e example_udpecho  -t upload
pio run -e example_ledtest  -t upload

# serial monitor (CDC over USB)
pio device monitor
```

## Board configuration (baked into `platformio.ini`)

- **Upload:** DFU (hold BOOT0 high, reset → enters ST bootloader at VID:PID `0x0483:0xDF11`)
- **USB:** CDC "generic `Serial` supersede U(S)ART" — `Serial` prints to the CDC USB port
- **U(S)ART:** generic Serial enabled (default)
- **USB descriptors:** VID `0x0483`, PID `0x5740`, manufacturer "Cornell Racing", product "CR Ethernet Switch"

Change any of this in `platformio.ini → build_flags`.

## Custom linker + variant

[`board/variants/H753VIT6_Custom/ldscript.ld`](board/variants/H753VIT6_Custom/ldscript.ld) adds:

```ld
.lwip_sec (NOLOAD) :
{
  . = ABSOLUTE(0x30040000);
  KEEP(*(.RxDecripSection))
  KEEP(*(.TxDecripSection))
  KEEP(*(.RxArraySection))
  KEEP(*(.TxArraySection))
} >RAM_D2
```

places STM32Ethernet's DMA descriptors + RX/TX buffers at the start of D2 SRAM3. The ETH MPU region configured by `ethernetif_h7_mpu_config()` in the library maps exactly 64 KB from there as Normal non-cacheable — DMA coherency without `SCB_CleanDCache*` calls.

[`board/variants/H753VIT6_Custom/variant_generic.h`](board/variants/H753VIT6_Custom/variant_generic.h) diverges from stock H753VI only in the SPI1 MOSI pin: stock is `PA7`, but `PA7` is `ETH_CRS_DV` in our RMII pin map, so MOSI is remapped to `PD7`.

## Verifying the build

Run a verbose build and confirm the link uses our ldscript:

```bash
pio run -e firmware -v 2>&1 | grep ldscript
#   ... -Tboard/variants/H753VIT6_Custom/ldscript.ld ...
```

The `.map` should show DMA buffers at `0x30040000`:

```
.lwip_sec       0x30040000     ...
                0x30040000     *fill*
 .RxDecripSection
                0x30040000     ...   ethernetif_h7.cpp.o
```
