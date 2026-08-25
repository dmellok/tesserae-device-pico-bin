#!/usr/bin/env bash
#
# flash.sh: flash firmware.uf2 to a Pimoroni Pico Plus 2 W over USB without
# unplugging anything, using the RP2350's 1200-baud reset-to-BOOTSEL touch.
#
# `pio run -t upload` does not work on this RP2350 (the bundled picotool is
# RP2040-era and cannot force a running device into BOOTSEL). Instead we touch
# the CDC port at 1200 baud, which the firmware's stdio-USB honours by rebooting
# into the bootloader; the RP2350 drive then mounts and we copy the .uf2 onto it.
#
# Needs the device's USB up (an awake cycle or the setup portal). If it is in
# deep sleep there is no serial port: double-tap RESET, then re-run.
#
# Expect to re-run this. POWMAN deep sleep tears the USB PHY down, so a device
# on a normal cadence is absent from USB for all but a few seconds of each
# cycle, and an attempt that lands in the sleep window fails through no fault of
# the hardware. Wrapping the call in a retry loop until it prints "flash: done"
# is the normal way to use it:
#
#   for i in $(seq 1 25); do
#       ./scripts/flash.sh 2>&1 | tail -1 | grep -q "flash: done" && break
#       sleep 4
#   done
#
# Setting ALWAYS_ON in secrets.h keeps USB up continuously and makes flashing
# immediate, which is worth doing for a bench session.
#
# macOS only (uses /Volumes and diskutil). Usage:
#   scripts/flash.sh [path/to/firmware.uf2]   # default: the PlatformIO build output
set -u

UF2="${1:-.pio/build/pimoroni_pico_plus_2w/firmware.uf2}"
[ -f "$UF2" ] || { echo "flash: uf2 not found: $UF2 (build first with: pio run)"; exit 1; }

PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
if [ -n "$PORT" ]; then
    # The 1200-baud open/close triggers reset-to-BOOTSEL. The open may raise
    # ("Device not configured") as the device resets; the touch still lands.
    python3 - "$PORT" <<'PY' 2>/dev/null || true
import sys, serial, time
try:
    s = serial.Serial(sys.argv[1], 1200); time.sleep(0.2); s.close()
except Exception:
    pass
PY
    echo "flash: 1200-baud touch sent to $PORT"
else
    echo "flash: no serial port (device asleep?) -- double-tap RESET to enter BOOTSEL"
fi

# Mount the BOOTSEL volume if macOS has enumerated the disk but not mounted it.
# This is not rare: the RP2350 appears as a FAT16 volume named RP2350, and
# Finder sometimes leaves it unmounted, in which case /Volumes/RP2350 never
# shows up and the wait below times out while the drive is sitting right there
# in `diskutil list`.
try_mount_bootsel() {
    [ -d /Volumes/RP2350 ] && return 0
    local dev
    dev=$(diskutil list 2>/dev/null | awk '/ RP2350 /{print $NF}' | head -1)
    [ -n "$dev" ] || return 1
    diskutil mount "$dev" >/dev/null 2>&1 || return 1
    # The mount is asynchronous enough that an immediate copy can still race.
    for _ in 1 2 3 4 5; do
        [ -d /Volumes/RP2350 ] && return 0
        sleep 1
    done
    return 1
}

echo "flash: waiting for the RP2350 BOOTSEL drive..."
for _ in $(seq 1 60); do
    try_mount_bootsel
    if [ -d /Volumes/RP2350 ]; then
        for try in 1 2 3 4 5 6 7 8 9 10; do
            if cp "$UF2" /Volumes/RP2350/firmware.uf2 2>/dev/null; then
                sync
                diskutil eject RP2350 >/dev/null 2>&1
                echo "flash: done ($UF2)"
                exit 0
            fi
            sleep 1   # permission race while the drive finishes mounting
        done
        echo "flash: copy failed after 10 tries -- re-run"; exit 1
    fi
    sleep 0.5
done
echo "flash: no BOOTSEL drive appeared -- double-tap RESET and re-run"; exit 1
