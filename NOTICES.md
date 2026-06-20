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
