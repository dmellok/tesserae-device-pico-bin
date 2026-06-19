/*
 * net_mqtt.h: MQTT client for the Tesserae protocol, on lwIP's MQTT app.
 *
 * Mirrors the ESP32 client's topic layout under tesserae/<device_id>/:
 *   frame/bin  (subscribe, retained)  -> {"url":"http://.../x.bin"} or bare URL
 *   config     (subscribe, retained)  -> {"sleep_interval_s":900}
 *   status     (publish,  retained)   -> heartbeat JSON; LWT {"state":"offline"}
 *
 * WiFi must be up (cyw43_arch initialised) before calling these. They are
 * synchronous wrappers over lwIP's async MQTT (threadsafe-background mode).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Connect to the broker, subscribe to frame/bin + config, and wait up to
 * timeout_ms for retained messages. On a frame message the URL is copied into
 * url_out; on a config message *sleep_out is set to the requested interval
 * (left untouched if none arrives). Returns true if a frame URL was received. */
bool mqtt_fetch_retained(const char *uri, const char *device_id,
                         const char *user, const char *pass,
                         char *url_out, size_t url_cap,
                         int32_t *sleep_out, uint32_t timeout_ms);

/* Connect and publish a retained status payload to .../status, waiting for the
 * PUBACK. Returns true if the publish was acknowledged. */
bool mqtt_publish_status(const char *uri, const char *device_id,
                         const char *user, const char *pass,
                         const char *json, uint32_t timeout_ms);
