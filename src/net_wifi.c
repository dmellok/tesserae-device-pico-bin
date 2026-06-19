/*
 * net_wifi.c: CYW43 + lwIP station connectivity. See net_wifi.h.
 */
#include "net_wifi.h"

#include <stdio.h>
#include "pico/cyw43_arch.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"

static bool s_inited = false;

bool wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms)
{
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
    printf("wifi: connecting to '%s'...\n", ssid);

    int r = cyw43_arch_wifi_connect_timeout_ms(ssid, have_pass ? pass : NULL,
                                               auth, timeout_ms);
    if (r != 0) {
        printf("wifi: connect failed (%d)\n", r);
        return false;
    }

    char ip[16];
    wifi_get_ip(ip, sizeof ip);
    printf("wifi: connected, ip=%s rssi=%d\n", ip, wifi_rssi());
    return true;
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

void wifi_stop(void)
{
    if (s_inited) {
        cyw43_arch_deinit();
        s_inited = false;
    }
}
