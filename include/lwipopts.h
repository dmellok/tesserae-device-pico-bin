/*
 * lwipopts.h: lwIP configuration for the CYW43 WiFi stack.
 *
 * NO_SYS / threadsafe-background mode (no RTOS), matching how the picosdk
 * framework builds pico_cyw43_arch here. Based on the canonical pico-examples
 * "examples_common" lwIP options, extended for the apps this firmware uses
 * (DNS, SNTP, mDNS, MQTT, HTTPD). App source files are added to the build as
 * each feature lands; the options here are harmless until then.
 */
#ifndef _LWIPOPTS_H
#define _LWIPOPTS_H

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    16000
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1
#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0
#define LWIP_CHKSUM_ALGORITHM       3
#define LWIP_DHCP                   1
#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0
#define LWIP_IGMP                   1   /* needed for mDNS multicast */
#define SO_REUSE                    1
#define LWIP_ALTCP                  1   /* lwIP HTTP client is built on altcp */
#define LWIP_ALTCP_TLS              0   /* HTTP only for now, no TLS */

/* extra timeouts headroom for the apps (DHCP/DNS/SNTP/MQTT/mDNS timers) */
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 8)

#define LWIP_DEBUG                  0

#endif /* _LWIPOPTS_H */
