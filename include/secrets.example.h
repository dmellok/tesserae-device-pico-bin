/*
 * secrets.example.h: copy to include/secrets.h (git-ignored) and fill in to
 * bypass the provisioning portal during development. If secrets.h is absent the
 * firmware falls back to stored config / the portal.
 *
 *   cp include/secrets.example.h include/secrets.h
 */
#pragma once

/* WiFi station credentials. Leave WIFI_PASS empty ("") for an open network. */
#define WIFI_SSID   "your-network"
#define WIFI_PASS   "your-password"

/* MQTT broker and identity (used once the MQTT layer lands). */
/* #define MQTT_URI        "mqtt://homeassistant.local:1883" */
/* #define MQTT_DEVICE_ID  "pico" */
/* #define MQTT_USER       "" */
/* #define MQTT_PASS       "" */

/* ------------------------------------------------------------------------
 * Development overrides. All off by default; uncomment the ones you want.
 * These are read in main.c, which includes this file, so setting them here is
 * equivalent to passing them as -D build flags (no env var needed).
 * ------------------------------------------------------------------------ */

/* Pin the deep-sleep interval (seconds), overriding the broker/flash value.
 * Set 0 to stay awake and loop the cycle (no deep sleep) so USB serial stays
 * up and the board is easy to reflash. */
/* #define DEV_SLEEP_S        60 */

/* Ignore the SHA-256 dedup: re-download and repaint every cycle, even when the
 * frame URL has not changed. */
/* #define DEV_FORCE_REPAINT  1 */

/* Bypass the retained MQTT frame topic and fetch this fixed URL instead (handy
 * before the server publishes a retained frame for this device). */
/* #define DEV_FRAME_URL      "http://192.168.1.50:8765/renders/abc.bin" */

/* Force the provisioning portal (setup AP) on boot even when WiFi creds exist,
 * for testing the captive portal / splash. */
/* #define DEV_FORCE_PORTAL   1 */
