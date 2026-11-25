#!/usr/bin/env python3
"""Idempotent source fix for extern/borealis' broken vibration clear.

`brls::SwitchInputManager::clearVibration()` calls

    sendRumbleInternal(handles, values, 160.0f, 320.0f, 0.0f, 0.0f);

but that overload treats arguments 3/4 as *motor intensities*, not frequencies.
The result is `amp_low = amp_high = 320.0f` (HidVibrationValue amplitude is
0.0f..1.0f, where 1.0f is full scale, so Horizon clamps it to full) combined with
`freq = 160/204 = 0.78 Hz` and `320/204 = 1.57 Hz` — far below the linear
resonant actuator band. A wireless Joy-Con driven like that buzzes faintly and
forever, because Horizon keeps replaying the last written value until it is
overwritten. clearVibration() runs on every controller style-set change, i.e.
exactly when the app boots with the Joy-Con detached from the rails (wireless).

Symptom that motivated this patch: launch the homebrew with detached Joy-Con in
handheld/docked mode -> faint continuous vibration; press HOME (system clears the
value) and re-enter -> no vibration (no style change, so clearVibration is not
called again).

The app-side idle vibration guard (app/src/levelup/rumble_switch.cpp) zeroes all
devices every 16 ms during startup and every 250 ms afterwards, so this patch is
a belt-and-braces root-cause fix: with it, the bogus write never happens at all.

Usage: patch_borealis_rumble.py <path to switch_input.cpp>
Exit status is always 0 unless the file cannot be read — the build must not be
blocked by an upstream refactor.
"""

from __future__ import annotations

import sys

SENTINEL = "LEVELUP_RUMBLE_ZERO_FIX"

BAD = "sendRumbleInternal(m_vibration_device_handles[controller], m_vibration_values[controller], 160.0f, 320.0f, 0.0f, 0.0f);"
GOOD = (
    "// " + SENTINEL + ": was 160.0f/320.0f, i.e. frequencies fed into the amplitude lane -> amp=320\n"
    "    // (clamped to full scale) at ~0.78 Hz, far below the LRA band, which makes detached Joy-Con\n"
    "    // buzz faintly forever. Clearing means amplitude 0.\n"
    "    sendRumbleInternal(m_vibration_device_handles[controller], m_vibration_values[controller], 0.0f, 0.0f, 0.0f, 0.0f);"
)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: patch_borealis_rumble.py <switch_input.cpp>")
        return 0

    path = sys.argv[1]
    try:
        with open(path, "r", encoding="utf-8", newline="") as handle:
            text = handle.read()
    except OSError as exc:
        print(f"skip ({exc})")
        return 0

    if SENTINEL in text:
        print("already patched")
        return 0

    if BAD not in text:
        print("pattern not found, upstream changed - left untouched")
        return 0

    crlf = "\r\n" in text
    body = GOOD.replace("\n", "\r\n") if crlf else GOOD
    text = text.replace(BAD, body, 1)

    with open(path, "w", encoding="utf-8", newline="") as handle:
        handle.write(text)

    print("patched" + (" (crlf)" if crlf else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
