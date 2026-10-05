# Firmware 13.60 startup

Native titles must link against the SDK's `libkernel.so`. Linking all kernel
stub variants can select `libkernel_web` through the weak stub. On the tested
13.60 console that import was unresolved: the first `clock_gettime` call
jumped to address zero before startup logging. `tools/build_native.sh` now
puts the native provider first and excludes the other `libkernel*.so` stubs.

## Required helper dependency

The kernel import fix alone is insufficient with the helper release used in
this investigation. Its CRT firmware table did not recognize 13.60, so the
helper terminated before granting `/data`, and the title reported a connection
reset followed by `data-helper-incomplete`.

The helper must be built with a 13.60-compatible SDK. The tested SDK was v0.42
from native foundation commit `9c0b994a048521af6fb84c73ded364504fe250e9`.
That SDK's credential attribute API takes 32-byte arrays. The corresponding
PS5-Lapy-JB-Daemon fix is commit
`611fa5f7ed46cdde7094b764168390ab5c4e89f8`
([PR #49](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon/pull/49)):
preserve all attribute bytes in snapshot/verification/rollback, propagate read
errors, and set the privilege flag in `attrs[3]`. The old scalar started at
`ucred+0x83`; the array starts at `ucred+0x80`.

Until a corrected helper release is published, build that helper for the
package's title ID and replace the downloaded `lapy.elf` and matching
`lapy-manifest.json` after building the title. Validate the manifest against
the resulting helper. Do not mix a helper with another build's manifest or
assume a newly built title has automatically acquired the corrected helper.

## Hardware evidence and scope

On 2026-10-05, two independent launcher starts on firmware `13600007` (13.60)
with the native kernel provider and rebuilt helper rendered the library.
Logs confirmed layout validation, cooperative credential cloning, successful
root transfer, donor cleanup, balanced root references, `helper_completed=1`,
`helper_errno=0`, `data_after=1`, and video/pad/HID initialization.

The tested eboot SHA-256 was
`0ed20049cc8e1587250d1c52e25a3c0419efe35cbe8b3597eab36c5666986b80`;
helper SHA-256 was
`e31424f19b1308b31d0b63792ac21068ee332c45a49401371bcdbc4a89e65806`.
These identify the tested artifacts, not expected hashes for future builds.

Half-Life 2 subsequently reached its rendered main menu using the existing
Wine runtime, a repaired offline prefix, DXVK 2.6.2 D3D9 DLLs and a 1920x1080
virtual monitor. Follow-up tests rendered train-station gameplay and verified
left-stick movement, right-stick look, Cross jump, Options menu and keyboard
Escape resume through MCP/JetKVM. The user confirmed physical USB keyboard W
movement and working audio. HDMI capture also contained nonzero stereo audio.
These control tests passed after restoring the normal feature-branch launcher
(eboot SHA-256 `ef2c0bfbda5a6fa9f0d614384085699f5dde6894d73b7d8016a05a91e552e8ee`);
no diagnostic SDL overrides were needed.

The incomplete offline bottle required its missing 32-bit COM registrations:

| COM class | CLSID | Server |
| --- | --- | --- |
| MMDeviceEnumerator | `{BCDE0395-E52F-467C-8E3D-C4579291692E}` | `mmdevapi.dll` |
| DirectInput8 | `{25E609E4-B259-11CF-BFC7-444553540000}` | `dinput8.dll` |

With Wine closed, back up a fresh `system.reg` and copy only each existing
`Software\\Classes\\CLSID` class and `InprocServer32` section into
`Software\\Classes\\Wow6432Node\\CLSID`, retaining the DLL path and
`ThreadingModel="Both"`. Do not overwrite a running bottle's registry or
replace unrelated sections with an older snapshot. Audio already selected
`ps5` under `Software\\Wine\\Drivers`. MMDeviceEnumerator's absence caused
DirectSound error `80040154`; DirectInput8's absence prevented controller
detection. After repair, XInput loaded and recognized the title's controller.
These were bottle preparation failures, rather than firmware-specific driver
changes.

The final HL2 profile uses `+exec 360controller.cfg +joystick 1`, `[input]
mode = xinput` and `options = escape`. Temporary D-pad WASD/Cross Space
bindings, forced map startup, console startup and verbose tracing were removed.
The existing SDL raw-input hint in the normal launcher was sufficient.

Sustained performance, saves and a long session were not verified. The startup
evidence does not certify every runtime, SDK or firmware combination, and does
not change the helper's release validation manifest.
