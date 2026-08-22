/*
 * relay.h: Tesserae cloud-relay transport (remote panels).
 *
 * Lets a panel live somewhere the home instance is not reachable from. Both
 * ends talk *outbound* to a small Worker that is a per-device mailbox: home
 * seals each rendered frame and PUTs it, the panel polls and decrypts it. The
 * relay is zero-knowledge -- it holds ciphertext, both public keys and a token
 * hash, never the frame key.
 *
 * Wire contract: docs/relay/contract.md in the Tesserae repo (authoritative for
 * home, Worker and firmware alike). Crypto is in relay_crypto.[ch]; pure wire
 * handling (parsing, URL building) is in relay_wire.[ch].
 *
 * Lifecycle:
 *
 *   unconfigured --set relay_url + code--> pairing --ready--> polling
 *                                             |                  |
 *                                             +-- expired/failed +  (cleared)
 *
 * Pairing may span deep sleeps, so its state lives in the flash config: the
 * panel's X25519 private key is kept only until the frame key is derived, then
 * wiped.
 *
 * This is an ALTERNATIVE to the direct-REST path, not a replacement. A device
 * is either relay-paired or talking to a home server; relay_ready() decides,
 * and main.c prefers the relay when it is ready.
 *
 * Every route is HTTPS, so this path requires the TLS transport (net_tls.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The hosted relay. A self-hosted Worker just sets its own origin, so the two
 * are interchangeable. */
#define RELAY_DEFAULT_URL "https://relay.tesserae.ink"

/* The device kind and palette the panel self-reports at pairing, so "Add a
 * remote panel" collapses to just a device id instead of making the operator
 * retype hardware facts. Wrong answers here are silent: a panel told the wrong
 * kind gets a mis-packed frame and paints garbage, so these must match what the
 * Tesserae hardware catalog knows this board as. */
#define RELAY_DEVICE_MODEL "pico_bin_client"
#define RELAY_DEVICE_GAMUT "spectra_6"

/* How long the device keeps working a pairing code before giving up on it.
 *
 * The two ends of the rendezvous have very different clocks. The relay enforces
 * a short expiry on POST /v1/pair only -- which the firmware satisfies by
 * submitting exactly once, immediately -- while GET /v1/pair/<code> has NO
 * expiry at all, so the panel may poll for as long as the home instance needs
 * to complete its half. That half can be slow: it waits on the operator opening
 * Settings, on the home instance's own poller, and on a panel that may only
 * wake every few minutes.
 *
 * A day is therefore the right patience. Shorter, and a code entered in the
 * evening is dead by morning even though the relay would still honour it;
 * longer, and a genuinely abandoned pairing keeps a stale code on the device
 * indefinitely. Only a 404 (the relay saying the code is gone) or this deadline
 * clears the state. */
#define RELAY_PAIR_WINDOW_MS (24ull * 60ull * 60ull * 1000ull)

/* True when the device holds a complete mailbox identity + frame key and
 * should use the relay instead of the direct REST path. */
bool relay_ready(void);

/* True when a relay URL and pairing code are set but pairing has not completed
 * -- the caller should run relay_pair_step() while the radio is up. */
bool relay_pairing_pending(void);

typedef enum {
    RELAY_PAIR_IDLE = 0,    /* nothing to do (no code, or already paired) */
    RELAY_PAIR_WAITING,     /* submitted; home has not completed it yet */
    RELAY_PAIR_DONE,        /* key derived and persisted; polling can start */
    RELAY_PAIR_EXPIRED,     /* code unknown/expired -- state cleared */
    RELAY_PAIR_ERROR,       /* transient (network/parse); safe to retry */
} relay_pair_result_t;

/* Advance the rendezvous by one step. Radio must be up.
 *
 * First call with a fresh code generates the keypair and POSTs /v1/pair;
 * subsequent calls poll GET /v1/pair/<code>. On "ready" it derives the frame
 * key from the returned home_pubkey, persists everything, and clears the code.
 * Safe to call every wake: it is a no-op once paired.
 *
 * The POST happens EXACTLY ONCE (the persisted private key is the latch),
 * because only that route enforces the code's 10-minute expiry -- re-POSTing
 * later would 404 a code that is still perfectly good for polling. The poll
 * itself has no expiry, so it may span as many wakes as home needs.
 *
 * panel_w/panel_h are the panel's NATIVE landscape dimensions, self-reported so
 * "Add a remote panel" collapses to just a device id. They are runtime values
 * here (the driver is chosen from the model EEPROM at boot), not compile-time
 * constants as on a fixed-panel board; pass 0 for either to omit it, which is
 * better than guessing.
 *
 * *dirty is set when config changed and the caller must config_save() with the
 * radio down. */
