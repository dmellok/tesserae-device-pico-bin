# tesserae-device-pico-bin

A battery-friendly **Tesserae** e-paper client for a **Pimoroni Inky Impression**
panel driven by a **Pimoroni Pico Plus 2 W** (RP2350B), written in C against the
Raspberry Pi Pico SDK. It is part of the Tesserae self-hosted e-ink dashboard
ecosystem, alongside `tesserae-device-esp32-bin` and the other
`tesserae-device-*` firmwares.

Each wake it runs one cycle: detect the panel from its model EEPROM, connect
WiFi, ask Tesserae for the current frame, download the panel-native `.bin` into
PSRAM, paint it (skipping the slow refresh when the frame is unchanged), post a
heartbeat, then **POWMAN deep sleep** until the next refresh. With no WiFi
credentials it comes up as a captive-portal setup AP instead. The headline
pieces:

- Three transports, picked automatically: the **Tesserae REST API** (default),
  the **cloud relay** for a panel that cannot see the home instance at all, and
  a legacy MQTT path
- **HTTPS** throughout (mbedTLS over lwIP altcp) with a small built-in root
  store and hostname verification
- 8 MB QMI PSRAM for the 960 KB 13.3" frame; ETag/SHA-256 dedup so an unchanged
  frame never triggers a refresh
- Real RP2350 POWMAN deep sleep (LPOSC-trimmed), wake-reason tracking, and a
  WiFi fast-connect hint that skips the all-channel scan on every wake
- Captive provisioning portal (setup AP + DHCP + DNS hijack + WiFi scan + form),
  mDNS (`tesserae-pico-XXXX.local`), and on-device procedural splashes that say
  what setup is waiting on
- Battery telemetry with a low-battery goodbye screen, and a 20-second
  front-button hold for a factory reset
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
  enter WiFi plus a Tesserae server URL or a cloud-relay pairing code. Saving
  reboots and applies them.
- **Configured**: connect WiFi, ask the server for the current frame
  (`If-None-Match` against the last ETag), download the `.bin`, and paint it
  (a full refresh is ~20-45 s; e-paper is slow). An unchanged frame answers 304
  and the refresh is skipped entirely. It then posts a heartbeat and
  deep-sleeps for the interval the server hands back, waking to repeat.

The wall clock comes from each response's HTTP `Date` header, which works on a
LAN with no internet and costs nothing; SNTP is only used on the legacy MQTT
path.

### Onboarding feedback

Setup narrates itself, so a headless device is never a black box:

| The panel says | What happened | What to do |
| --- | --- | --- |
| *Wi-Fi didn't connect* | Wrong password, or the AP is gone | Rejoin the setup AP and fix it |
| *Can't reach the server* | WiFi is fine, Tesserae is not answering | Check the URL, check the server is up |
| *Almost done* | Reached the server, waiting on approval | Approve it in Settings -> Devices -> Discovered |
| *Pairing with the relay* | Code submitted, waiting on the home instance | Nothing; it can take a few minutes |
| *Connected!* | Onboarded, no frame rendered yet | Assign the device a deck or page |
| *Battery empty* | Cell below 3.25 V; the device has hibernated | Charge it; it re-checks hourly |

These cost a full refresh, so they paint only on a real **state change**, and
only while the panel is not already showing a dashboard. Once a real frame is
up, a single failed poll leaves it alone -- a stale dashboard is far more useful
than an error card.

### Cloud relay

A panel can live somewhere the Tesserae server is not reachable from -- a
different house, a phone hotspot, behind CGNAT -- without exposing the home
instance to the internet. Both ends talk **outbound** to a small Cloudflare
Worker that acts as a per-device mailbox: home seals each rendered frame and
`PUT`s it, the panel polls and decrypts it.

