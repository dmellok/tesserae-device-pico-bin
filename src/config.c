/*
 * config.c: flash-backed device configuration. See config.h.
 *
 * Layout: a single struct (magic + version + config_t + crc32) lives in one
 * flash sector at CONFIG_FLASH_OFFSET, read via the memory-mapped XIP window
 * and written with flash_range_erase/program. The offset is a fixed 1 MB into
 * flash, far past this firmware (tens of KB of code plus the ~225 KB CYW43
 * blob), and within even a 2 MB part, so it is safe regardless of flash size.
 */
#include "config.h"

#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h"   /* XIP_BASE */

#define CONFIG_FLASH_OFFSET   (1u * 1024 * 1024)   /* 1 MB into flash */
#define CONFIG_MAGIC          0x54434647u           /* "TCFG" */
#define CONFIG_VERSION        8u                    /* bump to force-wipe stale config -> portal */
#define DEFAULT_SLEEP_S       60

/* The stored blob is one flash sector; the struct has to stay well inside it. */
_Static_assert(sizeof(config_t) + 16 <= FLASH_SECTOR_SIZE,
               "config_t no longer fits in one flash sector");

typedef struct {
    uint32_t magic;
    uint32_t version;
    config_t cfg;
    uint32_t crc;            /* crc32 over magic..cfg (everything before crc) */
} stored_t;

static config_t s_cfg;

/* Small table-less CRC32 (poly 0xEDB88320), enough to validate the blob. */
static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

