#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""The PS5 bus's extra HID gamepads (Wine patch 0966): when a controller's device is made and removed, and what
the patch must keep so that a game that does not look for controllers never opens the other users' pads."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PATCH = ROOT / "wine/patches/0966-winebus-ps5-other-controllers-as-hid-gamepads.patch"

HARNESS = r'''
#include <assert.h>
typedef int BOOL;
enum { FALSE, TRUE };
/*PATCH*/
int main(void)
{
    enum ps5_slot_state state = PS5_SLOT_ABSENT;

    /* A controller that is not there has no device. */
    assert(ps5_slot_step(&state, FALSE) == PS5_SLOT_NONE && state == PS5_SLOT_ABSENT);
    /* It comes: one device, however long it stays. */
    assert(ps5_slot_step(&state, TRUE) == PS5_SLOT_CREATE && state == PS5_SLOT_PRESENT);
    assert(ps5_slot_step(&state, TRUE) == PS5_SLOT_NONE && state == PS5_SLOT_PRESENT);
    /* It goes: the device is asked to go once. */
    assert(ps5_slot_step(&state, FALSE) == PS5_SLOT_REMOVE && state == PS5_SLOT_REMOVING);
    assert(ps5_slot_step(&state, FALSE) == PS5_SLOT_NONE && state == PS5_SLOT_REMOVING);
    /* Back before the old device stopped: no second device with its instance ID. */
    assert(ps5_slot_step(&state, TRUE) == PS5_SLOT_NONE && state == PS5_SLOT_REMOVING);
    /* ps5_device_stop frees the slot, and the next poll makes a new one. */
    state = PS5_SLOT_ABSENT;
    assert(ps5_slot_step(&state, TRUE) == PS5_SLOT_CREATE && state == PS5_SLOT_PRESENT);
    return 0;
}
'''


def added(path):
    for diff in PATCH.read_text().split("diff --git ")[1:]:
        if diff.splitlines()[0].split(" b/", 1)[1] == path:
            return "\n".join(line[1:] for line in diff.splitlines() if line.startswith("+") and not line.startswith("+++"))
    raise AssertionError(path)


def main():
    bus = added("dlls/winebus.sys/bus_ps5.c")
    enums = re.search(r"enum ps5_slot_state \{.*?\};\nenum ps5_slot_action \{.*?\};", bus, re.S)
    step = re.search(r"static enum ps5_slot_action ps5_slot_step\(.*?\n\}\n", bus, re.S)
    assert enums and step
    with tempfile.TemporaryDirectory() as scratch:
        source = Path(scratch) / "slots.c"
        source.write_text(HARNESS.replace("/*PATCH*/", enums[0] + "\n" + step[0]))
        program = Path(scratch) / "slots"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(program)],
                       check=True)
        subprocess.run([str(program)], check=True)

    # winebus polls in every game: a read that noted the controller as asked for would open the other users' pads
    # in all of them.
    assert 'dlsym(RTLD_DEFAULT, "pw_wine_pad_peek")' in bus
    assert "pw_wine_pad_at" not in bus
    # Opening a device asks for controllers 1..3 only, and the device of controller 0 stays always present.
    assert "((1u << PS5_SLOTS) - 1) & ~1u" in bus
    assert "for (slot = 1; slot < PS5_SLOTS; slot++)" in bus
    # Each device has its own serial number, and a stop frees its slot.
    assert "(WCHAR)('0' + slot)" in bus
    assert "ps5_slots[impl->slot].state = PS5_SLOT_ABSENT;" in bus
    # An Xbox 360 gamepad like controller 0's: the &IG_ marking keeps a game that reads XInput and HID from counting twice.
    # (The descriptor is patch 0951's, unchanged, so it is not in this diff.)
    base = (ROOT / "wine/patches/0951-winebus-ps5-title-controller-as-hid-gamepad.patch").read_text()
    assert ".is_gamepad = TRUE" in base and ".vid = 0x045e" in base and ".pid = 0x028e" in base
    assert "ps5_device_create(UINT32 slot)" in bus
    table = PATCH.read_text()
    # The new call has a slot in both Unix call tables.
    assert table.count("+    ps5_bus_ask,") == 2
    # A hosted start waits for as many gamepads as the title says are there, and the title says so.
    host = added("dlls/ntoskrnl.exe/ntoskrnl.c")
    assert 'L"WINE_PS5_HID_PADS"' in host and "InterlockedDecrement( &host_gamepads ) <= 0" in host
    assert 'setenv("WINE_PS5_HID_PADS"' in (ROOT / "native/wine64_main.c").read_text()
    # Each device is opened at most once as far as the minidriver knows.
    device = added("dlls/hidclass.sys/device.c")
    assert "activate = !pdo->activated;" in device and "IOCTL_HID_ACTIVATE_DEVICE" in device
    # Devices list by instance ID, and games take the first for player 1: no parent hash may decide that.
    assert "+            caps->UniqueID = 1;" in table
    print("Wine HID slots passed: made while connected, removed when gone, not replaced until stopped")


if __name__ == "__main__":
    main()
