# Meizu Note JIIOV platform driver

GPL-2.0-only source implementation for the existing M2468
`compatible = "jiiov,fingerprint"` device and the supplied ANC HAL. The ABI was
reconstructed offline from the device's stock `jiiov_fingerprint.ko` and
`anc.hal.so`; this directory contains no stock binary. It builds the external
module `jiiov_fingerprint.ko` against the Note Android 13 Linux 5.15 kernel.

This supplies the platform resource and netlink interfaces. Sensor transactions
in the observed HAL initialization go through TEE. There is no SPI, fabricated
read/write result, input event, screen notifier, or automatic touch/UI-ready
producer here. Building this module does not establish that loading, probe,
HAL/TEE initialization, HBM, enrollment, or authentication succeeds on hardware.

## Device tree and initial state

The existing Note node must provide `anc,vdd_use_pmic`, `vdd-supply`,
`anc,vdd_config = <vmin_uV vmax_uV load_uA>`, `anc,gpio_rst`, and `anc,gpio_irq`.
Resource acquisition looks up the states `anc_reset_low`, `anc_reset_high`, and
`anc_irq_default`. The Note DT values are 3.2 V / 150000 uA, reset GPIO 41 and IRQ
GPIO 40, but the driver reads these values from DT instead of hard-coding them.
GPIO values intentionally remain raw, matching stock despite the IRQ DT flag.

Only the observed PMIC/pinctrl mode is supported. Missing PMIC mode or present
`anc,vdd_use_gpio`, `anc,use_gpio_init`, or `anc,enable-on-boot` returns
`-EOPNOTSUPP`; missing/invalid required data returns an error. GPIO lookup errors,
including `-EPROBE_DEFER`, are preserved. There is no DT modification in this
directory.

Probe also checks the supply phandle targets the observed child of
`qcom,rpmh-vrm-regulator`, with no separate child driver, regulator coupling,
or upstream `*-supply` property on the child or aggregate parent. These checks
are needed for the Linux 5.15 disable-error retry semantics described below;
other regulator topologies are rejected rather than assumed equivalent.
Only one device can claim the module's character-device number.

Probe initializes work, locking, and lifetime protection, then publishes
`/dev/jiiov_fp` and the platform sysfs attributes. It does not request GPIOs,
enable the supply, reset the sensor, or request an IRQ. Open and close only
manage references. The observed HAL startup is resource request, power on,
reset, followed by TEE initialization.

## Character-device ABI

The complete command value is compared, including size/direction bits. All
successful commands return zero. An unknown command with `(cmd & 0xff00) ==
0x6100` returns `-EINVAL`; another magic returns `-ENOTTY`.

| Command | Operation |
| --- | --- |
| `0x6100` | Reset low then high using pinctrl, each with a minimum 10 ms sleep |
| `0x6101`, `0x6102` | Request / release resources |
| `0x6103`, `0x6104` | Enable / disable requested IRQ |
| `0x6105`, `0x6106` | Request / free IRQ |
| `0x6107`, `0x6108` | Mask / unmask new IRQ events |
| `0x610b`, `0x610c` | Power on / off |
| `0x610e` | Verified stock successful no-op |
| `0x610f`, `0x6110` | Stay awake / relax with 125 ms grace |
| `0x40106112` | Copy exactly 16 product bytes from userspace |
| `0x80016113` | Copy exactly one raw IRQ GPIO byte to userspace |

`0x6109`, `0x610a`, `0x610d`, the HAL's unused speed command `0x4004610d`,
and key-event command `0x40086111` remain unsupported and return `-EINVAL`.
The compat handler forwards the same commands with a compat userspace pointer.

Each reset phase uses `usleep_range(10000, 10100)`: at least 10 ms with a
100 us scheduling allowance, without busy-waiting. Scheduler delays can extend
the actual elapsed time beyond that requested range.

GPIO/pinctrl acquisition and power are idempotent. Power/reset/IRQ request and
IRQ GPIO reading before resource acquisition return `-ENODEV`. Enabling an
unrequested IRQ also returns `-ENODEV`; disabling/freeing an unrequested IRQ and
powering off an already-off device succeed. IRQ request uses
`IRQF_TRIGGER_FALLING | IRQF_ONESHOT` and is enabled on success. Its thread holds
a wake event for 125 ms and queues byte 1; masking stops new events without
cancelling work already queued.

## Sysfs and netlink

The platform node exposes six store-only, owner-write (`0200`) files. Stock
prefix acceptance is preserved with bounded comparisons, including a trailing
newline or suffix. A successful store returns the input count except
`netlink_event`, which returns the actual netlink send result as stock does.

| File | Commands |
| --- | --- |
| `resource_set` | `request`, `release` |
| `device_power` | `on`, `off` |
| `irq_set` | `enable`, `disable` |
| `hw_reset` | `reset` |
| `pinctl_set` | `anc_reset_low`, `anc_reset_high`, `anc_irq_default` |
| `netlink_event` | `test`, `irq`, `screen_off`, `screen_on`, `touch_down`, `touch_up`, `ui_ready`, `exit` |

