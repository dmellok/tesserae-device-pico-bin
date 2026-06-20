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
#define CONFIG_VERSION        5u                    /* bump to force-wipe stale config -> portal */
#define DEFAULT_SLEEP_S       60

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

bool config_has_wifi(void)   { return s_cfg.wifi_ssid[0]  != '\0'; }
bool config_has_server(void) { return s_cfg.server_url[0] != '\0'; }

const char *config_device_id(void)
{
    if (s_cfg.mqtt_device_id[0] != '\0') return s_cfg.mqtt_device_id;

    /* Stable default derived from the RP2350 unique board id, computed once. */
    static char s_devid[6 + 2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    if (s_devid[0] == '\0') {
        pico_unique_board_id_t bid;
        pico_get_unique_board_id(&bid);
        int n = snprintf(s_devid, sizeof s_devid, "pico_");
        for (int i = 0; i < PICO_UNIQUE_BOARD_ID_SIZE_BYTES &&
                        n < (int)sizeof s_devid - 2; i++)
            n += snprintf(s_devid + n, sizeof s_devid - n, "%02x", bid.id[i]);
    }
    return s_devid;
}
