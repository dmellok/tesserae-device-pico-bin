# tesserae-device-pico-bin

Firmware for driving a **Pimoroni Inky Impression** e-paper panel from a
**Pimoroni Pico Plus 2 W** (RP2350B), written in C against the Raspberry Pi Pico
SDK. It reads the Inky's model EEPROM at boot, picks the matching panel driver,
and paints a hardcoded test pattern of vertical colour stripes, to prove the
panel paints what the firmware tells it to. No networking, no sleep, no Tesserae
integration yet; those are later layers.

It is part of the Tesserae self-hosted e-ink dashboard ecosystem, alongside
`tesserae-device-esp32-bin` and the other `tesserae-device-*` firmwares.

### Supported panels

Selected automatically from the EEPROM `display_variant`. Command sequences are
ported from the corresponding Pimoroni Inky driver
([pimoroni/inky](https://github.com/pimoroni/inky)).

| Panel | Family | Variants | Resolution | Status |
| --- | --- | --- | --- | --- |
| 13.3" EL133UF1 | Spectra 6 | 21, 27 | 1200x1600 | **verified on hardware** |
| 7.3" E673 | Spectra 6 | 22, 26 | 800x480 | untested port |
| 4.0" E640 | Spectra 6 | 25 | 600x400 | untested port |
| 7.3" AC073TC1A | 7-colour | 20 | 800x480 | untested port |
| 5.7" / 4" UC8159 | 7-colour | 14, 15, 16 | 600x448 / 640x400 | untested port |

Only the 13.3" is confirmed working. The others are blind ports from the
Pimoroni reference drivers; the firmware prints `[UNTESTED driver]` for them.
Verify each on the real panel before trusting it.

Note: an earlier attempt ported the Waveshare ESP32 demo for the bare EL133UF1
and the panel stayed blank; the Inky needs its own power configuration, so it is
not a drop-in for the Waveshare panel. All drivers here follow the Inky sources.

## What you should see

After flashing, the panel refreshes (roughly 20 to 45 seconds; e-paper is slow
and the time varies) and shows vertical colour stripes. On a 6-colour Spectra 6
panel that is six stripes; on a 7-colour panel, seven. For the 13.3", left to
right:

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

Shared transport (same on every panel, defined in `include/epd_io.h`):

| Signal | Pi BCM | Pico GP | Notes |
| --- | --- | --- | --- |
| SCLK (clock) | BCM11 | **GP10** | adapter swaps clock/data; GP10 = SPI1 SCK |
| MOSI (data) | BCM10 | **GP11** | GP11 = SPI1 TX |
| DC (data/command) | BCM22 | GP22 | low = command, high = data |
| RST (reset) | BCM27 | GP27 | active low |
| BUSY (input) | BCM17 | GP17 | **low = busy**, high = ready |
| EEPROM SDA / SCL | BCM2 / BCM3 | GP2 / GP3 | model-ID I2C (in `src/inky_eeprom.c`) |

Chip-select differs per panel (defined in `src/panels.c`): the 13.3" is
dual-CS, **CS_M = GP26** (left half, cols 0..599) and **CS_S = GP16** (right
half); every other panel is single-CS on **GP8** (Pi BCM8).

None of these collide with the RM2 wireless module (GP23/24/25/29), so WiFi
remains available for later. The Inky has no software power-enable line, so
there is no power pin; it takes 3V3 and GND from the Pico.

Transport pins live in [`include/epd_io.h`](include/epd_io.h) and the per-panel
chip-selects in [`src/panels.c`](src/panels.c). If you use a different adapter or
board, those are the only places to change pins.

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
`.pio/build/pimoroni_pico_plus_2w/firmware.uf2`.

## Flash

The drag-and-drop UF2 route is the most reliable:

1. Hold **BOOTSEL** on the Pico Plus 2 W while plugging in USB (or hold BOOTSEL
   and tap RESET). It mounts as a USB drive named `RP2350`.
2. Copy the UF2 onto it:

   ```sh
   cp .pio/build/pimoroni_pico_plus_2w/firmware.uf2 /Volumes/RP2350/
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
| [`include/epd_io.h`](include/epd_io.h) / [`src/epd_io.c`](src/epd_io.c) | Shared transport: hardware SPI1, D/C, reset, BUSY wait, command + frame-stream helpers. Fixed pins (SCLK/MOSI/DC/RST/BUSY); CS passed in per panel. |
| [`include/panels.h`](include/panels.h) / [`src/panels.c`](src/panels.c) | One descriptor + `run()` per panel (init sequence, geometry, CS scheme, stripe pattern), plus the variant-to-panel registry. Sequences ported from the Pimoroni Inky drivers. |
| [`include/inky_eeprom.h`](include/inky_eeprom.h) / [`src/inky_eeprom.c`](src/inky_eeprom.c) | Bit-banged I2C reader for the Inky model EEPROM (panel detection). |
| [`src/main.c`](src/main.c) | Reads the EEPROM, looks up the panel by variant, runs its driver. |

The init-sequence argument blobs in `src/panels.c` come from the Pimoroni Inky
drivers. Do not edit the canned parameter values. To add a panel, add a `run()`
and a `panel_t` descriptor with its variant ids.

## Porting to another board

Nothing here is specific to the Pico Plus 2 W. The firmware only uses standard
GPIOs (GP2, 3, 8, 10, 11, 16, 17, 22, 26, 27, all in the GP0-29 range present on
every Pico), drives the panel with `SPI1` (SCK=GP10, TX=GP11, the same on RP2040
and RP2350), streams the frame row by row so it needs almost no RAM or flash,
and never touches PSRAM or the wireless radio.

- **Another Pico in the same Hard Stuff adapter** (Pico 2 / 2 W, Plus 2 non-W,
  original Pico / Pico W): change `board` in `platformio.ini` to the matching id
  (for example `rpipico2`, `pimoroni_pico_plus_2w`, `rpipico`, `rpipicow`); the
  `picosdk` framework supports them all. The adapter is fixed and the pins are
  standard, so the mapping is unchanged. Verified only on the Plus 2 W, but it
  should build and run on any of these.
- **A different adapter or direct wiring:** edit the transport pins in
  [`include/epd_io.h`](include/epd_io.h) and the per-panel chip-select in
  [`src/panels.c`](src/panels.c) to match your wiring. Use hardware-SPI-capable
  pins for SCLK/MOSI to keep the SPI block. The panel quirks (300 ms command
  setup for Spectra 6, BUSY active low, SPI speeds) are panel properties, not
  board properties, so they carry over.
- **A non-RP board (ESP32, STM32, ...):** not supported as-is; it is pico-sdk C.
  Only the `epd_io` transport layer would need porting; the panel sequences in
  `panels.c` would carry over (this is essentially what `tesserae-device-esp32
  -bin` already does for the 13.3").

## Not in this MVP

WiFi, MQTT, HTTP frame fetch, PNG/.bin decoding, deep sleep and wake, battery
monitoring, OTA, captive portal, and Tesserae server protocol integration. These
come later as separate layers on top of this driver.

## License

AGPL-3.0-or-later, matching the sibling `tesserae-device-*` repositories. See
[LICENSE](LICENSE).
