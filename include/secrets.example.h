/*
 * secrets.example.h: copy to include/secrets.h (git-ignored) and fill in to
 * bypass the provisioning portal during development. If secrets.h is absent the
 * firmware falls back to stored config / the portal.
 *
 *   cp include/secrets.example.h include/secrets.h
 *
 * To enable any optional line below, just delete its leading "//".
 */
#pragma once

/* WiFi station credentials. Leave WIFI_PASS empty ("") for an open network. */
#define WIFI_SSID   "your-network"
#define WIFI_PASS   "your-password"

/* Tesserae server (REST transport, recommended). Defining SERVER_URL selects the
 * REST API over MQTT. Leave PAIRING_CODE unset to auto-register (approve the
 * device in Tesserae -> Settings -> Devices); set it only for strict admin-gated
 * pairing. No trailing slash. */
//#define SERVER_URL      "http://tesserae.local:8765"
//#define PAIRING_CODE    "123456"

/* Cloud relay (for a panel that cannot reach the Tesserae server directly).
 * Get a single-use code from Settings -> Cloud relay -> Add a remote panel.
 *
 * The code is a SEED, not a standing instruction: it is applied only when it
 * differs from what is stored and the device is not already paired, so a
 * reflash does not restart pairing or clobber a working mailbox. The device
 * then works the code for 24 hours (RELAY_PAIR_WINDOW_MS in relay.h) before
 * giving up -- the relay's own short expiry applies only to the initial
 * submission, which the firmware does immediately and exactly once.
 *
 * A relay panel needs NO SERVER_URL. The two are alternatives, and the relay
 * wins if both are set. */
//#define RELAY_URL       "https://relay.tesserae.ink"
//#define RELAY_CODE      "ABCD1234"

/* Kiosk mode: never deep-sleep, run the cycle on a timer instead. Keeps serial,
 * mDNS and the 1200-baud flash touch reachable between refreshes, which makes
 * bench work far less painful. USB power only -- the radio and core stay up,
 * and this will flatten a battery in hours. The server can also set it (relay
 * config always_on). */
//#define ALWAYS_ON       1

/* MQTT broker and identity (legacy; only used when SERVER_URL is not set). */
//#define MQTT_URI        "mqtt://homeassistant.local:1883"
//#define MQTT_DEVICE_ID  "pico"
//#define MQTT_USER       ""
//#define MQTT_PASS       ""

/* ------------------------------------------------------------------------
 * Development overrides. All off by default; uncomment the ones you want.
 * Read in main.c (which includes this file), so setting them here is
 * equivalent to passing them as -D build flags, just easier to tweak.
 * ------------------------------------------------------------------------ */

/* Pin the deep-sleep interval (seconds), overriding the broker/flash value.
 * 0 = stay awake and loop the cycle (no deep sleep) so USB serial stays up. */
//#define DEV_SLEEP_S        60

/* Ignore the SHA-256 dedup: re-download and repaint every cycle. */
//#define DEV_FORCE_REPAINT  1

/* Bypass the retained MQTT frame topic and fetch this fixed URL instead. */
//#define DEV_FRAME_URL      "http://192.168.1.50:8765/renders/abc.bin"

/* Force the provisioning portal (setup AP) on boot even when creds exist. */
//#define DEV_FORCE_PORTAL   1

/* Startup delay (ms) so a serial monitor can catch the boot logs. Default 0
 * (production: no wasted awake time on each wake); set ~2000 for bench debugging. */
//#define DEV_BOOT_LOG_DELAY_MS  2000
