/*
 * net_mdns.h: advertise the device on the LAN over mDNS.
 *
 * Publishes "tesserae-pico-XXXX.local" (suffix from the flash unique id) with
 * an _http._tcp service on port 80, so the device is discoverable by name while
 * it is connected (or running the setup AP). Idempotent: only advertises once
 * per boot.
 */
#pragma once

/* Advertise on the current default netif. Safe to call after WiFi connects or
 * after the portal AP comes up; later calls are no-ops. */
void mdns_advertise(void);