bool config_load(void)
{
    const stored_t *f = (const stored_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    if (f->magic == CONFIG_MAGIC && f->version == CONFIG_VERSION &&
        f->crc == crc32(f, offsetof(stored_t, crc))) {
        s_cfg = f->cfg;
        return true;
    }
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.sleep_s = DEFAULT_SLEEP_S;
    return false;
}

bool config_save(void)
{
    stored_t st;
    memset(&st, 0, sizeof st);
    st.magic = CONFIG_MAGIC;
    st.version = CONFIG_VERSION;
    st.cfg = s_cfg;
    st.crc = crc32(&st, offsetof(stored_t, crc));

    /* Program buffer padded up to a flash-page multiple. */
    static uint8_t page[((sizeof(stored_t) + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE) * FLASH_PAGE_SIZE];
    memset(page, 0xFF, sizeof page);
    memcpy(page, &st, sizeof st);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(CONFIG_FLASH_OFFSET, page, sizeof page);
    restore_interrupts(ints);

    /* Verify by reading back through XIP. */
    const stored_t *f = (const stored_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    return (f->magic == CONFIG_MAGIC && f->crc == crc32(f, offsetof(stored_t, crc)));
}

bool config_erase(void)
{
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(CONFIG_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);

    const stored_t *f = (const stored_t *)(XIP_BASE + CONFIG_FLASH_OFFSET);
    return f->magic != CONFIG_MAGIC;   /* erased flash reads 0xFF */
}

const config_t *config_get(void) { return &s_cfg; }

static void set_str(char *dst, size_t cap, const char *src)
{
    if (src == NULL) return;                /* leave unchanged */
    snprintf(dst, cap, "%s", src);
}

void config_set_wifi(const char *ssid, const char *pass)
{
    set_str(s_cfg.wifi_ssid, sizeof s_cfg.wifi_ssid, ssid);
    set_str(s_cfg.wifi_pass, sizeof s_cfg.wifi_pass, pass);
}

void config_set_mqtt(const char *uri, const char *device_id,
                     const char *user, const char *pass)
{
    set_str(s_cfg.mqtt_uri,       sizeof s_cfg.mqtt_uri,       uri);
    set_str(s_cfg.mqtt_device_id, sizeof s_cfg.mqtt_device_id, device_id);
    set_str(s_cfg.mqtt_user,      sizeof s_cfg.mqtt_user,      user);
    set_str(s_cfg.mqtt_pass,      sizeof s_cfg.mqtt_pass,      pass);
}

void config_set_sleep_s(int32_t s) { s_cfg.sleep_s = s; }

void config_set_last_hash(const char *hex)
{
    set_str(s_cfg.last_hash, sizeof s_cfg.last_hash, hex);
}

void config_set_server(const char *url)
{
    set_str(s_cfg.server_url, sizeof s_cfg.server_url, url);
}

void config_set_device_id(const char *id)
{
    set_str(s_cfg.mqtt_device_id, sizeof s_cfg.mqtt_device_id, id);
}

void config_set_device_token(const char *token)
{
    set_str(s_cfg.device_token, sizeof s_cfg.device_token, token);
}

void config_set_pairing_code(const char *code)
{
    set_str(s_cfg.pairing_code, sizeof s_cfg.pairing_code, code);
}

void config_set_frame_etag(const char *etag)
{
    set_str(s_cfg.last_frame_etag, sizeof s_cfg.last_frame_etag, etag);
}

void config_set_tz(const char *tz)
{
    set_str(s_cfg.tz, sizeof s_cfg.tz, tz);
}

bool config_set_ap_hint(const uint8_t bssid[6], uint8_t channel)
{
    if (bssid == NULL) return false;
    if (s_cfg.ap_hint && s_cfg.ap_channel == channel &&
        memcmp(s_cfg.ap_bssid, bssid, 6) == 0)
        return false;                      /* unchanged: do not dirty the config */
    memcpy(s_cfg.ap_bssid, bssid, 6);
    s_cfg.ap_channel = channel;
    s_cfg.ap_hint = 1;
    return true;
}

bool config_clear_ap_hint(void)
{
    if (!s_cfg.ap_hint) return false;
    memset(s_cfg.ap_bssid, 0, sizeof s_cfg.ap_bssid);
    s_cfg.ap_channel = 0;
    s_cfg.ap_hint = 0;
    return true;
}

/* ---- cloud relay ---- */

void config_set_relay_url(const char *url)
{
    set_str(s_cfg.relay_url, sizeof s_cfg.relay_url, url);
}

void config_set_relay_code(const char *code)
{
    set_str(s_cfg.relay_code, sizeof s_cfg.relay_code, code);
}

void config_set_relay_priv(const uint8_t priv[CONFIG_RELAY_PRIV_LEN])
{
    if (priv == NULL) return;
    memcpy(s_cfg.relay_priv, priv, CONFIG_RELAY_PRIV_LEN);
    s_cfg.relay_have_priv = 1;
}

void config_set_relay_paired(const char *install_id, const char *device_id,
                             const char *token,
                             const uint8_t key[CONFIG_RELAY_KEY_LEN])
{
    set_str(s_cfg.relay_install, sizeof s_cfg.relay_install, install_id);
    set_str(s_cfg.relay_device,  sizeof s_cfg.relay_device,  device_id);
    set_str(s_cfg.relay_token,   sizeof s_cfg.relay_token,   token);
    if (key != NULL) memcpy(s_cfg.relay_key, key, CONFIG_RELAY_KEY_LEN);

    /* The scalar has done its only job. Wiping it here (rather than at the next
     * save) means a config dump, a flash read or a stolen board never yields a
     * key that is still live for this mailbox. */
    memset(s_cfg.relay_priv, 0, sizeof s_cfg.relay_priv);
    s_cfg.relay_have_priv = 0;
    s_cfg.relay_code[0] = '\0';            /* single-use; consumed */
    s_cfg.relay_paired = 1;
}

void config_set_relay_etag(const char *etag)
{
    set_str(s_cfg.relay_etag, sizeof s_cfg.relay_etag, etag);
}

void config_set_relay_config_etag(const char *etag)
{
    set_str(s_cfg.relay_config_etag, sizeof s_cfg.relay_config_etag, etag);
}

void config_clear_relay(void)
{
    s_cfg.relay_code[0] = '\0';
    s_cfg.relay_install[0] = '\0';
    s_cfg.relay_device[0] = '\0';
    s_cfg.relay_token[0] = '\0';
    s_cfg.relay_etag[0] = '\0';
    s_cfg.relay_config_etag[0] = '\0';
    memset(s_cfg.relay_priv, 0, sizeof s_cfg.relay_priv);
    memset(s_cfg.relay_key,  0, sizeof s_cfg.relay_key);
    s_cfg.relay_have_priv = 0;
    s_cfg.relay_paired = 0;
    /* relay_url is operator configuration, not pairing state: it survives. */
}

bool config_relay_ready(void)
{
    return s_cfg.relay_paired && s_cfg.relay_install[0] && s_cfg.relay_device[0] &&
           s_cfg.relay_token[0];
}

bool config_has_wifi(void)   { return s_cfg.wifi_ssid[0]  != '\0'; }
bool config_has_server(void) { return s_cfg.server_url[0] != '\0'; }
bool config_has_relay(void)  { return s_cfg.relay_url[0]  != '\0'; }

const char *config_device_id(void)
{
    if (s_cfg.mqtt_device_id[0] != '\0') return s_cfg.mqtt_device_id;

    /* Default id is "<board model>_<suffix>", matching the ecosystem convention
     * (tesserae-device-firmware commit e21a8f9). The suffix comes from the
     * RP2350 unique board id rather than the MAC: the CYW43 interface MACs read
     * zero until the radio is up, and this is called at boot for logging. Only
     * a first-boot placeholder in any case, since discover/register replaces it
     * with the server's canonical MAC-matched id. */
    static char s_devid[sizeof "pico_plus_2w_" + 6];
    if (s_devid[0] == '\0') {
        pico_unique_board_id_t bid;
        pico_get_unique_board_id(&bid);
        snprintf(s_devid, sizeof s_devid, "pico_plus_2w_%02x%02x%02x",
                 bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 3],
                 bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
                 bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
    }
    return s_devid;
}