relay_pair_result_t relay_pair_step(uint16_t panel_w, uint16_t panel_h,
                                    bool *dirty);

/* Fetch the current frame from the mailbox into `buf` (which must hold
 * `cap` >= sealed size; the caller sizes it from the panel plus
 * RELAY_SEAL_OVERHEAD) and decrypt it IN PLACE.
 *
 *   RELAY_FRAME_NEW       *frame / *len point into buf at the decrypted image.
 *   RELAY_FRAME_UNCHANGED 304 -- keep what is on the glass.
 *   RELAY_FRAME_NONE      204 -- home has not published one yet.
 *   RELAY_FRAME_ERROR     network, auth, or a FAILED AUTHENTICATION TAG. A tag
 *                         failure is never painted: it means the mailbox does
 *                         not match this panel's key, and showing it would
 *                         render whatever an attacker or a corrupted store
 *                         supplied.
 *
 * The contract also exposes panel dimensions as plaintext X-Tesserae-* headers;
 * this deliberately ignores them. The firmware knows its own geometry, and
 * validating the DECRYPTED LENGTH against it is a stronger check than trusting
 * a header the relay could have altered.
 *
 * *dirty is set when config changed (a failed tag drops the cached ETag) and
 * the caller must config_save() with the radio down. */
typedef enum {
    RELAY_FRAME_NEW = 0,
    RELAY_FRAME_UNCHANGED,
    RELAY_FRAME_NONE,
    RELAY_FRAME_ERROR,
} relay_frame_result_t;

relay_frame_result_t relay_fetch_frame(uint8_t *buf, size_t cap,
                                       const uint8_t **frame, size_t *len,
                                       bool *dirty);

/* Commit the ETag of a frame that actually reached the glass. Kept separate
 * from the fetch so a frame that failed to decode or paint is re-fetched next
 * wake rather than skipped by a 304. Sets *dirty when config changed. */
void relay_commit_frame(bool *dirty);

/* True once the relay has answered a device-token route (frame GET, config GET
 * or status POST) with 401 on two consecutive wakes.
 *
 * Revoking a panel deletes its token record on the relay, and re-pairing
 * invalidates the previous token, so 401 is unambiguous: this pairing is gone.
 * It is NOT transient and must never be retried. (204 on the frame GET still
 * means only "paired, nothing published yet" -- keep the image.)
 *
 * Two wakes rather than one is deliberate: unpairing costs the operator a
 * physical trip to type a fresh code, so a single 401 injected by a captive
 * portal or middlebox should not trigger it. */
bool relay_pairing_revoked(void);

/* Drop the stored pairing after relay_pairing_revoked(). The caller keeps the
 * last image on the glass and reopens setup so a fresh code re-pairs cleanly.
 * relay_url survives, being operator configuration rather than pairing state.
 * The caller must config_save() with the radio down. */
void relay_forget_revoked_pairing(void);

/* POST the device's status JSON to the mailbox (battery, RSSI, fw_version ...).
 * Body is the SAME shape a REST client posts to a home /status endpoint, so the
 * home instance can feed it through its normal heartbeat pipeline. Plaintext by
 * contract: operational telemetry, not dashboard content. Best-effort -- a
 * failure only costs the Devices UI a stale last-seen. */
bool relay_post_status(int rssi, const char *ip, uint16_t panel_w,
                       uint16_t panel_h, const char *fw_version);

/* Adopt the device config the home instance published to the config mailbox.
 *
 * Settings edited on the home instance reach a LOCAL device through its next
 * REST poll; a relay panel has no such channel, so without this it would keep
 * its pairing-time sleep interval forever.
 *
 * Call AFTER relay_post_status() on the same wake: that response advertises the
 * current config etag, which lets this skip its request entirely when nothing
 * changed. It is still correct (just less frugal) when status was not posted.
 *
 * The document is sealed with the same frame key as a frame and is applied only
 * once it decrypts, authenticates and parses; the etag is stored last, so any
 * failure is retried next wake instead of being skipped by a 304 forever.
 * Every failure is non-fatal -- the last-known config simply stays in force. */
typedef enum {
    RELAY_CFG_UNCHANGED = 0,  /* 304, or status advertised the etag we hold */
    RELAY_CFG_APPLIED,        /* 200: decrypted, parsed and applied */
    RELAY_CFG_NONE,           /* 204: nothing published yet */
    RELAY_CFG_ERROR,          /* network, auth, or a failed tag; config kept */
} relay_config_result_t;

relay_config_result_t relay_sync_config(bool *dirty);
