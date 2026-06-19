/*
 * net_sntp.c: drive lwIP's SNTP client to one fix, then stop. See net_sntp.h.
 *
 * lwIP delivers the time through the SNTP_SET_SYSTEM_TIME(sec) hook, which we
 * define in lwipopts.h to store the epoch into the two globals below (an
 * assignment, so no cross-file prototype to keep in sync). We poll the flag,
 * then hand the epoch to the sleep manager's wall-clock.
 */
#include "net_sntp.h"

#include <stdio.h>

#include "pico/cyw43_arch.h"
#include "lwip/apps/sntp.h"

#include "sleepmgr.h"

/* Written by the SNTP_SET_SYSTEM_TIME hook (see lwipopts.h). */
volatile unsigned long g_sntp_epoch;
volatile int           g_sntp_got;

bool sntp_sync(uint32_t timeout_ms)
{
    g_sntp_got = 0;

    cyw43_arch_lwip_begin();
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
    cyw43_arch_lwip_end();

    printf("sntp: syncing...\n");
    uint32_t t = 0;
    while (!g_sntp_got && t < timeout_ms) { sleep_ms(50); t += 50; }

    cyw43_arch_lwip_begin();
    sntp_stop();
    cyw43_arch_lwip_end();

    if (!g_sntp_got) { printf("sntp: no response (%ums)\n", (unsigned)t); return false; }
    sleep_set_epoch((uint32_t)g_sntp_epoch);
    printf("sntp: epoch=%lu\n", g_sntp_epoch);
    return true;
}
