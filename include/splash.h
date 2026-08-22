/*
 * splash.h: on-device procedural splash screens.
 *
 * Renders into a panel-native framebuffer (the same packed-4bpp landscape
 * buffer paint() consumes, so it works for any attached panel and the 13.3"
 * rotate is handled downstream) and paints it. Monochrome (black on white) so
 * it is correct on both Spectra 6 and 7-colour palettes.
 *
 * These exist so a headless device is never a black box: whatever goes wrong
 * during setup, the panel says what and what to do about it. They cost a full
 * ~20-45 s refresh, so main.c paints them only on a real STATE CHANGE, never on
 * every retry (see splash_state_t and sleep_splash_state()).
 */
#pragma once

#include <stdint.h>
#include "panels.h"

/* What the panel is currently telling the user. Stored across deep sleeps in a
 * POWMAN scratch word, so a repeated failure repaints nothing and a recovery
 * repaints exactly once.
 *
 * SPLASH_NONE means "a real frame (or the setup screen) is on the glass" -- it
 * is the value main.c stores after a successful paint, and it is what makes the
 * first failure after a good run actually show up. */
typedef enum {
    SPLASH_NONE = 0,
    SPLASH_SETUP,             /* the provisioning portal screen is up */
    SPLASH_WIFI_FAILED,       /* SSID/password wrong, or the AP is gone */
    SPLASH_SERVER_UNREACHABLE,/* WiFi is fine; the Tesserae server is not */
    SPLASH_AWAITING_APPROVAL, /* reached the server, admin has not clicked Register */
    SPLASH_RELAY_PAIRING,     /* relay code submitted, waiting on the home instance */
    SPLASH_CONNECTED_NO_FRAME,/* onboarded; the first frame has not been rendered yet */
    SPLASH_BATTERY_EMPTY,     /* cell flat: this is the last thing painted before hibernating */
} splash_state_t;

/* Render and paint the provisioning splash: Tesserae glyph, the setup AP name,
 * the portal URL, and a QR that joins the open AP when scanned. Sized/centered
 * for the attached panel. Takes ~20-45s (a full e-paper refresh). */
void splash_show_setup(const panel_t *panel, uint8_t variant, const char *ssid);

/* Render and paint a status screen for `state`. SPLASH_NONE and SPLASH_SETUP
 * are not valid here (the first is "nothing to say", the second has its own
 * renderer) and are ignored. Same cost as above. */
void splash_show_status(const panel_t *panel, uint8_t variant,
                        splash_state_t state);