The relay is **zero-knowledge**. At pairing the panel and the home instance
exchange X25519 *public* keys through it and each derive the same AES-256-GCM
frame key locally; the key is never transmitted. The relay only ever holds
ciphertext, two public keys and a token hash, so it can validate a poll but
never read a dashboard. Frames decrypt **in place** in PSRAM (a second 960 KB
buffer would be wasteful), and a failed authentication tag is never painted:
it means the mailbox does not match this panel's key.

Setup is one extra card in the setup portal: a relay URL (defaults to
`https://relay.tesserae.ink`) and the single-use pairing code from
*Settings -> Cloud relay -> Add a remote panel*. A relay panel needs **no**
server URL; the portal accepts either. Pairing may span several wakes, and the
firmware submits the code exactly once (only that route enforces the 10-minute
expiry) and then polls until home completes the exchange.

Two consecutive wakes answered `401` mean the pairing was revoked. The device
then clears its mailbox identity, keeps the last image on the glass, and reopens
setup so a fresh code can be typed. Two wakes rather than one is deliberate:
unpairing costs a physical trip, so a single `401` injected by a captive portal
should not trigger it.

The wire contract is `docs/relay/contract.md` in the Tesserae repo, and it is
authoritative for all three sides.

### TLS

`https://` works for both the REST server and the relay. mbedTLS is built from
the Pico SDK's bundle against a deliberately narrow
[`include/mbedtls_config.h`](include/mbedtls_config.h) (TLS 1.2 client, ECDHE +
AES-GCM, no server, no DTLS, no PSK), and the trust store in
[`include/ca_roots.h`](include/ca_roots.h) is three roots rather than a full web
CA bundle -- this device talks to one class of host, and trusting ~150 CAs to
reach it would be a strictly worse trade. Regenerate it with
[`scripts/gen_ca_roots.sh`](scripts/gen_ca_roots.sh), which fingerprint-checks
every root before writing it.