Netlink protocol 30 is created before registering the platform driver. Outgoing
messages use type 30, sequence/pid zero, fixed destination port 100, and exactly
one payload byte. Explicit sysfs commands map to bytes 0 through 7 in the listed
order. No screen, touch, or UI event is generated automatically.

Receive validates the complete header, `nlmsg_len`, and the first payload byte.
Only registration/test byte 0 and exit byte 7 are echoed; other incoming events
are ignored. The HAL's 144-byte send initializes only that first byte, so its
remaining bytes are never interpreted as a string or structure. Echoing 7
wakes the HAL receiver before teardown joins its thread. A send error is
returned to sysfs. The receive callback has no error return to propagate reply
failures to the original sendmsg call. Socket creation failure fails module initialization;
the kernel socket API does not expose a detailed reason for its NULL result.
Protocol 30 must be free, including from the unused WLAN Cesium socket.

## Safety changes relative to stock

- Hardware failures are returned by ioctl and sysfs rather than swallowed.
  IRQ GPIO copy failure returns `-EFAULT` instead of stock's positive `1`.
  Product data is copied atomically from a local buffer and never printed as
  an unbounded string or dereferenced through a userspace pointer.
- A mutex serializes hardware ioctl/sysfs operations. IRQ/work do not take that
  mutex, so synchronous IRQ/work teardown cannot deadlock on it. Work is
  initialized once before publication, including before the first IRQ.
- Each successful supply enable is balanced once. Power-off first clears this
  consumer's load. A load-clear failure leaves the enable count untouched; with
  the validated Note topology, subsequent disable failures also precede the
  consumer count decrement. This avoids 5.15's late DRMS/coupling/upstream-supply
  failure ambiguity. Errors retain the handle and resources for a retry. A new
  power-on after a failed power-off restores the configured load without another
  enable. Resource release stops IRQ/work, then powers off before dropping pins.
- IRQ wake-disable failure preserves the requested IRQ, its number, wake state,
  and GPIO resources so `0x6106` or `0x6102` can retry. Linux 5.15 restores
  `wake_depth` when disabling wake fails; freeing the IRQ then requesting it again
  would otherwise accumulate wake references. IRQ request takes a module
  reference before registration; failed request/wake-enable unwinds it, and a
  clean IRQ/wake release drops it. On irreversible removal/shutdown, a failed
  wake disable is logged and callbacks/work are forcibly stopped, while the wake
  flag, IRQ number and module reference remain owned. The device/parent and
  single-device claim are retained so rebind returns `-EBUSY` instead of adding
  another wake reference. The failure requires a device restart to recover.
- Power-on separately takes a module reference before acquiring/enabling the
  regulator; errors unwind it and successful power-off releases it. Thus normal
  module unloading while powered or while IRQ/wake ownership remains is busy
  and requires the usual HAL IRQ-free/power-off/deinit first. If irreversible
  unbind still cannot disable power, the driver never
  calls `regulator_put` on the enabled handle or forces a shared supply off. It
  retains the regulator, device/parent, module reference, and single-device claim,
  after stopping callbacks and removing userspace entry points. The error log
  states shutdown failed and a device restart is required to recover. This is an
  intentional bounded retention on power or IRQ-wake failure, not successful cleanup;
  there is no retry work or callback that could outlive the module.
- Embedded `struct device` and `cdev_device_add` protect the enclosing object
  during open, including a partially failed publication. Open files hold device
  references. Removal marks the device dead, stops new entry points, frees IRQs
  and cancels work before unregistering the wakeup source. Existing files return
  `-ENODEV`; the final device release frees memory. Probe exposes only a dead
  object until all publication steps succeed.
- Module exit unregisters the platform driver and its event producers before
  releasing netlink. Socket sending and detachment are serialized independently.

## Host regression

Run `python3 -B tests/run.py` from this directory, with `CC` set if a C compiler
is not on PATH. Only Python's standard library, a GNU-compatible C compiler, and
POSIX threads are needed. The bringup wrapper is
`python3 -B bringup/m2468/scripts/test_note_jiiov.py`.

The runner extracts the actual driver functions and device state unchanged,
compiles them with `-Wall -Wextra -Werror`, and executes kernel/hardware boundary
shims. Thirteen groups check acquisition rollback, regulator failures and counts,
reset timing requests, IRQ/mask ordering, exact ioctl values and copy failures,
all six sysfs stores, concurrent power calls and dead descriptors, malformed
netlink and exit echo, DT/probe failures, module teardown order, wake-depth retry,
load-clear ordering, and irreversible power-failure retention. Temporary
build files live outside the source tree and are removed by the runner.

These are host contract tests. They do not implement or prove the Linux device
core's reference mechanics, real scheduling, GPIO/IRQ controller behavior,
regulator operation, TEE, or sensor behavior. Kernel compilation and later
authorized hardware validation remain separate checks.
