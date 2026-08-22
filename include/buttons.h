/*
 * buttons.h: the Inky Impression front buttons.
 *
 * Active-low, wired straight to GPIO (an I2C scan on GP2/GP3 finds only the
 * model EEPROM at 0x50, no expander). The mapping was established by a GP0..28
 * hunt on real hardware: the standard Inky A5/B6/C16/D24 BCM map does NOT hold
 * through the Hard Stuff adapter, and only B matches it.
 *
 *   A = GP15    B = GP6    C = GP28    D = GP1
 *
 * D is deliberately NOT claimed here: GP1 is UART0 TX, and driving a pull-up on
 * it fights whatever stdio does with the UART. GP5 reads low at boot (tied or
 * in use) and is not a button at all.
 *
 * SCOPE: these are readable only while the device is AWAKE. Buttons cannot wake
 * it from POWMAN deep sleep -- POWMAN_POWER_STATE_NONE cuts the IO/pad domain,
 * so the pad cannot sense the edge, while the always-on timer wake still fires.
 * That is why the only button feature here is a hold checked during the boot
 * window (see main.c), rather than a general press-to-refresh.
 */
#pragma once

#include <stdbool.h>

typedef enum {
    BTN_A = 0,   /* GP15 -- primary/refresh; the factory-reset hold */
    BTN_B,       /* GP6  */
    BTN_C,       /* GP28 */
    BTN_COUNT,
} button_id_t;

/* Configure the button pads as inputs with pull-ups. Safe to call once at boot;
 * costs nothing on a board with no buttons fitted (the pads simply read high). */
void buttons_init(void);

/* True while the button is pressed (the pad reads low). */
bool button_down(button_id_t b);

/* True if `b` stays held for the whole of `ms`, sampling as it goes. Returns
 * false the moment it is released, so a mis-press costs no extra time.
 * `on_tick(elapsed_ms)` is called about once a second so the caller can log
 * progress; pass NULL for none. */
bool button_held_for(button_id_t b, unsigned ms, void (*on_tick)(unsigned));
