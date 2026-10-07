# M2468 touch coordinate units

The existing M2468 MP profile (`goodix,brl-d`, root `meizu,board-id = <3 5>`)
reports firmware X/Y in tenths of the DT axis unit. The DT/input axes remain
1264 by 2780. Convert each position once, immediately before reporting it to
Linux input; leave contact IDs, width, pressure, release and gesture gates intact.
Other profiles retain their existing coordinate units.

This moves the user-confirmed `0001-Fix-touch-on-Meizu-21-Note.patch`
inputflinger workaround into the device driver. It uses the same integer
truncation for the normal finger, existing gesture and optional pen position
outputs. No gesture is enabled and no UI-ready or touch event is synthesized.

**Remove that framework patch when building a ROM with this driver.** Keeping
both conversions divides coordinates by 100 overall. This tree cannot remove
a patch from the user's separate ROM checkout.

Local ARM64 compilation, imported CRCs, exported touch ABI and CFI/ThinLTO
were checked. `tests/test_coordinates.py` executes the real finger reporter
with captured input events (run with a host C compiler). It covers ten slots,
origin/edges, truncation, axis swap, other profiles, width and release.
No device was connected during this change; full-screen tracking, multi-touch,
suspend/resume and fingerprint interactions still require device validation.
