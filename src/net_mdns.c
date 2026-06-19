/*
 * net_mdns.c: mDNS responder bring-up on lwIP's mDNS app. See net_mdns.h.
 */
#include "net_mdns.h"

#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "pico/unique_id.h"
#include "lwip/apps/mdns.h"
#include "lwip/netif.h"

static bool s_advertised;

#if LWIP_MDNS_RESPONDER
static void txt_cb(struct mdns_service *service, void *txt_userdata)
{
    (void)txt_userdata;
    mdns_resp_add_service_txtitem(service, "path=/", 6);
}
#endif

void mdns_advertise(void)
{
#if LWIP_MDNS_RESPONDER
    if (s_advertised) return;

    struct netif *netif = netif_default;
    if (netif == NULL) { printf("mdns: no default netif\n"); return; }

    pico_unique_board_id_t bid;
    pico_get_unique_board_id(&bid);
    static char host[24];
    snprintf(host, sizeof host, "tesserae-pico-%02x%02x",
             bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
             bid.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);

    cyw43_arch_lwip_begin();
    static bool inited;
    if (!inited) { mdns_resp_init(); inited = true; }
    err_t e = mdns_resp_add_netif(netif, host);
    if (e == ERR_OK)
        mdns_resp_add_service(netif, host, "_http", DNSSD_PROTO_TCP, 80, txt_cb, NULL);
    cyw43_arch_lwip_end();

    if (e == ERR_OK) {
        s_advertised = true;
        printf("mdns: advertising %s.local\n", host);
    } else {
        printf("mdns: add_netif failed (%d)\n", e);
    }
#endif
}
