/*
 * net_http.h: minimal HTTP GET into a caller-provided buffer, on lwIP's HTTP
 * client app. Plain HTTP only (no TLS). WiFi must be up.
 *
 * Used to download a Tesserae frame (.bin) into the PSRAM/SRAM frame buffer.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* GET `url` (http://host[:port]/path) into dst, up to cap bytes. On success
 * sets *out_len to the number of body bytes received. Returns true only on a
 * 2xx response whose body fit entirely in cap. */
bool http_get(const char *url, uint8_t *dst, size_t cap, size_t *out_len,
              uint32_t timeout_ms);
