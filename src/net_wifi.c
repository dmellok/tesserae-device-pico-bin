/*
 * net_wifi.c: CYW43 + lwIP station connectivity. See net_wifi.h.
 */
#include "net_wifi.h"

#include <stdio.h>
#include <string.h>
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

#include "config.h"

static bool s_inited = false;

/* Attempts for the ordinary scan-and-join path. The CYW43 intermittently
 * returns -8 (link fail) on the first attempt after a fresh init, especially
 * with the deinit/reinit we do each wake, so a couple of quick retries turn
 * those transient misses into a connect instead of a skipped (and eventually
 * portal-triggering) cycle. */
#define SCAN_ATTEMPTS  3

/* The targeted attempt gets a shorter budget than the full one: its whole point
 * is to be quick, and a hint that is going to fail should not eat the caller's
 * timeout before the scan fallback gets a turn. */
#define FAST_TIMEOUT_MS  6000

/* Join and wait for a DHCP lease, mirroring cyw43_arch_wifi_connect_*_until()
 * but passing a channel through -- which is the part that actually skips the
 * scan, and which the arch wrappers hard-code to CYW43_CHANNEL_NONE. */
static int join_until(const char *ssid, const char *pass, uint32_t auth,
                      const uint8_t *bssid, uint32_t channel,
                      absolute_time_t until)
{
    size_t slen = strlen(ssid);
    size_t plen = (pass != NULL) ? strlen(pass) : 0;

    int err = cyw43_wifi_join(&cyw43_state, slen, (const uint8_t *)ssid,
                              plen, (const uint8_t *)pass, auth, bssid, channel);
    if (err) return err;

    int status = CYW43_LINK_UP + 1;
    while (status >= 0 && status != CYW43_LINK_UP) {
        int now = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
        if (now == CYW43_LINK_NONET) {
            /* "No network" against a targeted BSSID/channel means the hint is
             * wrong. Report it rather than re-joining in a loop: the caller
             * clears the hint and falls back to a full scan, which is the only
             * thing that can actually find a moved AP. */
            if (bssid != NULL) return PICO_ERROR_CONNECT_FAILED;
            now = CYW43_LINK_JOIN;
            err = cyw43_wifi_join(&cyw43_state, slen, (const uint8_t *)ssid,
                                  plen, (const uint8_t *)pass, auth, NULL,
                                  CYW43_CHANNEL_NONE);
            if (err) return err;
        }
        status = now;
        /* Test for success BEFORE the deadline. cyw43_arch_wait_for_work_until
         * returns exactly at the deadline, so the common "DHCP landed during
         * the final wait" case would otherwise be reported as a timeout on a
         * link that is up -- which here means clearing a perfectly good AP hint
         * and running three more full-scan joins for nothing. */
        if (status == CYW43_LINK_UP) break;
        if (time_reached(until)) return PICO_ERROR_TIMEOUT;
        cyw43_arch_poll();
        cyw43_arch_wait_for_work_until(until);
    }
    if (status == CYW43_LINK_UP)      return PICO_OK;
    if (status == CYW43_LINK_BADAUTH) return PICO_ERROR_BADAUTH;
    return PICO_ERROR_CONNECT_FAILED;
}

/* Record the AP we ended up on, so the next wake can target it.
 *
 * The CYW43 driver exposes the associated BSSID but not the associated
 * CHANNEL (cyw43_t tracks a channel for AP mode only), so the channel can only
 * come from a scan. The portal records it for the network the operator picks
 * (net_portal.c), and it stays valid as long as we keep landing on the same
 * BSSID. Associating with a DIFFERENT BSSID -- a mesh node handoff, or a
 * genuinely different network -- invalidates it, so the channel is dropped and
 * the next fast attempt goes out band-agnostic rather than aimed at the wrong
 * band. An unknown channel still leaves a directed join, which is most of the
 * saving. */
