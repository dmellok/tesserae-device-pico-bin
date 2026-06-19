/*
 * splash.h: on-device procedural splash screens.
 *
 * Renders into a panel-native framebuffer (the same packed-4bpp landscape
 * buffer paint() consumes, so it works for any attached panel and the 13.3"
 * rotate is handled downstream) and paints it. Monochrome (black on white) so
 * it is correct on both Spectra 6 and 7-colour palettes.
 */
#pragma once

#include <stdint.h>
#include "panels.h"

/* Render and paint the provisioning splash: Tesserae glyph, the setup AP name,
 * the portal URL, and a QR that joins the open AP when scanned. Sized/centered
 * for the attached panel. Takes ~20-45s (a full e-paper refresh). */
void splash_show_setup(const panel_t *panel, uint8_t variant, const char *ssid);
