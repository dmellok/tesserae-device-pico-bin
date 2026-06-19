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

/* Development overrides (off by default). Handy while iterating on hardware:
 *   DEV_SLEEP_S      pin the sleep interval, overriding the broker/flash value.
 *                    Set 0 to stay awake and loop the cycle (no deep sleep), so
 *                    USB serial stays up and the board is easy to reflash.
 *   DEV_FORCE_REPAINT  ignore the SHA-256 dedup and re-download + repaint every
 *                    cycle (useful when the frame URL has not changed).
 *   DEV_FRAME_URL    bypass the retained MQTT topic and fetch a fixed frame URL
 *                    (string literal). Handy before the server publishes a
 *                    retained frame for this device. e.g.
 *                    -DDEV_FRAME_URL=\"http://host:8765/renders/abc.bin\"
 *   DEV_FORCE_PORTAL force the provisioning portal (setup AP) on boot even when
 *                    WiFi creds exist, for testing the captive portal.
 * Can also be passed as build flags, e.g.
 *   PLATFORMIO_BUILD_FLAGS="-DDEV_SLEEP_S=0 -DDEV_FORCE_REPAINT" pio run -t upload
 */
/* #define DEV_SLEEP_S       0 */
/* #define DEV_FORCE_REPAINT 1 */