static bool remember_ap(void)
{
    uint8_t bssid[6] = {0};
    if (cyw43_wifi_get_bssid(&cyw43_state, bssid) != 0) return false;

    const config_t *c = config_get();
    bool same = c->ap_hint && memcmp(c->ap_bssid, bssid, 6) == 0;
    return config_set_ap_hint(bssid, same ? c->ap_channel : 0);
}

bool wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms,
                  bool *dirty)
{
    if (dirty != NULL) *dirty = false;

    if (!s_inited) {
        if (cyw43_arch_init_with_country(CYW43_COUNTRY_WORLDWIDE) != 0) {
            printf("wifi: cyw43 init failed\n");
            return false;
        }
        s_inited = true;
    }
    cyw43_arch_enable_sta_mode();

    bool have_pass = (pass != NULL && pass[0] != '\0');
    uint32_t auth = have_pass ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
    const char *pw = have_pass ? pass : NULL;

    /* Fast path: target the AP we last associated with. */
    const config_t *c = config_get();
    if (c->ap_hint) {
        uint32_t chan = c->ap_channel ? c->ap_channel : CYW43_CHANNEL_NONE;
        printf("wifi: fast connect to '%s' (%02x:%02x:%02x:%02x:%02x:%02x ch %u)\n",
               ssid, c->ap_bssid[0], c->ap_bssid[1], c->ap_bssid[2],
               c->ap_bssid[3], c->ap_bssid[4], c->ap_bssid[5],
               (unsigned)c->ap_channel);
        uint32_t budget = (timeout_ms < FAST_TIMEOUT_MS) ? timeout_ms : FAST_TIMEOUT_MS;
        int r = join_until(ssid, pw, auth, c->ap_bssid, chan,
                           make_timeout_time_ms(budget));
        if (r == 0) {
            char ip[16];
            wifi_get_ip(ip, sizeof ip);
            printf("wifi: connected (fast), ip=%s rssi=%d\n", ip, wifi_rssi());
            if (remember_ap() && dirty != NULL) *dirty = true;
            return true;
        }
        printf("wifi: fast connect failed (%d); clearing hint and scanning\n", r);
        if (config_clear_ap_hint() && dirty != NULL) *dirty = true;
    }

    for (int a = 1; a <= SCAN_ATTEMPTS; a++) {
        printf("wifi: connecting to '%s' (attempt %d/%d)...\n", ssid, a, SCAN_ATTEMPTS);
        int r = join_until(ssid, pw, auth, NULL, CYW43_CHANNEL_NONE,
                           make_timeout_time_ms(timeout_ms));
        if (r == 0) {
            char ip[16];
            wifi_get_ip(ip, sizeof ip);
            printf("wifi: connected, ip=%s rssi=%d\n", ip, wifi_rssi());
            if (remember_ap() && dirty != NULL) *dirty = true;
            return true;
        }
        printf("wifi: connect failed (%d)%s\n", r, a < SCAN_ATTEMPTS ? "; retrying" : "");
        if (a < SCAN_ATTEMPTS) sleep_ms(1000);
    }
    return false;
}

void wifi_get_ip(char *out, size_t n)
{
    const ip4_addr_t *ip = (netif_default != NULL) ? netif_ip4_addr(netif_default) : NULL;
    snprintf(out, n, "%s", (ip != NULL) ? ip4addr_ntoa(ip) : "0.0.0.0");
}

int wifi_rssi(void)
{
    int32_t rssi = 0;
    if (cyw43_wifi_get_rssi(&cyw43_state, &rssi) != 0) return 0;
    return (int)rssi;
}

void wifi_get_mac(char *out, size_t n)
{
    uint8_t mac[6] = {0};
    cyw43_wifi_get_mac(&cyw43_state, CYW43_ITF_STA, mac);
    snprintf(out, n, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void wifi_stop(void)
{
    if (s_inited) {
        cyw43_arch_deinit();
        s_inited = false;
    }
}
