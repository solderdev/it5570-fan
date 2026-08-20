# it5570-fan

Linux hwmon kernel module for the ITE IT5570 embedded controller on the **LattePanda Sigma**, providing fan monitoring and control where there is no native Linux driver.

This fork is **LattePanda Sigma-only**: the probe is gated on a DMI match (`LattePanda` / `LattePanda Sigma`) and refuses to bind on anything else. The upstream project this was forked from targets white-label AMD mini PCs (AceMagic, Beelink, MinisForum, ...) that happen to carry the same IT5570 chip ID but run completely different EC firmware with an incompatible register layout — this driver's constants would silently do the wrong thing there. If you have one of those AMD-based mini PCs, use the [upstream project](https://github.com/passiveEndeavour/it5570-fan) instead.

## Features

- Fan RPM monitoring (one fan)
- PWM fan speed control — manual duty, full speed, and the EC's automatic curve
- One CPU temperature sensor
- Works with [coolercontrol](https://gitlab.com/coolercontrol/coolercontrol) for custom fan curves
- DKMS support — auto-rebuilds on kernel updates
- Restores automatic EC fan control on module unload, and hands control back to the EC across suspend/resume

### sensors output

Real capture from the live driver test (2026-08-20, kernel 6.12.103, EC auto mode at idle):

```
it5570_fan-isa-0000
Adapter: ISA adapter
fan1:        2061 RPM
CPU:          +48.0 C  
pwm1:             64%
```

`pwm1` reports the *last commanded* manual duty cycle, not the EC auto curve's live output — the EC exposes no readback of what the auto curve is currently doing, so in `pwm1_enable=2` (auto) this value is stale/last-known rather than live.

The `64%` above is an lm-sensors display quirk, not the duty: `sensors` 3.6.2 prints `pwm1` as the raw sysfs value divided by 2 (the sysfs `pwm1` at capture time was 128, i.e. 50 % EC duty; 204 displays as `102%`). Read `/sys/class/hwmon/hwmonN/pwm1` for the true hwmon 0–255 value.

## Is this driver for me?

If you ran `sensors-detect` and got this message:

```
Probing for Super-I/O at 0x4e/0x4f
Trying family `National Semiconductor/ITE'...               Yes
Found unknown chip with ID 0x5570
```

Then yes — this is the driver you need. The ITE IT5570 is an embedded controller not recognized by `sensors-detect` or the in-tree `it87` driver. There is no mainline Linux driver for this chip. This module provides the missing hwmon support.

You may also have found this page by searching for:
- `Found unknown chip with ID 0x5570`
- `ITE IT5570 Linux fan control`
- `IT5570 no driver Linux`
- `IT5570 hwmon driver`
- `IT5570E sensors-detect unknown chip`
- `mini PC fan always on Linux`
- `AceMagic fan control Linux`
- `Beelink fan control Linux`
- `MinisForum fan noise Linux`
- `mini PC fan loud Linux no control`

## Tested Hardware

| Device | APU | EC firmware | Status |
|---|---|---|---|
| LattePanda Sigma | Intel Core i5-1340P (Alder Lake-P) | ITE EC-V14.6, `LP-EC-WTADLC1R210-V1.02` | Driver tested live end-to-end (2026-08-20, kernel 6.12.103): load, manual duty, 10 % floor clamp, full speed, auto restore, unload, suspend/resume — see "Verification results" |

*Supported by the upstream project, not this fork:* the AMD Phoenix/Hawk Point white-label mini PCs (AceMagic, Beelink, MinisForum, etc.) that the original project targets also carry an ITE IT5570 EC, but with a completely different register layout. This fork's DMI gate refuses to bind on that hardware; use the [upstream project](https://github.com/passiveEndeavour/it5570-fan) there instead.

## Installation

This fork is not published to the AUR — install straight from a clone.

Prerequisites: `dkms` and the headers for your kernel (Manjaro/Arch: `sudo pacman -S dkms linux-headers`).

```bash
git clone <this repo>
cd it5570-fan

# Register with DKMS, build and install for the running kernel,
# and enable auto-load at boot (modules-load.d):
make dkms-install
sudo modprobe it5570_fan
```

DKMS rebuilds the module automatically on every kernel update. `make dkms-remove` uninstalls everything again (module, DKMS registration, boot autoload).

For a quick one-off test without installing anything: `make && sudo insmod it5570_fan.ko` (and `sudo rmmod it5570_fan` when done).

### CachyOS / Clang-built kernels

If your kernel was built with clang (CachyOS default), pass the compiler flags:

```bash
make CC=clang LD=ld.lld
```

The DKMS installation handles this automatically.

## Usage

### Reading sensors

```bash
sensors it5570_fan-isa-0000
```

### Manual fan control

```bash
# Switch to manual mode
echo 1 | sudo tee /sys/class/hwmon/hwmon*/pwm1_enable

# Set fan speed (0-255, where 255 = 100%)
echo 128 | sudo tee /sys/class/hwmon/hwmon*/pwm1

# Return to automatic EC control
echo 2 | sudo tee /sys/class/hwmon/hwmon*/pwm1_enable
```

### Thermal safety

Manual mode (`pwm1_enable=1`) hands duty control to the host. Whether the EC firmware still applies its own thermal override underneath manual mode (e.g. forcing full speed past some temperature) has not been established through reverse engineering — treat host-side thermal management (e.g. a fan curve daemon) as load-bearing when using manual mode, with CPU self-throttling as the last-resort backstop if you don't set one up.

The driver limits how bad a hung or crashed fan controller can get:
- Manual duty is floored at 10 % (see "hwmon sysfs interface" below) — it can never command the fan fully off.
- Fan control reverts to the EC's automatic curve when the module is unloaded, when the system suspends, and on shutdown/reboot; on resume, the manual/full state active before suspend is re-applied.

### coolercontrol

Install [coolercontrol](https://gitlab.com/coolercontrol/coolercontrol) and it will automatically detect the hwmon interface. You can then create a custom fan curve using the CPU temperature sensor as input.

## LattePanda Sigma port (work in progress)

This fork targets the [LattePanda Sigma](https://docs.lattepanda.com/content/sigma_edition/EC_Firmware/) (Intel Core i5-1340P), which also uses an ITE IT5570 EC (`sensors-detect`: "Found unknown chip with ID 0x5570" at 0x4E) — but with **completely different EC firmware**, so the register map above does not apply.

Findings (2026-08-19, against flashed EC firmware V1.02):

- The Sigma's EC firmware (`LP-EC-WTADLC1R210-V1.02.bin`, "ITE EC-V14.6", "INTEL ADL P") is derived from Intel's **Alder Lake-P RVP reference EC firmware**, with DFRobot customizations.
- The Sigma DSDT declares the EC device (`H_EC`, PNP0C09) but `_STA` returns Zero and all EC access methods are stubbed. Linux therefore never binds its ACPI EC driver (`ec_sys` exposes nothing) — raw port I/O at 0x62/0x66 is required, as this driver already does.
- The DSDT stub retains ~20 register-name constants matching Intel's published [ADL RVP EC layout](https://github.com/slimbootloader/slimbootloader/blob/master/Platform/AlderlakeBoardPkg/AcpiTables/Dsdt/EC.ASL) — but live probing showed those offsets read as zero: DFRobot's firmware customization moved the ACPI window contents, so the RVP layout does **not** apply.

### Sigma EC register map (ACPI EC space, verified live)

Reads were confirmed by sampling all 256 EC bytes at 1 Hz through an idle → 8-core load → cooldown cycle and correlating against `coretemp` (Pearson r in parentheses); writes were confirmed by a live fan-control test.

| Offset | R/W | Description |
|---|---|---|
| 0x2E/0x2F | R | CPU fan RPM, 16-bit **big-endian** (~1200 idle → ~3000 load); mirrored at 0x76/0x77 |
| 0x70 | R | CPU temperature, °C (r=+0.94 vs coretemp) |
| 0x96/0x97 | R | CPU temperature, 0.1 °C units, 16-bit **little-endian** (r=+0.95) |
| 0x60 | R | Slow-moving temperature, °C — board temp candidate |
| 0x23 | **R/W** | **Fan mode: 0 = off, 1 = manual, 2 = auto curve (default), 3 = full speed** |
| 0x2D | **R/W** | **Manual duty in percent (0–100), applied when mode = 1. Defaults to 0** |
| 0x28–0x2B | R/W | Auto-curve parameters: slope divisor, base duty %, low temp, high temp |

Auto-curve defaults differ between EC firmware revisions (V1.02: 0x28=3, 0x29=40, 0x2A=30, 0x2B=80), so treat them as tunables rather than constants.

### Fan control interface (recovered from firmware disassembly)

The ACPI EC window maps to EC SRAM at **0x400** (EC offset `n` = SRAM `0x400+n`), confirmed because the firmware's `MOV DPTR,#0x04xx` sites line up exactly with the offsets found by live probing.

The fan routine at file offset **0xA7AB** dispatches on EC offset 0x23:

```asm
0a7d6:  MOV DPTR,#0x0423   ; fan mode
0a7d9:  MOVX A,@DPTR
0a7da:  JNZ  0xa7e0        ; mode 0 -> duty register = 0
0a7dc:  MOV DPTR,#0x1804   ; PWM duty hardware register
0a7df:  MOVX @DPTR,A
...
0a7e4:  XRL  A,#0x01       ; mode 1 -> manual percent
0a7e8:  MOV DPTR,#0x1841   ; PWM max/period value
0a7ed:  MOV DPTR,#0x042d   ; duty percent from host
0a7f0:  MOV R5,#0x64       ; scale = 100
0a7f2:  LCALL 0xa8bb       ; [0x1804] = [0x042d] * [0x1841] / 100
```

Crucially, the firmware **only ever reads** offsets 0x23 and 0x2D — it never writes them. They are host-input fields, so this is an intended control path rather than a side effect.

To control the fan: write the duty percent to **0x2D**, then write **1** to **0x23**. Write **2** to 0x23 to hand control back to the EC's automatic curve. Mode 3 forces 100 %.

Write 0x2D **before** switching to mode 1. It defaults to 0, so entering manual mode first would briefly stop the fan.

Fan RPM is computed by the firmware as **RPM = 2156250 / tach_counter** (routine at 0xA753, tach hardware register 0x181E/0x181F) and published to 0x2E/0x2F.

### Verification results

Live test on EC firmware V1.02: writing 0x2D=80 then 0x23=1 raised the fan from 1131 to 2481 RPM within 5 s and pulled the CPU from 51 °C to 45 °C. Restoring 0x23=2 returned control to the EC. RPM decays gradually rather than dropping instantly, matching the firmware's incremental ramp logic at 0xA880.

Mode 3 full-speed test (2026-08-19): at 50 °C baseline, writing 0x2D=50 then 0x23=3 raised the fan from ~1264 to ~3028 RPM within 5 s, well above the 2481 RPM observed at 80 % duty in manual mode. Restoring 0x23=2 returned control to the EC; RPM decayed to 3007, confirming expected firmware ramp-down. Mode 3 verified as full speed; `pwm1_enable=0` may safely map to mode 3.

Live driver test (2026-08-20, kernel 6.12.103-1-MANJARO): the compiled module was loaded and exercised end-to-end through sysfs. Probe reported CPU 50 °C, 2867 RPM, mode 2, and all reads tracked the EC (temp1_input in m°C, `temp1_label` = CPU, `pwm1_enable` = 2). Manual mode at `pwm1`=204 (80 %) reached 2487 RPM, matching the 2481 RPM raw-port result. Writing `pwm1`=0 clamped to the 10 % floor (readback 26) with the fan stable and spinning at ~209 RPM. `pwm1_enable`=0 (EC mode 3) reached 3028 RPM with `pwm1` reading 255; `pwm1_enable`=2 handed control back to the auto curve with the usual gradual ramp-down, and `rmmod` logged "fan control restored to auto mode".

Suspend/resume test (2026-08-20): with manual mode at 80 % active, the system entered S3 and the resume hook re-applied the manual state — `pwm1`=204, `pwm1_enable`=1, fan back at 2478 RPM — with no driver warnings in the log. One platform caveat, unrelated to this driver: the Sigma wakes from S3 immediately (asleep for under a second, ACPI GPE 6D / PME_B0 firing with an xHCI "xHC error in resume, USBSTS 0x401" on every cycle). A control suspend with the module unloaded reproduced the instant wake exactly, so suspend-hold time is a firmware/USB issue, not an EC or driver one.

Status: reads and fan control verified live, and the driver has since been adapted to this register map — register constants, PWM scaling (EC 0–100 % ↔ hwmon 0–255), and `pwm1_enable` mode mapping (0/1/2 → EC modes 3/1/2) are implemented and documented in `it5570_fan.c` and in "hwmon sysfs interface" below. The full live driver test, including suspend/resume, has passed. Remaining work: packaging.

## Technical Background

### The ITE IT5570

The IT5570 is an embedded controller (EC) from ITE Tech, built around an 8051 microcontroller core. Unlike ITE's Super I/O chips (IT8613, IT8720, etc.) which have well-documented hardware monitoring registers, the IT5570 is a programmable EC whose register layout is defined entirely by its firmware. There is no public programming guide for the fan control interface — the register map was determined through reverse engineering.

The IT5570 is commonly found in budget mini PCs, particularly white-label AMD Phoenix/Hawk Point systems sold under brands like AceMagic, Beelink, MinisForum, and others. These systems typically ship with no thermal management under Linux beyond the EC's built-in fan curve.

### How it works

The driver talks to the EC entirely through the ACPI EC ports (0x62/0x66), using the standard read (cmd 0x80) and write (cmd 0x81) transactions. On the Sigma this raw port I/O is mandatory, not just an implementation choice: the DSDT declares the EC device but its `_STA` returns zero, so the kernel's own ACPI EC driver never binds and `ec_sys` exposes nothing. The Super I/O ports (0x4E/0x4F) are used only once, at module load, to confirm the IT5570 chip ID before probing further — the Sigma has no extended-temperature SRAM sensors reachable through them the way the original AMD-based hardware did.

### EC register map

This is the map the driver actually uses. It was recovered by live probing and firmware disassembly against the Sigma's EC firmware — see ["LattePanda Sigma port"](#lattepanda-sigma-port-work-in-progress) below for the full register table, the disassembly, and how each field was verified.

| Offset | R/W | Description |
|---|---|---|
| 0x23 | R/W | Fan mode: 1 = manual, 2 = auto curve (default), 3 = full speed (0 = off; the driver never writes it) |
| 0x2D | R/W | Manual duty percent (0–100), applied only while mode = 1 |
| 0x2E/0x2F | R | Fan RPM, 16-bit big-endian |
| 0x70 | R | CPU temperature, °C |

### Reverse engineering methodology (original AMD hardware)

This section describes how the upstream project found *its* register map — it does not apply to the Sigma map above, which was found by a different process (firmware disassembly and live correlation against `coretemp`; see "LattePanda Sigma port"). Kept here as background on the original AMD-based board's EC (offsets 0x0E/0x0F duty, 0x22/0x23 RPM, 0x26/0xF1 temperature, plus SRAM-indirect temperature sensors), which this fork no longer targets:

1. **DSDT analysis** — The ACPI tables revealed the EC at `\_SB_.PCI0.SBRG.EC0_` with command port 0x66 and data port 0x62, but contained no fan control methods — the firmware handles everything internally.
2. **EC SRAM diffing** — Dumping the full 8KB SRAM at idle, under CPU stress, and during cooldown, then comparing the dumps to identify registers that track temperature, RPM, and duty cycle.
3. **Brute-force register probing** — Systematically writing to each ACPI EC offset and monitoring fan RPM changes to find the control register.
4. **Cross-referencing** — Comparing findings with the [ec-su_axb35](https://github.com/cmetz/ec-su_axb35-linux) driver for a similar ITE EC platform.

## hwmon sysfs interface

| Attribute | Description |
|---|---|
| `fan1_input` | Fan speed in RPM |
| `temp1_input` / `temp1_label` | CPU temperature (label: `CPU`) |
| `pwm1` | Fan duty, scaled 0–255 from the EC's native 0–100 % (`pwm1 = round(EC% * 255 / 100)`). While `pwm1_enable=2` (auto), reports the *last commanded* manual duty rather than the EC auto curve's live output — the EC exposes no readback of what the curve is currently doing. |
| `pwm1_enable` | `0` = full speed (EC mode 3) · `1` = manual (EC mode 1) · `2` = EC automatic curve (EC mode 2, default) |

**10 % manual-duty floor:** the driver clamps every manual-mode duty write to a 10–100 % range, so `pwm1` values 1–25 are accepted but round-trip up to 26 on readback (`round(10% * 255 / 100) = 26`) — values 1–25 are simply unreachable when reading `pwm1` back. If you run `pwmconfig` against this driver, it will find the fan never fully stops and will record `MINPWM` around 26 instead of 0 — that's the floor working as intended, not a bug.

## Contributing

If you have a mini PC with an ITE IT5570 EC, please test this driver and report your results by opening an issue with:
- Your device brand and model
- `sudo dmidecode -t system` output
- `sensors` output with the module loaded
- Whether fan control works correctly

## License

GPL-2.0 — see the [SPDX identifier](https://spdx.org/licenses/GPL-2.0-only.html) in the source.
