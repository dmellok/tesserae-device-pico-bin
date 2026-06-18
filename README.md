# tesserae-device-pico-bin

Firmware for driving a **Pimoroni Inky Impression 13.3"** (EL133UF1 / Spectra 6,
1200x1600, 6 colours) from a **Pimoroni Pico Plus 2** (RP2350B), written in C
against the Raspberry Pi Pico SDK.

This is the MVP. It does exactly one thing: paint a hardcoded test pattern of
six vertical colour stripes, to prove the panel paints what the firmware tells
it to from a known-good init sequence. There is no networking, no MQTT, no
remote frame fetch, no sleep, and no Tesserae server integration yet. Those are
later layers on top of this working panel driver.

It is part of the Tesserae self-hosted e-ink dashboard ecosystem, alongside
`tesserae-device-esp32-bin` (the same panel on an ESP32-S3) and the other
`tesserae-device-*` firmwares.

The panel command opcodes, argument blobs, and init/refresh sequencing
originate from Waveshare's official EL133UF1 / Spectra 6 demo
([waveshareteam/ESP32-S3-ePaper-13.3E6](https://github.com/waveshareteam/ESP32-S3-ePaper-13.3E6)),
derived from the EL133UF1 datasheet. This firmware follows the ESP-IDF port in
`tesserae-device-esp32-bin`, which adapted that demo; the opcodes and sequencing
are unchanged from the Waveshare original, only the transport (SPI/GPIO) was
rewritten for the RP2350.

## What you should see

After flashing, the panel takes roughly 30 seconds (Spectra 6 panels refresh
slowly) and then shows six vertical stripes, left to right:

```
| black | white | red | green | blue | yellow |
\------ CS_M (left) ------/\----- CS_S (right) -----/
        columns 0..599            columns 600..1199
```

The split down the middle is deliberate: the panel has two controllers, one per
half, selected by separate chip-select lines. The left three stripes are sent to
`CS_M` and the right three to `CS_S`. A clean seam at the centre means the dual
controller split is working.

## Hardware

| Part | Detail |
| --- | --- |
| Panel | Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6), 1200x1600, 6 colours, 4 bits per pixel |
| Board | Pimoroni Pico Plus 2 (RP2350B) |
| Adapter | Hard Stuff "Pico to Pi Hat" (bridges the Pico to the Inky's 40-pin Raspberry Pi header) |

### Pin map

Two layers decide each Pico GPIO. The Inky driver fixes which Pi BCM line
carries each panel signal (from `pimoroni/inky`, `inky/inky_el133uf1.py`); the
adapter fixes which Pico GPIO each Pi BCM line lands on (from Hard Stuff's
`pico_to_pi_mappings.h`). Composing them:

| Panel signal | Pi BCM line | Pico GPIO |
| --- | --- | --- |
| MOSI (data) | BCM10 | GP36 |
| SCLK (clock) | BCM11 | GP35 |
| CS_M (left half, cols 0..599) | BCM26 | GP7 |
| CS_S (right half, cols 600..1199) | BCM16 | GP42 |
| DC (data/command) | BCM22 | GP6 |
| RST (reset) | BCM27 | GP8 |
| BUSY (input) | BCM17 | GP41 |

The Inky Impression has no software power-enable line, so there is no power pin
(the ESP32 port's `EPD_PIN_PWR` has no equivalent here).

If you wire differently or use a different adapter, change the numbers in
[`include/epd_config.h`](include/epd_config.h). That is the only file with pin
definitions.

## Why bit-banged SPI, not the hardware SPI block

The adapter wires the panel's clock and data to GP35 and GP36. On the RP2350B
those pins are `SPI0 TX` and `SPI0 RX`; no SPI peripheral can route a clock onto
GP35, so the hardware SPI block cannot drive this panel through this adapter.
The driver therefore bit-bangs a transmit-only SPI on plain GPIO (the panel is
write-only and BUSY is a separate input, so nothing is lost). If you ever rewire
the clock/data lines onto proper hardware-SPI pins, this is the part to revisit.
A PIO-based SPI would also work and would clock faster, but plain GPIO is the
simplest thing that proves the panel.

## Why pico-sdk, and the platform fork

We use the official Raspberry Pi Pico SDK (PlatformIO framework `picosdk`)
because the port maps almost mechanically onto the ESP-IDF source. RP2350 +
`picosdk` is provided by the maxgerhardt fork of the PlatformIO raspberrypi
platform, which `platformio.ini` pins via its git URL. The stock `raspberrypi`
platform does not ship a `picosdk` framework, so the fork is required.

## Build

Prerequisites: [PlatformIO Core](https://platformio.org/install) (CLI) or the
PlatformIO IDE extension for VS Code. The examples below use the CLI; in VS Code
the same actions are on the PlatformIO toolbar (the checkmark builds, the arrow
uploads).

```sh
git clone <this-repo> tesserae-device-pico-bin
cd tesserae-device-pico-bin
pio run
```

The first build downloads the toolchain and the Pico SDK and takes a few
minutes; later builds take a few seconds. The output is
`.pio/build/pimoroni_pico_plus_2/firmware.uf2`.

## Flash

Two ways. The drag-and-drop UF2 route is the most reliable for a first flash:

1. Hold the **BOOTSEL** button on the Pico Plus 2 while plugging it into USB
   (or hold BOOTSEL and tap RESET). It mounts as a USB drive named `RP2350`.
2. Copy the UF2 onto it:

   ```sh
   cp .pio/build/pimoroni_pico_plus_2/firmware.uf2 /Volumes/RP2350/
   ```

   The board reboots and runs the firmware as soon as the copy finishes.

Alternatively, let PlatformIO flash it over USB (uses `picotool`; the board
should be in BOOTSEL mode, or already running this firmware so the 1200-baud
reset can kick it into the bootloader):

```sh
pio run -t upload
```

## Watch the logs (optional)

The firmware prints progress over USB serial. It does not wait for a monitor, so
the panel paints whether or not anything is attached.

```sh
pio device monitor
```

Expected output:

```
tesserae-device-pico-bin: painting six vertical stripes
epd: init complete
epd: streaming frame...
epd: refreshing (this takes ~30s on Spectra 6)...
epd: refresh done
done. the panel should now show: black white red | green blue yellow
```

## Source layout

| File | Purpose |
| --- | --- |
| [`include/epd_config.h`](include/epd_config.h) | Pin map, panel geometry, 6-colour palette. The one place to edit pins. |
| [`include/epd_13in3e.h`](include/epd_13in3e.h) | Driver API. |
| [`src/epd_13in3e.c`](src/epd_13in3e.c) | Driver: bit-bang SPI, reset, init sequence, dual-CS frame write, refresh, BUSY polling. Sequence from Waveshare's EL133UF1 demo, by way of `tesserae-device-esp32-bin`. |
| [`src/main.c`](src/main.c) | Generates the six vertical stripes and paints one frame. |

The init sequence magic numbers in `src/epd_13in3e.c` come from Waveshare's
EL133UF1 demo (by way of the ESP32 port's `src/epd_driver.c`). Do not edit the
canned parameter blobs.

## Not in this MVP

WiFi, MQTT, HTTP frame fetch, PNG/.bin decoding, deep sleep and wake, battery
monitoring, OTA, captive portal, and Tesserae server protocol integration.
These come later as separate layers on top of this driver.

## License

AGPL-3.0-or-later, matching the sibling `tesserae-device-*` repositories. See
[LICENSE](LICENSE).
