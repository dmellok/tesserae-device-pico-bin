/*
 * net_portal.h: WiFi/MQTT provisioning captive portal.
 *
 * When the device has no usable WiFi config, it comes up as an open access
 * point ("Tesserae-Setup-XXXX"), runs a DHCP server + a wildcard DNS hijack
 * (so a phone's captive-portal check pops the form automatically), and serves
 * a setup form styled to match the Tesserae ESP32 client. Submitting the form
 * saves WiFi + MQTT settings to flash and reboots to apply them.
 */
#pragma once

#include <stdint.h>
#include "panels.h"

/* Bring up the provisioning AP + portal and service it until the user submits
 * the form, at which point the config is saved and the device reboots. Does
 * not return. The panel (if any) is used to paint the setup splash. */
void portal_run(const panel_t *panel, uint8_t variant);
