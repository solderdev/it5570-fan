# control fan rpm from linux

This repository is a fork from a repo using this software on other hardware than mine.

The goal is to adopt the software to run on my Lattepanda Sigma PC.

It seems the Lattepanda Sigma uses an ITE IT5570 embedded controller as super-I/O controller.
Output from "sudo sensors-detect":
...
Some Super I/O chips contain embedded sensors. We have to write to
standard I/O ports to probe them. This is usually safe.
Do you want to scan for Super I/O sensors? (YES/no):
..
Probing for Super-I/O at 0x4e/0x4f
Trying family `National Semiconductor/ITE'...               Yes
Found unknown chip with ID 0x5570
...


The Lattepanda Sigma EC firmware binary blob is stored in ../ec_firmware directory.
The verified register map and fan control interface are documented in README.md
("LattePanda Sigma port"): read RPM at 0x2E/0x2F (big-endian) and CPU temp at 0x70;
control the fan via mode register 0x23 (1 = manual) plus duty percent at 0x2D.
The ACPI EC window maps to EC SRAM base 0x400.
The Sigma DSDT disables the ACPI EC device (_STA=0), so all access is raw port I/O.
Intel's ADL RVP reference layout does not apply — it reads as zero on this firmware.

Current state: it5570_fan.c is adapted to the Sigma register map (mode 0x23, duty
percent 0x2D, RPM 0x2E/0x2F big-endian, CPU temp 0x70); the constants and full
read/write semantics are documented in the file header and in README.md. The probe
is Sigma-only, gated on a DMI match, and refuses to bind on the upstream AMD
hardware. Manual duty is floored at 10% and pwm1_enable maps 0=full/1=manual/2=EC
auto curve; suspend hands the fan to the EC auto curve and resume re-applies the
manual/full state that was active before suspend, restoring EC auto control on
module unload. Remaining: the live driver test (plan Task 8) has not run yet, then
packaging.

A live fan-control write test (0x2D=80 then 0x23=1) was run once with per-experiment
approval on 2026-08-19 and worked; see README "Verification results". When writing,
set 0x2D before 0x23=1, and restore 0x23=2 afterwards.

Workflow: prefer plain feature branches over git worktrees. A fresh worktree only
contains committed files, so untracked project context (like this file, before it
was tracked) silently goes missing there.

Critical:
- never run commands which might put the system / stability at risk!
- read-only EC transactions (cmd 0x80 via ports 0x66/0x62) are approved.
- EC writes (cmd 0x81), including fan duty control, still need explicit user
  approval per experiment.
