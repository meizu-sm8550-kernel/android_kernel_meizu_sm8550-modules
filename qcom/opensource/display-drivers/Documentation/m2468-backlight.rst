Meizu 21 Note ordinary backlight
================================

This compatibility implementation restores the missing ordinary PWM/DC and
demura command path for the M2468 Tianma panel. It uses the existing device-tree
command tables; it does not tune gamma, brightness limits or panel voltages.

Scope and units
---------------

The existing Note identity requires ILI7838E_TIANMA_MEIZU, the exact Tianma node,
and exactly two root meizu,board-id cells containing 3 and 5. The ordinary path
also requires qcom,mdss-pwm-dc-siwtch=1 (the misspelling is the vendor ABI), the
1107 DC boundary and valid per-mode tables. Other panels keep their existing
backlight path and do not parse these additional tables.

The setter receives panel brightness after the existing scale and dimming LUT.
The switch tables retain all 47 packets, with a private big-endian 0x51 payload
at packet 5. Ordinary brightness retains the existing inverted-DBV convention.
The three demura tables and all five refresh rates are validated independently.
No DT command payload is modified in place.

Transactions and recovery
-------------------------

The existing display and panel transaction locks serialize normal requests,
local HBM restoration, modes and low-power transitions. The driver votes DSI
core and link clocks over each transaction. A PWM/DC switch uses the vendor
sequence: synchronously request the SDE early-wakeup resource event, then wait
for the encoder's RD_PTR vblank before sending the table. The synchronous helper
avoids queuing early-wakeup work behind the caller's vblank wait. If the physical
encoder is not enabled during first handoff, the wait returns EWOULDBLOCK and
the table is sent as in the vendor path. Other wake/wait errors invalidate the
state for retry. This never allocates a GPIO IRQ or reads TE through GPIO: TLMM's
IRQ request remuxes the shared TE pad away from mdp_vsync, while passive GPIO
sampling can miss short pulses and delay or reject wakeup brightness.

Both normal panel ON and continuous-splash handoff establish the Note panel's
DPMS ON state. Continuous splash also invalidates HBM/backlight state so the
first brightness request rebuilds the table instead of treating inherited boot
state as known. The handoff state update is serialized with display/panel locks.
Encoder, vblank, transfer and clock errors reach the caller.
For 30/60/120 Hz modes, a full switch temporarily disables a known nonzero
ADFR setting and restores it afterward. Unknown ADFR state is first set to OFF.
Failures preserve the first error and only successful sends establish cached
ADFR state. Fixed 144/90 Hz modes never query or send ADFR tables.
Post-switch delays follow the existing panel protocol: 14/18/26/34/66 ms for
144/120/90/60/30 Hz. These are not changes to refresh rate.

The private DSI host transfer interface returns zero on success, unlike a
byte-count API. Unexpected positive returns are rejected. Partial buffered
transactions are discarded on failure. Successful brightness and compensation
state are recorded only when the complete transaction and clock release succeed.

Mode, target band, actual compensation and validity are distinct state. The
known DC-to-low transition preserves the observed vendor ordering: low demura,
then the full DC-to-PWM table, which leaves middle compensation. It is not
modelled as an ideal three-way brightness lookup.

ON, reset, ESD, low power, mode changes and local HBM invalidate ordinary state.
ON alone does not prove the page-0x73 configuration. The first nonzero request
from unknown state therefore replays the complete PWM/DC table followed by the
required demura table. This deliberate recovery differs from the vendor's
cached reset-to-ON path and avoids trusting a stale global cache. Subsequent
requests in the same established mode/band send only brightness. Zero during
low power keeps the existing brightness-only behavior and does not establish
valid compensation. HBM OFF idempotence, fixed 144/90 Hz ADFR separation, dynamic
LOCAL_OFF demura and the framework-ready handshake remain intact.

Verification boundary
---------------------

Run tests/note_backlight/run.py with a host C compiler; CC may select another
compiler. The tests compile the real functions with hardware substitutes and
exercise packets, failures, parsing and lifecycle boundaries. They are not a
hardware wakeup/latency or optical verification.

The local maintenance layer creates a new isolated module candidate from
verified retained objects and replaces only msm_drm in the current 388-module
selection. It does not claim that an obsolete full-package maintenance chain
has been repaired. Rebuild the published modules with the ROM's default toolchain
and update the matching images before evaluating the physical display.
