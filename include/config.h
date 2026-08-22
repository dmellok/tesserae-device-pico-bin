/*
 * config.h: persistent device configuration, stored in a reserved flash sector.
 *
 * Replaces the ESP32 client's NVS. Holds the same surface: WiFi credentials,
 * MQTT broker/identity, REST identity, the cloud-relay pairing, and runtime
 * state (last-painted hash, sleep interval). The whole config is a single fixed
 * struct, loaded into RAM at boot and written back on change.
 *
 * Flash-write safety: config_save() erases/programs a flash sector with
 * interrupts disabled, so it must be called with the radio down (e.g. at boot
 * before wifi_connect, or during provisioning). Do not call it while WiFi is up.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Cloud-relay key material sizes (mirrors relay_crypto.h so config.h stays
 * standalone; the two are static-asserted together in relay_crypto.c). */
#define CONFIG_RELAY_PRIV_LEN  32
#define CONFIG_RELAY_KEY_LEN   32

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

    /* WiFi fast-connect hint: the BSSID + channel we last associated with.
     * Targeting them directly skips the all-channel scan on a wake, which is
     * the single biggest slice of radio-on time in a cycle. Cleared whenever a
     * targeted join fails, so a roamed/moved AP self-heals via a full scan. */
    uint8_t ap_bssid[6];
    uint8_t ap_channel;
    uint8_t ap_hint;                /* 1 when ap_bssid/ap_channel are valid */

    /* IANA timezone published by the server (proto v2 status contract). Empty
     * until the server delivers one; echoed back on every heartbeat. */
    char    tz[40];

    /* Kiosk mode: never deep-sleep, run the cycle on a timer instead.
     *
     * A hardware fact plus an operator choice. The board CAN stay awake, and on
     * USB power there is no reason not to: it makes the panel reachable (serial,
     * mDNS, the 1200-baud flash touch) between refreshes instead of dark for
     * minutes at a time. On battery it is ruinous, which is why it is opt-in and
     * not merely "sleep_s <= 0". Settable from the server (relay config
     * always_on) or seeded from secrets.h. */
    uint8_t always_on;

    /* ---- cloud relay (see relay.h) ----
     * A relay panel talks only to the relay mailbox and needs no server_url.
     * relay_priv is held ONLY between the pairing POST and the key derivation,
     * then wiped: once relay_key exists the scalar is dead weight and a
     * liability. */
    char    relay_url[160];         /* e.g. https://relay.tesserae.ink */
    char    relay_code[24];         /* single-use pairing code, cleared on success */
    char    relay_install[64];      /* mailbox install id  */
    char    relay_device[64];       /* mailbox device id   */
    char    relay_token[256];       /* mailbox bearer token */
    char    relay_etag[80];         /* last painted frame's ETag */
    char    relay_config_etag[80];  /* last applied config document's ETag */
    uint8_t relay_priv[CONFIG_RELAY_PRIV_LEN];
    uint8_t relay_key[CONFIG_RELAY_KEY_LEN];
    uint8_t relay_have_priv;        /* 1 while the X25519 scalar is held */
    uint8_t relay_paired;           /* 1 once install/device/token/key are all set */
    /* Monotonic ms (POWMAN timer) when this pairing attempt began, so a code can
     * be given a generous wall-clock window instead of a retry count. 0 = not
     * started. See RELAY_PAIR_WINDOW_MS in relay.h. */
    uint64_t relay_pair_started_ms;
} config_t;

/* Load config from flash into the in-RAM copy. Returns true if a valid saved
 * config was found; false if flash was blank/corrupt (RAM copy is then zeroed
 * with sleep_s defaulted). */
bool config_load(void);

/* Write the in-RAM copy back to flash. Radio must be down. Returns true on a
 * successful program+verify. */
bool config_save(void);

/* Erase the stored config so the next boot comes up blank (factory reset).
 * Radio must be down; does not touch the in-RAM copy. */
bool config_erase(void);

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

/* Set the device id (shared by config_device_id() and the MQTT topics). The REST
 * client calls this to adopt the canonical id the server returns from discover/
 * register, so frame/status URLs match the token. */
void config_set_device_id(const char *id);
void config_set_device_token(const char *token);
void config_set_pairing_code(const char *code);
void config_set_frame_etag(const char *etag);

/* Timezone published by the server (proto v2). Empty clears it. */
void config_set_tz(const char *tz);

/* Kiosk mode. Returns true if the value changed (so the caller knows to save). */
bool config_set_always_on(bool on);

/* WiFi fast-connect hint. config_set_ap_hint stores the BSSID + channel of a
 * successful association; config_clear_ap_hint drops a hint that failed. Both
 * are no-ops when the value is unchanged, so a steady-state wake never dirties
 * the config and never pays a flash erase. Returns true if anything changed. */
bool config_set_ap_hint(const uint8_t bssid[6], uint8_t channel);
bool config_clear_ap_hint(void);

/* ---- cloud relay ---- */

void config_set_relay_url(const char *url);
void config_set_relay_code(const char *code);

/* Persist the X25519 scalar between the pairing POST and the key derivation.
 * Also stamps relay_pair_started_ms, since submitting the code is what starts
 * the pairing window. */
void config_set_relay_priv(const uint8_t priv[CONFIG_RELAY_PRIV_LEN],
                           uint64_t started_ms);

/* Re-stamp the pairing window's start. Used when the monotonic clock has
 * obviously jumped (the wall clock being set moves the same POWMAN timer), so a
 * pairing is never expired by a clock correction. */
void config_set_relay_pair_started(uint64_t started_ms);

/* Adopt a completed pairing: mailbox identity + derived frame key. Wipes the
 * private scalar, which is no longer needed and must not outlive the exchange. */
void config_set_relay_paired(const char *install_id, const char *device_id,
                             const char *token,
                             const uint8_t key[CONFIG_RELAY_KEY_LEN]);

void config_set_relay_etag(const char *etag);
void config_set_relay_config_etag(const char *etag);

/* Drop every piece of relay pairing state (code, scalar, identity, key, etags).
 * relay_url survives: it is operator configuration, not pairing state. */
void config_clear_relay(void);

/* True once the device holds a complete mailbox identity and frame key. */
bool config_relay_ready(void);

/* Convenience: true if a WiFi SSID is configured. */
bool config_has_wifi(void);

/* True if a REST server URL is configured (selects REST over MQTT). */
bool config_has_server(void);

/* True if a relay URL is configured (the device may be paired or still pairing). */
bool config_has_relay(void);

/* Effective device id: the configured mqtt_device_id if set, otherwise a stable
 * default "<board model>_<unique-id suffix>". Cached; never NULL. */
const char *config_device_id(void);
