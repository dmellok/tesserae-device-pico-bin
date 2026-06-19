/*
 * sleepmgr.c: RP2350 POWMAN deep sleep + wall-clock. See sleepmgr.h.
 *
 * Wall-clock: the POWMAN timer (1 kHz, lposc-sourced) holds epoch milliseconds
 * once SNTP sets it; it lives in the always-on domain so it keeps counting
 * through deep sleep. We treat a timer value past 2017 as "time is set".
 *
 * Persistence: POWMAN scratch words survive low-power mode. Writes to the
 * POWMAN block carry a password in the top 16 bits, so we only trust the low
 * 16 bits of a scratch word and always write it with the password attached --
 * correct whether or not scratch itself enforces the password.
 */
#include "sleepmgr.h"

#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/powman.h"
#include "hardware/structs/powman.h"
#include "hardware/structs/usb.h"
#include "hardware/clocks.h"
#include "hardware/resets.h"
#include "hardware/watchdog.h"

#define WAKE_MAGIC      0xB007u                 /* low-16 marker: a deep sleep has happened */
#define EPOCH_VALID_MS  1500000000000ull        /* ~2017-07-14; below this, time is unset   */

static wake_reason_t s_reason;
static uint32_t      s_boot;
static bool          s_latched;

static inline void scratch_put16(int i, uint32_t v)
{
    powman_hw->scratch[i] = POWMAN_PASSWORD_BITS | (v & 0xFFFFu);
}
static inline uint32_t scratch_get16(int i)
{
    return powman_hw->scratch[i] & 0xFFFFu;
}

void sleep_timer_init(void)
{
    /* Establish the 1 kHz LPOSC tick source. The SDK's
     * powman_timer_set_1khz_tick_source_lposc() only runs the source switch +
     * USING_LPOSC handshake (which is what actually applies the /32.768 divider
     * that turns the ~32 kHz LPOSC into 1 ms ticks) when the timer is already
     * running. So start the timer first, then assert the source: that guarantees
     * the divider takes effect on every boot, including a timer-wake boot where
     * the timer is already running from the always-on domain. Without this the
     * timer counts raw LPOSC as if it were ms and alarms fire ~33x too early.
     * The running count is preserved (no set_ms here), so the clock survives. */
    if (!powman_timer_is_running())
        powman_timer_start();
    powman_timer_set_1khz_tick_source_lposc();
}

static void latch(void)
{
    if (s_latched) return;
    if (scratch_get16(0) == WAKE_MAGIC) {
        s_reason = WAKE_TIMER;
        s_boot   = scratch_get16(1) + 1;
    } else {
        s_reason = WAKE_COLD;
        s_boot   = 1;
    }
    scratch_put16(1, s_boot);
    s_latched = true;
}

wake_reason_t sleep_wake_reason(void) { latch(); return s_reason; }
uint32_t      sleep_boot_count(void)  { latch(); return s_boot;   }

void sleep_set_epoch(uint32_t sec)
{
    sleep_timer_init();
    powman_timer_set_ms((uint64_t)sec * 1000ull);
}

uint32_t sleep_epoch_now(void)
{
    uint64_t ms = powman_timer_get_ms();
    return (ms > EPOCH_VALID_MS) ? (uint32_t)(ms / 1000ull) : 0;
}

void sleep_deep_ms(uint64_t ms)
{
    sleep_timer_init();
    uint64_t now = powman_timer_get_ms();
    scratch_put16(0, WAKE_MAGIC);   /* next boot will read this as a timer wake */

    /* Prepare for a clean power-down, mirroring pimoroni/badger2350's powman
     * module (the proven RP2350 deep-sleep reference). Without this the chip
     * will not stay powered down while USB is connected: the USB peripheral
     * holds it awake and it resets after a few seconds instead of sleeping for
     * the full interval. Steps: run from pll_usb and stop pll_sys, unlock the
     * VREG control interface, then fully tear down the USB PHY (power it off
     * and apply pulldowns on DP/DM). */
    set_sys_clock_48mhz();
    hw_set_bits(&powman_hw->vreg_ctrl, POWMAN_PASSWORD_BITS | POWMAN_VREG_CTRL_UNLOCK_BITS);

    reset_block_mask(RESETS_RESET_USBCTRL_BITS);
    unreset_block_mask_wait_blocking(RESETS_RESET_USBCTRL_BITS);
    usb_hw->muxing    = USB_USB_MUXING_TO_PHY_BITS | USB_USB_MUXING_SOFTCON_BITS;
    usb_hw->main_ctrl = USB_MAIN_CTRL_CONTROLLER_EN_BITS;
    usb_hw->sie_ctrl  = USB_SIE_CTRL_EP0_INT_1BUF_BITS;
    usb_hw->inte      = USB_INTS_BUFF_STATUS_BITS | USB_INTS_BUS_RESET_BITS | USB_INTS_SETUP_REQ_BITS |
                        USB_INTS_DEV_SUSPEND_BITS | USB_INTS_DEV_RESUME_FROM_HOST_BITS | USB_INTS_DEV_CONN_DIS_BITS;
    usb_hw->phy_direct = USB_USBPHY_DIRECT_TX_PD_BITS | USB_USBPHY_DIRECT_RX_PD_BITS |
                         USB_USBPHY_DIRECT_DM_PULLDN_EN_BITS | USB_USBPHY_DIRECT_DP_PULLDN_EN_BITS;
    usb_hw->phy_direct_override =
        USB_USBPHY_DIRECT_RX_DM_BITS | USB_USBPHY_DIRECT_RX_DP_BITS | USB_USBPHY_DIRECT_RX_DD_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_TX_DIFFMODE_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_DM_PULLUP_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_FSSLEW_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_TX_PD_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_RX_PD_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DM_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_TX_DP_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DM_OE_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_TX_DP_OE_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_DM_PULLDN_EN_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_DP_PULLDN_EN_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_DP_PULLUP_EN_OVERRIDE_EN_BITS |
        USB_USBPHY_DIRECT_OVERRIDE_DM_PULLUP_HISEL_OVERRIDE_EN_BITS | USB_USBPHY_DIRECT_OVERRIDE_DP_PULLUP_HISEL_OVERRIDE_EN_BITS;

    powman_set_debug_power_request_ignored(true);   /* allow sleep with a debugger attached */

    /* off = every domain off (switched core powers down). On wake, power the
     * switched core + XIP cache back up; the sequencer restores SRAM. */
    powman_power_state off = POWMAN_POWER_STATE_NONE;
    powman_power_state on  = POWMAN_POWER_STATE_NONE;
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_SWITCHED_CORE);
    on = powman_power_state_with_domain_on(on, POWMAN_POWER_DOMAIN_XIP_CACHE);

    if (!powman_configure_wakeup_state(off, on)) {
        watchdog_reboot(0, 0, 0);
        return;
    }

    /* Clear the boot vector so the ROM does a normal boot on wake. */
    powman_hw->boot[0] = 0;
    powman_hw->boot[1] = 0;
    powman_hw->boot[2] = 0;
    powman_hw->boot[3] = 0;

    powman_enable_alarm_wakeup_at_ms(now + ms);

    powman_set_power_state(off);

    /* Power down. The wake event resets the chip, so this never returns. */
    while (true) __asm volatile ("wfi");
}
