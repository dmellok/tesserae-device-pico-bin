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
