/*
 * config.h: persistent device configuration, stored in a reserved flash sector.
 *
 * Replaces the ESP32 client's NVS. Holds the same surface: WiFi credentials,
 * MQTT broker/identity, and runtime state (last-painted hash, sleep interval).
 * The whole config is a single fixed struct, loaded into RAM at boot and
 * written back on change.
 *
 * Flash-write safety: config_save() erases/programs a flash sector with
 * interrupts disabled, so it must be called with the radio down (e.g. at boot
 * before wifi_connect, or during provisioning). Do not call it while WiFi is up.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Field sizes mirror the ESP32 NVS limits (+1 for NUL). */
typedef struct {
    char    wifi_ssid[33];
    char    wifi_pass[65];
    char    mqtt_uri[160];
    char    mqtt_device_id[33];
    char    mqtt_user[64];
    char    mqtt_pass[64];
    char    last_hash[65];     /* 64 hex chars of the last painted URL's SHA-256 */
    int32_t sleep_s;           /* deep-sleep interval, seconds */
    /* REST transport (v2). When server_url is set the device uses the Tesserae
     * REST API instead of MQTT (see net_rest.h). */
    char    server_url[160];        /* e.g. http://tesserae.local:8765 */
    char    device_token[256];      /* bearer token, set after /device/register */
    char    pairing_code[16];       /* set pre-register, cleared on success */
    char    last_frame_etag[80];    /* cached across wakes for If-None-Match */
} config_t;

/* Load config from flash into the in-RAM copy. Returns true if a valid saved
 * config was found; false if flash was blank/corrupt (RAM copy is then zeroed
 * with sleep_s defaulted). */
bool config_load(void);

/* Write the in-RAM copy back to flash. Radio must be down. Returns true on a
 * successful program+verify. */
bool config_save(void);

/* Accessors (return the in-RAM copy; never NULL). */
const config_t *config_get(void);

/* Mutators (update the in-RAM copy; call config_save() to persist). NULL is
 * treated as "leave unchanged"; empty string clears the field. */
void config_set_wifi(const char *ssid, const char *pass);
void config_set_mqtt(const char *uri, const char *device_id,
                     const char *user, const char *pass);
void config_set_sleep_s(int32_t s);
void config_set_last_hash(const char *hex);

/* REST transport mutators (v2). Same NULL/empty semantics as above. */
void config_set_server(const char *url);
void config_set_device_token(const char *token);
void config_set_pairing_code(const char *code);
void config_set_frame_etag(const char *etag);

/* Convenience: true if a WiFi SSID is configured. */
bool config_has_wifi(void);

/* True if a REST server URL is configured (selects REST over MQTT). */
bool config_has_server(void);

/* Effective device id: the configured mqtt_device_id if set, otherwise a stable
 * default "pico_" + hex(unique board id). Cached; never NULL. */
const char *config_device_id(void);
