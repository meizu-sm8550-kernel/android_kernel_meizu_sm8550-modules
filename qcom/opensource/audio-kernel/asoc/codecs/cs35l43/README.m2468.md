# M2468 CS35L43

Source: [Meizu 21 Pro public driver](https://github.com/meizu-8650-kernel/vendor_meizu_opensource_kernel-modules/tree/dafe79bf5dbb772def7805a05ac917fa55ae9855/cs35l43), retaining Cirrus copyright and GPL notices.

Adapted for the M2468 Android 13/Linux 5.15 kernel. The I2C remove callback uses the 5.15 signature. PCM playback configures RX and capture configures TX, matching the M2468 stock module; the machine driver supplies the two-slot bit clock. IRQ/DSP failures propagate, and teardown drains interrupt and firmware work before freeing resources.

The M2468 DT supplies addresses, regulators, reset/interrupt GPIOs, RCV/SPK prefixes and the `m2468-cs35l43` firmware stem. No Pro board parameters or tuning binaries are included. The Kalama machine adaptation selects TX3/TX4 and secondary MI2S dual-codec links when `qcom,enable_cs35l43` is set.

Local compilation and matching module CRC/dependencies pass. Nine pointer-free constant tables match the M2468 stock module, including PLL, register defaults and power sequences. This is a source adaptation with targeted binary comparison, not a claim of complete recovery of Meizu's original source or full binary equivalence. The stock module also contains register-access retry behavior not fully reproduced here. Firmware/HAL integration, sound-card registration, playback/capture, calibration and suspend/resume still require device validation.
