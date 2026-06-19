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
    void         (*run)(uint8_t variant);  /* full sequence; variant for multi-res panels */
} panel_t;

/* Find the descriptor for an EEPROM display_variant, or NULL if unsupported. */
const panel_t *panel_for_variant(uint8_t variant);
