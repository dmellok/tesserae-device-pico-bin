# tesserae-device-pico-bin

Firmware for driving a **Pimoroni Inky Impression 13.3"** (EL133UF1 / Spectra 6,
1200x1600, 6 colours) from a **Pimoroni Pico Plus 2 W** (RP2350B), written in C
against the Raspberry Pi Pico SDK.

This is the MVP. It does exactly one thing: paint a hardcoded test pattern of
six vertical colour stripes, to prove the panel paints what the firmware tells
it to. There is no networking, no MQTT, no remote frame fetch, no sleep, and no
Tesserae server integration yet. Those are later layers on top of this working
panel driver.

It is part of the Tesserae self-hosted e-ink dashboard ecosystem, alongside
`tesserae-device-esp32-bin` (the same panel on an ESP32-S3) and the other
`tesserae-device-*` firmwares.

The panel init/power command sequence and argument values are ported from
Pimoroni's Inky driver
([pimoroni/inky, `inky/inky_el133uf1.py`](https://github.com/pimoroni/inky/blob/main/inky/inky_el133uf1.py)),
which is the source of truth for this board. An earlier attempt ported the
Waveshare ESP32 demo for the bare EL133UF1 panel and the panel stayed blank: the
Inky board needs its own power configuration (extra DCDC / POFS / CMDA4 commands
and different boost/PSR/CDI values), so it is not a drop-in for the Waveshare
panel.

## What you should see

After flashing, the panel takes roughly 20 to 35 seconds (Spectra 6 panels
refresh slowly, and the time varies run to run) and then shows six vertical
stripes, left to right:

```
| black | white | red | green | blue | yellow |
\------ CS_M (left) ------/\----- CS_S (right) -----/
        columns 0..599            columns 600..1199
```

The split down the middle is deliberate: the panel has two controllers, one per
half, selected by separate chip-select lines. The left three stripes go to
`CS_M`, the right three to `CS_S`. A clean seam at the centre means the dual
controller split is working.

## Hardware

| Part | Detail |
| --- | --- |
| Panel | Pimoroni Inky Impression 13.3" (EL133UF1 / Spectra 6), 1200x1600, 6 colours, 4 bits per pixel |
| Board | Pimoroni Pico Plus 2 W (RP2350B) |
| Adapter | Hard Stuff "Pico to Pi Hat", carrying the Pico's GPIO to the Inky's 40-pin Raspberry Pi header |

No jumpers or rewiring are needed: the Inky seats on the adapter, the Pico seats
on the adapter, and the standard 40-pin header carries everything including
power.

### Pin map

This particular adapter turned out to be a **1:1 Raspberry-Pi-BCM-to-Pico-GP
passthrough, with one exception: it swaps SCLK and MOSI** (verified with a
continuity meter; the vendor's published `pico_to_pi_mappings.h` and PDF do not
match this unit and should be ignored). The Inky's per-signal BCM lines come
from `inky_el133uf1.py`. Composing them gives:

| Panel signal | Pi BCM | Pico GP | Notes |
| --- | --- | --- | --- |
| SCLK (clock) | BCM11 | **GP10** | adapter swaps clock/data; GP10 = SPI1 SCK |
| MOSI (data) | BCM10 | **GP11** | GP11 = SPI1 TX |
| CS_M (left half, cols 0..599) | BCM26 | GP26 | |
| CS_S (right half, cols 600..1199) | BCM16 | GP16 | |
| DC (data/command) | BCM22 | GP22 | low = command, high = data |
| RST (reset) | BCM27 | GP27 | active low |
| BUSY (input) | BCM17 | GP17 | **low = busy**, high = ready |
| EEPROM SDA | BCM2 | GP2 | model-ID I2C (optional) |
| EEPROM SCL | BCM3 | GP3 | |

None of these collide with the RM2 wireless module (GP23/24/25/29), so WiFi
remains available for later. The Inky has no software power-enable line, so
there is no power pin; it takes 3V3 and GND from the Pico.

All pin numbers live in [`include/epd_config.h`](include/epd_config.h). If you
use a different adapter or board, that is the only file to change.

## Gotchas worth knowing

Bringing this panel up on the Pico took longer than expected; these are the
non-obvious things, all captured in code comments too:

- **SCLK and MOSI are swapped by the adapter.** Driving the clock and data on
  the wrong pins gives a completely blank panel even though everything else
  looks right. Confirm with a meter, not the adapter's docs.
- **The command setup delay is mandatory.** The controller will not latch a
  command unless D/C is held ~300 ms before the clock starts (`DC_SETUP_MS`).
  Too short and the panel silently ignores the DTM (data) command, refreshes an
  empty buffer, and stays blank. This adds ~8 s to each refresh and is the price
  of reliable command latching.
- **BUSY is active low here** (low = busy), the opposite of what the Pimoroni
  Python driver's pull-up heuristic suggests. The refresh wait holds while BUSY
  is low and releases when it goes high.
- **The model EEPROM is a separate I2C chip**, not the panel controller.
  Reading it (it reports `variant=21`, "Spectra 6 13.3 ... EL133UF1") is a handy
  way to confirm the board is wired and talking before chasing the SPI side.

## Why pico-sdk, and the platform fork

We use the official Raspberry Pi Pico SDK (PlatformIO framework `picosdk`).
RP2350 + `picosdk` is provided by the maxgerhardt fork of the PlatformIO
raspberrypi platform, which `platformio.ini` pins via its git URL. The stock
`raspberrypi` platform does not ship a `picosdk` framework, so the fork is
required.

## Build

Prerequisites: [PlatformIO Core](https://platformio.org/install) (CLI) or the
PlatformIO IDE extension for VS Code. The examples use the CLI; in VS Code the
same actions are on the PlatformIO toolbar.

```sh
git clone <this-repo> tesserae-device-pico-bin
cd tesserae-device-pico-bin
pio run
```

The first build downloads the toolchain and the Pico SDK and takes a few
minutes; later builds take a few seconds. The output is
`.pio/build/pimoroni_pico_plus_2/firmware.uf2`.

## Flash

The drag-and-drop UF2 route is the most reliable:

1. Hold **BOOTSEL** on the Pico Plus 2 W while plugging in USB (or hold BOOTSEL
   and tap RESET). It mounts as a USB drive named `RP2350`.
2. Copy the UF2 onto it:

   ```sh
   cp .pio/build/pimoroni_pico_plus_2/firmware.uf2 /Volumes/RP2350/
   ```

   The board reboots and runs as soon as the copy finishes.

Alternatively, let PlatformIO flash over USB (uses `picotool`; the board should
be running this firmware so the 1200-baud reset can kick it into the bootloader,
or already in BOOTSEL):

```sh
pio run -t upload
```

## Watch the logs (optional)

The firmware prints progress over USB serial. It does not wait for a monitor, so
the panel paints whether or not anything is attached.

```sh
pio device monitor   # 115200 baud
```

Expected output:

```
tesserae-device-pico-bin: painting six vertical stripes
eeprom: 1600x1200 variant=21 (Spectra 6 13.3 1600 x 1200 (EL133UF1))
epd: init complete
epd: streaming frame...
epd: refreshing (this takes ~30s on Spectra 6)...
epd: refresh complete after 33800ms
epd: refresh done
done. the panel should now show: black white red | green blue yellow
```

## Source layout

| File | Purpose |
| --- | --- |
| [`include/epd_config.h`](include/epd_config.h) | Pin map, SPI config, panel geometry, 6-colour palette. The one place to edit pins. |
| [`include/epd_13in3e.h`](include/epd_13in3e.h) | Panel driver API. |
| [`src/epd_13in3e.c`](src/epd_13in3e.c) | Driver: hardware SPI, reset, init sequence, dual-CS frame write, refresh, BUSY wait. Sequence ported from `inky_el133uf1.py`. |
| [`include/inky_eeprom.h`](include/inky_eeprom.h) / [`src/inky_eeprom.c`](src/inky_eeprom.c) | Bit-banged I2C reader for the Inky model EEPROM (panel detection / comms check). |
| [`src/main.c`](src/main.c) | Reads the EEPROM, generates the six vertical stripes, paints one frame. |

The init sequence argument blobs in `src/epd_13in3e.c` come from
`inky_el133uf1.py`. Do not edit the canned parameter values.

## Not in this MVP

WiFi, MQTT, HTTP frame fetch, PNG/.bin decoding, deep sleep and wake, battery
monitoring, OTA, captive portal, and Tesserae server protocol integration. These
come later as separate layers on top of this driver.

## License

AGPL-3.0-or-later, matching the sibling `tesserae-device-*` repositories. See
[LICENSE](LICENSE).
