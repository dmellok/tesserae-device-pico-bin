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

/* MQTT broker and identity. */
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
