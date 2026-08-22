/*
 * net_wifi.h: WiFi station connectivity via the CYW43 + lwIP stack.
 *
 * Replaces the ESP32 client's esp_wifi/esp_netif layer. Threadsafe-background
 * mode (no RTOS); cyw43_arch services lwIP in the background, so the connect
 * call is synchronous from the caller's point of view.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Bring up CYW43 (once) and join an AP in station mode. pass may be NULL/empty
 * for an open network. Returns true on association + DHCP lease.
 *
 * Fast path: if config holds a BSSID + channel hint from the last successful
 * join (config_set_ap_hint), the first attempt targets that AP directly, which
 * skips the all-channel scan the driver otherwise runs. A stale hint (the AP
 * moved channel, or we are somewhere else entirely) fails quickly and is
 * cleared before falling back to a normal scan-and-join, so the hint can never
 * strand the device. On success the hint is refreshed from the AP we actually
 * associated with. Marks *dirty when the stored hint changed and the caller
 * should persist config; pass NULL if you do not care. */
bool wifi_connect(const char *ssid, const char *pass, uint32_t timeout_ms,
                  bool *dirty);

/* Dotted IPv4 of the station, or "0.0.0.0" if not up. out must be >= 16 bytes. */
void wifi_get_ip(char *out, size_t n);

/* Current AP RSSI in dBm (0 if unavailable). */
int wifi_rssi(void);

/* Station MAC as "aa:bb:cc:dd:ee:ff". out must be >= 18 bytes. */
void wifi_get_mac(char *out, size_t n);

/* Power down the radio (the big pre-paint power saving). A later wifi_connect()
 * re-initialises it. */
void wifi_stop(void);
