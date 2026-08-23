# it5570-fan — LattePanda Sigma fan control driver

Linux hwmon kernel module for the ITE IT5570 EC on the LattePanda Sigma.
Fork of a driver for AMD mini PCs that carry the same chip ID with
incompatible EC firmware; this fork is Sigma-only, gated on a DMI match,
and refuses to bind on the upstream hardware.

## Safety rules (critical)

- Never run commands that might put the system / stability at risk!
- Read-only EC transactions (cmd 0x80 via ports 0x66/0x62) are approved.
- EC writes (cmd 0x81), including fan duty control, need explicit user
  approval per experiment.
- EC write ordering: set duty (0x2D) before enabling manual mode
  (0x23=1), and restore auto mode (0x23=2) afterwards. 0x2D powers up
  at 0, so reversing the order stops the fan.

## Hardware facts

- `sensors-detect` reports "Found unknown chip with ID 0x5570" at 0x4E;
  no mainline driver exists for this EC.
- The DSDT declares the EC device with _STA=0, so the kernel's ACPI EC
  driver never binds — all EC access is raw port I/O on 0x62/0x66.
- The ACPI EC window maps to EC SRAM base 0x400. Intel's ADL RVP
  reference layout does NOT apply — those offsets read as zero on this
  firmware.
- Register map (verified live; full table, disassembly, and methodology
  in README "Reverse engineering the Sigma EC"): fan mode 0x23
  (1 = manual, 2 = auto curve, 3 = full speed), manual duty percent
  0x2D, RPM 0x2E/0x2F big-endian, CPU temp 0x70.
- The EC firmware binary blob lives in ../ec_firmware/.

## Project state

- The driver is fully ported and verified live end-to-end, including
  suspend/resume — see README "Verification results". Manual duty is
  floored at 10 %; pwm1_enable maps 0/1/2 → EC modes 3/1/2; EC auto
  control is restored on unload, suspend, and shutdown.
- Known platform quirk: the Sigma wakes from S3 instantly (xHCI /
  GPE 6D firmware issue, reproduced with the module unloaded). Not a
  driver bug — don't debug it as one.
- Packaging is out of scope: no AUR/PKGBUILD. Supported install path:
  clone → `make dkms-install` (DKMS registration, module install,
  modules-load.d autoload).
- Fan control: the persistent EC auto curve is the full solution — no
  fancontrol/thinkfan/daemon. The firmware's thermal-emergency branch
  only runs in auto mode, so a manual-mode daemon is strictly less
  safe; the SSD sits outside the CPU fan's airflow, so non-CPU temp
  sources are irrelevant.
- Curve persistence: solved. Module params (curve_slope, curve_start_pwm,
  curve_start_temp, curve_full_temp; /etc/modprobe.d/it5570_fan-curve.conf,
  template ships via dkms-install) re-apply the curve at probe and resume;
  sysfs curve_* attrs + curve_commit give staged live tuning. Both paths
  enforce start_pwm >= 26 and the byte-overflow invariant. curveset.py in
  ../ec_dumps/ is RE tooling only - never run it while the module is
  loaded (raw port I/O races the driver; curve_commit=0 is the live view).

## Workflow

- Prefer plain feature branches over git worktrees: a fresh worktree
  only contains committed files, so untracked project context silently
  goes missing there.
- `make` lists all targets. Builds land in build/; `make clean`
  removes it. Clang-built kernels are auto-detected from the kernel
  .config.
