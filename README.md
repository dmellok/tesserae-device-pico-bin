# tesserae-device-pico-bin

A battery-friendly **Tesserae** e-paper client for a **Pimoroni Inky Impression**
panel driven by a **Pimoroni Pico Plus 2 W** (RP2350B), written in C against the
Raspberry Pi Pico SDK. It is part of the Tesserae self-hosted e-ink dashboard
ecosystem, alongside `tesserae-device-esp32-bin` and the other
`tesserae-device-*` firmwares.

Each wake it runs one cycle: detect the panel from its model EEPROM, connect
WiFi, sync time over SNTP, read the retained frame URL + config from the
Tesserae MQTT broker, download the panel-native `.bin` frame into PSRAM, paint
it (skipping the slow refresh when the frame is unchanged), publish a heartbeat,
then **POWMAN deep sleep** until the next refresh. With no WiFi credentials it
comes up as a captive-portal setup AP instead. The headline pieces:

- WiFi (CYW43) + lwIP, MQTT, SNTP, HTTP frame fetch
- 8 MB QMI PSRAM for the 960 KB 13.3" frame; SHA-256 dedup so an unchanged
  frame never triggers a refresh
- Real RP2350 POWMAN deep sleep (LPOSC-trimmed), wake-reason tracking
- Captive provisioning portal (setup AP + DHCP + DNS hijack + WiFi scan + form),
  mDNS (`tesserae-pico-XXXX.local`), and an on-device procedural setup splash
- Reports `kind=pico_bin_client` to Tesserae (see "Tesserae server" below)

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

## Operation

On boot the firmware identifies the panel, then:

- **No WiFi credentials** (fresh device): it paints the setup splash (Tesserae
  logo, the AP name, the portal URL, and a join QR), brings up an open
  `Tesserae-Setup-XXXX` access point with a captive portal, and serves a form to
  enter WiFi + MQTT settings. Saving reboots and applies them.
- **Configured**: connect WiFi, SNTP, read the retained frame URL + config from
  `tesserae/<device_id>/{frame/bin,config}`, download the `.bin`, and paint it
  (a full refresh is ~20-45 s; e-paper is slow). If the frame URL is unchanged
  since last time (SHA-256 match) the refresh is skipped. It then publishes a
  retained heartbeat to `tesserae/<device_id>/status` and deep-sleeps for the
  configured `sleep_interval_s`, waking to repeat.

The 13.3" panel has two controllers split at column 600 (`CS_M` left half,
`CS_S` right). The driver replicates the Pimoroni `inky_el133uf1` transform:
the landscape frame is rotated 90 deg and split, so a portrait composition
displays upright. A `frame == NULL` path still paints the six/seven colour
stripe test pattern as a fallback.

### Tesserae server

This device deep-sleeps, so it needs the frame published **retained** (it reads
the topic on wake), and it does the rotate on-device, so it needs the frame in
the panel's **native landscape** orientation. Register the device in Tesserae as
**`pico_bin_client`** (the `pico_bin` renderer: landscape pack + `retain: true`).
The firmware reports that kind in its heartbeat so Settings -> Devices pre-fills
it.

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
tesserae-device-pico-bin
psram: 8388608 bytes detected; self-test PASS
config: wifi_ssid='turtle' device_id='pico_lounge' sleep_s=900 last_hash=set
eeprom: 1600x1200 variant=21 (Spectra 6 13.3 1600 x 1200 (EL133UF1))
panel: EL133UF1 13.3in Spectra 6 (1200x1600)
wake: timer (boot #42)
wifi: connected, ip=192.168.50.66 rssi=-54
mdns: advertising tesserae-pico-30b3.local
sntp: epoch=1781852034
mqtt: frame url = http://192.168.50.125:8765/renders/<hash>.bin
frame: unchanged (hash match); skipping refresh
mqtt: status published (243 bytes)
paint: skipped (unchanged)
sleep: deep sleep for 900 s (wake reboots)
```

## Source layout

| File | Purpose |
| --- | --- |
| [`src/main.c`](src/main.c) | Orchestrates one wake cycle: panel detect, portal-or-connect, fetch + dedup + paint, heartbeat, deep sleep. |
| [`src/epd_io.c`](src/epd_io.c) / [`src/panels.c`](src/panels.c) | Shared SPI1 transport, and one descriptor + `paint(variant, frame)` per panel (init + geometry + CS + the 13.3" rotate/split). Sequences from the Pimoroni Inky drivers. |
| [`src/inky_eeprom.c`](src/inky_eeprom.c) | Bit-banged I2C reader for the model EEPROM (panel detection). |
| [`src/config.c`](src/config.c) | Flash-backed key-value config store (WiFi/MQTT/sleep/last-frame-hash). |
| [`src/psram.c`](src/psram.c) | RP2350 QMI PSRAM (APS6404) bring-up for the 960 KB frame buffer. |
| [`src/net_wifi.c`](src/net_wifi.c) / [`src/net_mqtt.c`](src/net_mqtt.c) / [`src/net_http.c`](src/net_http.c) / [`src/net_sntp.c`](src/net_sntp.c) | CYW43/lwIP WiFi, MQTT (retained frame/config + status), HTTP frame fetch, SNTP. |
| [`src/net_mdns.c`](src/net_mdns.c) / [`src/net_portal.c`](src/net_portal.c) | mDNS advertising and the captive provisioning portal (AP + DHCP + DNS hijack + scan + form). |
| [`src/sleepmgr.c`](src/sleepmgr.c) | POWMAN deep sleep + wake reason + LPOSC-trimmed wall-clock. |
| [`src/splash.c`](src/splash.c) | On-device procedural setup splash (logo + text + QR). |
| [`src/vendor/`](src/vendor) | Third-party: DHCP/DNS servers, QR encoder. See [NOTICES.md](NOTICES.md). |

The init-sequence argument blobs in `src/panels.c` come from the Pimoroni Inky
drivers. Do not edit the canned parameter values. To add a panel, add a
`paint()` and a `panel_t` descriptor with its variant ids.

Development overrides (build flags or `secrets.h` defines, all off by default):
`DEV_SLEEP_S` (pin/disable the sleep interval; 0 = stay awake), `DEV_FORCE_REPAINT`
(ignore the dedup), `DEV_FRAME_URL` (fetch a fixed URL), `DEV_FORCE_PORTAL`
(force the setup portal). See [`include/secrets.example.h`](include/secrets.example.h).

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

## Not implemented

Battery monitoring (the heartbeat reports `battery_*: 0`; no sense ADC is wired)
and OTA updates. A LAN settings editor was scoped out: a deep-sleep device is not
a persistent web server, so deployed units are reconfigured via the setup portal.

## License

AGPL-3.0-or-later (see [LICENSE](LICENSE)), matching the sibling
`tesserae-device-*` repositories. Bundled third-party code (DHCP/DNS servers,
font, QR encoder) keeps its own permissive license; see [NOTICES.md](NOTICES.md).