Chain and hostname verification are always enforced. Certificate **dates** are
skipped while the wall clock has never been set, because a cold-booted
relay-only panel has no way to learn the time before its first handshake, and
failing closed there would brick it on any network that blocks NTP. Once any
wake sets the clock -- which survives deep sleep -- dates are checked normally.
The dashboard does not rest on this in any case: frames are sealed under a key
the relay never sees, so a TLS compromise reads a device token, not pixels.

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
config: wifi_ssid='my-wifi' device_id='pico' sleep_s=900 last_hash=set
eeprom: 1600x1200 variant=21 (Spectra 6 13.3 1600 x 1200 (EL133UF1))
panel: EL133UF1 13.3in Spectra 6 (1200x1600)
wake: timer (boot #42)
wifi: connected, ip=192.168.1.42 rssi=-54
mdns: advertising tesserae-pico-xxxx.local
sntp: epoch=1781852034
mqtt: frame url = http://192.168.1.50:8765/renders/<hash>.bin
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
| [`src/config.c`](src/config.c) | Flash-backed config store (WiFi, server/relay identity, sleep interval, cached ETags, WiFi fast-connect hint). |
| [`src/psram.c`](src/psram.c) | RP2350 QMI PSRAM (APS6404) bring-up for the 960 KB frame buffer. |
| [`src/net_httpc.c`](src/net_httpc.c) / [`src/net_tls.c`](src/net_tls.c) | The one HTTP/1.1 client, over plain TCP or TLS via lwIP altcp; streaming body sink for frames, small buffer for control JSON. TLS config, trust store and hostname verification. |
| [`src/net_rest.c`](src/net_rest.c) | The Tesserae REST protocol: discover/register, frame + ETag, status heartbeat. |
| [`src/relay.c`](src/relay.c) / [`src/relay_wire.c`](src/relay_wire.c) / [`src/relay_crypto.c`](src/relay_crypto.c) | Cloud relay: pairing state machine and polling; pure wire parsing and URL building; X25519 + HKDF-SHA256 + AES-256-GCM frame crypto. |
| [`src/net_wifi.c`](src/net_wifi.c) / [`src/net_mqtt.c`](src/net_mqtt.c) / [`src/net_sntp.c`](src/net_sntp.c) | CYW43/lwIP WiFi with the fast-connect path, the legacy MQTT transport, SNTP. |
| [`src/net_mdns.c`](src/net_mdns.c) / [`src/net_portal.c`](src/net_portal.c) | mDNS advertising and the captive provisioning portal (AP + DHCP + DNS hijack + scan + form). |
| [`src/sleepmgr.c`](src/sleepmgr.c) | POWMAN deep sleep + wake reason + LPOSC-trimmed wall-clock + the across-wake counters. |
| [`src/battery.c`](src/battery.c) / [`src/buttons.c`](src/buttons.c) | VSYS battery sense; the front buttons (awake-only, see the header). |
| [`src/splash.c`](src/splash.c) | On-device procedural splashes: setup (logo + text + QR) and the status screens. |
| [`src/vendor/`](src/vendor) | Third-party: DHCP/DNS servers, QR encoder, cJSON, Monocypher. See [NOTICES.md](NOTICES.md). |
| [`scripts/mbedtls.py`](scripts/mbedtls.py) | Builds the Pico SDK's bundled mbedTLS + lwIP's altcp_tls glue, which the PlatformIO framework does not. |

The init-sequence argument blobs in `src/panels.c` come from the Pimoroni Inky
drivers. Do not edit the canned parameter values. To add a panel, add a
`paint()` and a `panel_t` descriptor with its variant ids.

Development overrides (build flags or `secrets.h` defines, all off by default):
`DEV_SLEEP_S` (pin/disable the sleep interval; 0 = stay awake), `DEV_FORCE_REPAINT`
(ignore the dedup), `DEV_FRAME_URL` (fetch a fixed URL), `DEV_FORCE_PORTAL`
(force the setup portal). `SERVER_URL`/`PAIRING_CODE` and `RELAY_URL`/`RELAY_CODE`
seed the two transports without going through the portal. See
[`include/secrets.example.h`](include/secrets.example.h).

The firmware version reported in the heartbeat is stamped by CI
(`-DTESSERAE_FW_VERSION`); a local build reports `0.0.0-dev`, which the server
reads as "never offer an update to this".

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

## Factory reset

Hold the **A** button (leftmost, GP15) and tap RESET, then keep holding for 20
seconds. The stored config is erased and the device reboots into the setup
portal. Serial narrates the countdown, and releasing at any point cancels.

This exists because the connectivity-failure fallback cannot cover everything:
a device whose SSID still connects but whose server URL is wrong will never
trip it. The hold runs before anything touches flash or the radio, so it works
even when the rest of the cycle would not.

Buttons cannot **wake** this board -- POWMAN's power-down cuts the pad domain,
so the pad cannot sense an edge while the always-on timer wake still fires -- so
the gesture necessarily starts from a reset.

## Not implemented

**OTA updates.** The RP2350 has no built-in A/B slot scheme, so this needs a
bootloader (something like `picowota`) before the signed-descriptor half is
worth writing. The heartbeat already reports `fw_version` so the server knows
what is running.

**`always_on` / kiosk mode.** The server's `config.always_on` is parsed and
logged but not acted on: this board has no stay-awake loop yet. Adopting it
silently would have the Devices UI claim a behaviour the panel does not have.

**Local frame cache / offline albums.** There is no SD slot, but there is ~15 MB
of spare QSPI flash and a hardware SHA-256 block, so this is feasible; the
blocker is that deck *navigation* is button-driven and buttons cannot wake the
device.

**Partial refresh, touch, and the protocol-v2 interaction model.** All need
partial refresh and a digitizer. The Inky Spectra 6 has neither.

A LAN settings editor was scoped out: a deep-sleep device is not a persistent
web server, so deployed units are reconfigured via the setup portal.

## License

AGPL-3.0-or-later (see [LICENSE](LICENSE)), matching the sibling
`tesserae-device-*` repositories. Bundled third-party code (DHCP/DNS servers,
font, QR encoder) keeps its own permissive license; see [NOTICES.md](NOTICES.md).
