/*
 * panels.h: per-panel descriptors, selected at runtime by the Inky model
 * EEPROM's display_variant. Each supported panel provides a run() that does
 * the whole job for that panel: GPIO/SPI setup, reset, init, paint a test
 * pattern (vertical colour stripes), and refresh.
 *
 * Only the 13.3" (EL133UF1) is verified on hardware. The others are ports from
 * the Pimoroni reference drivers and are marked UNTESTED until exercised on the
 * real panel.
 */
#pragma once

#include <stdint.h>

typedef struct {
    const char    *name;
    const uint8_t *variants;     /* EEPROM display_variant ids this panel matches */
    uint8_t        n_variants;
    uint16_t       width;
    uint16_t       height;
    uint8_t        verified;     /* 1 if confirmed on hardware, 0 if a blind port */
    /* Full sequence: GPIO/SPI setup, reset, init, stream a frame, refresh.
     * frame == NULL paints the built-in vertical-stripe test pattern; otherwise
     * frame is the panel-native packed-4bpp buffer (width*height/2 bytes). */
    void         (*paint)(uint8_t variant, const uint8_t *frame);
} panel_t;

/* Frame size in bytes for a panel (packed 4bpp, 2 px/byte). */
static inline uint32_t panel_frame_bytes(const panel_t *p)
{
    return (uint32_t)p->width * p->height / 2;
}

/* Find the descriptor for an EEPROM display_variant, or NULL if unsupported. */
const panel_t *panel_for_variant(uint8_t variant);
