/*
 * buttons.c: Inky Impression front buttons. See buttons.h.
 */
#include "buttons.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

/* Indexed by button_id_t. See the header for how this map was established and
 * why D (GP1) is absent. */
static const uint8_t k_pin[BTN_COUNT] = {
    [BTN_A] = 15,
    [BTN_B] = 6,
    [BTN_C] = 28,
};

void buttons_init(void)
{
    for (int i = 0; i < BTN_COUNT; i++) {
        gpio_init(k_pin[i]);
        gpio_set_dir(k_pin[i], GPIO_IN);
        gpio_pull_up(k_pin[i]);
    }
    /* The pull-up needs a moment to settle before the first read, or a pad that
     * was left floating by the previous power state reads as a phantom press --
     * which, for the factory-reset hold, would be a very expensive false
     * positive. */
    sleep_ms(2);
}

bool button_down(button_id_t b)
{
    if (b < 0 || b >= BTN_COUNT) return false;
    return gpio_get(k_pin[b]) == 0;    /* active low */
}

bool button_held_for(button_id_t b, unsigned ms, void (*on_tick)(unsigned))
{
    if (!button_down(b)) return false;

    unsigned elapsed = 0;
    unsigned since_tick = 0;
    while (elapsed < ms) {
        sleep_ms(50);
        elapsed += 50;
        if (!button_down(b)) return false;   /* released: abandon immediately */
        since_tick += 50;
        if (on_tick != NULL && since_tick >= 1000) {
            since_tick = 0;
            on_tick(elapsed);
        }
    }
    return true;
}
