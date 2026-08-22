# Third-party code

This firmware is licensed AGPL-3.0-or-later (see `LICENSE`). It bundles the
following third-party components, each under its own (permissive) license.
Original license headers are kept intact in the vendored files.

## DHCP server — `src/vendor/dhcpserver.c`, `include/dhcpserver.h`
- Source: Raspberry Pi `pico-examples`, `pico_w/wifi/access_point/dhcpserver`
  (originally from the MicroPython project).
- License: MIT. Copyright (c) 2018-2022 Damien P. George and contributors.

## DNS server — `src/vendor/dnsserver.c`, `include/dnsserver.h`
- Source: Raspberry Pi `pico-examples`, `pico_w/wifi/access_point/dnsserver`.
- License: BSD-3-Clause. Copyright (c) 2022 Raspberry Pi (Trading) Ltd.

## 8x8 bitmap font — `include/font8x8_basic.h`
- Source: `dhepper/font8x8` (`font8x8_basic.h`).
- License: Public domain. Author: Daniel Hepper <daniel@hepper.net>.

## QR Code generator — `src/vendor/qrcodegen.c`, `include/qrcodegen.h`
- Source: Nayuki `QR-Code-generator` (C port).
- License: MIT. Copyright (c) Project Nayuki.

## cJSON — `src/vendor/cJSON.c`, `include/cJSON.h`
- Source: `DaveGamble/cJSON` v1.7.18.
- License: MIT. Copyright (c) 2009-2017 Dave Gamble and cJSON contributors.

## Monocypher — `src/vendor/monocypher.c`, `src/vendor/monocypher.h`
- Source: Monocypher 4.0.3 (<https://monocypher.org>). Used for X25519 in the
  cloud-relay key exchange (`src/relay_crypto.c`); the same version the sibling
  `tesserae-device-firmware` uses, so both derive identical frame keys.
- License: dual 2-clause BSD / CC0-1.0, full text in
  `src/vendor/MONOCYPHER-LICENCE.md`. Copyright (c) 2017-2023 Loup Vaillant and
  contributors.

## mbedTLS — bundled with the Pico SDK, not vendored here
- Source: `lib/mbedtls` inside the `framework-picosdk` package; compiled by
  `scripts/mbedtls.py` against `include/mbedtls_config.h`.
- License: Apache-2.0 OR GPL-2.0-or-later. Copyright The Mbed TLS Contributors.

## TLS root certificates — `include/ca_roots.h`
- Source: the published root certificates of Google Trust Services (GTS Root R1,
  GTS Root R4) and the Internet Security Research Group (ISRG Root X1),
  fetched and fingerprint-checked by `scripts/gen_ca_roots.sh`.
- Public certificates, distributed for the purpose of validating TLS chains; no
  license terms attach to their redistribution.
