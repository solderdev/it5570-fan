# it5570-fan

Linux hwmon kernel module for the ITE IT5570 embedded controller, providing fan monitoring and control on mini PCs that lack native Linux fan support.

## Features

- Fan RPM monitoring
- PWM fan speed control (manual and automatic modes)
- 6 temperature sensors with labels
- Works with [coolercontrol](https://gitlab.com/coolercontrol/coolercontrol) for custom fan curves
- DKMS support — auto-rebuilds on kernel updates
- Restores automatic fan control on module unload

### sensors output

```
it5570_fan-isa-0000
Adapter: ISA adapter
fan1:        1705 RPM
CPU:          +50.0°C
Board:        +52.0°C
CPU Die:      +53.0°C
Heatsink:     +60.0°C
Chipset:      +55.0°C
EC:           +51.0°C
pwm1:             44%
```

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

| Device | APU | BIOS | EC |
|---|---|---|---|
| AceMagic W1 | AMD Ryzen 7 8745HS (Phoenix) | AMI PHXPM7B0 | ITE IT5570 rev 0x02 |

Many white-label mini PCs from various brands share the same motherboard and EC firmware. If your system has an ITE IT5570 EC at Super I/O port 0x4E, this driver will likely work. The DMI strings on these boards are typically "Default string" (unprogrammed), so the driver detects the chip by its hardware ID (0x5570) rather than DMI matching.

## Installation

### AUR (Arch, CachyOS, EndeavourOS, Manjaro)

```bash
yay -S it5570-fan-dkms
```

### Manual (any distro)

```bash
git clone https://github.com/passiveEndeavour/it5570-fan.git
cd it5570-fan

# Build and load
make
sudo insmod it5570_fan.ko

# Or install with DKMS (persists across kernel updates)
make dkms-install
```

The module auto-loads on boot if installed via the AUR package or DKMS. For manual installs, add `it5570_fan` to `/etc/modules-load.d/it5570_fan.conf`.

### CachyOS / Clang-built kernels

If your kernel was built with clang (CachyOS default), pass the compiler flags:

```bash
make CC=clang LD=ld.lld
```

The DKMS and AUR installations handle this automatically.

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

### coolercontrol

Install [coolercontrol](https://gitlab.com/coolercontrol/coolercontrol) and it will automatically detect the hwmon interface. You can then create custom fan curves using any of the 6 temperature sensors as input.

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

Status: reads and fan control both verified live. Remaining work: adapt the driver's register constants and PWM scaling (EC uses 0–100 %, hwmon uses 0–255), and map `pwm1_enable` 1/2 to EC modes 1/2.

## Technical Background

### The ITE IT5570

The IT5570 is an embedded controller (EC) from ITE Tech, built around an 8051 microcontroller core. Unlike ITE's Super I/O chips (IT8613, IT8720, etc.) which have well-documented hardware monitoring registers, the IT5570 is a programmable EC whose register layout is defined entirely by its firmware. There is no public programming guide for the fan control interface — the register map was determined through reverse engineering.

The IT5570 is commonly found in budget mini PCs, particularly white-label AMD Phoenix/Hawk Point systems sold under brands like AceMagic, Beelink, MinisForum, and others. These systems typically ship with no thermal management under Linux beyond the EC's built-in fan curve.

### How it works

The driver uses two access methods to communicate with the EC:

**ACPI EC interface** (ports 0x62/0x66) — Used for fan control and the primary sensors. The EC exposes a 256-byte register space via the standard ACPI EC read (cmd 0x80) and write (cmd 0x81) commands. The key discovery was register **0x0F**: writing 1–100 sets manual fan duty percentage, writing 0 returns to automatic control.

**SIO indirect SRAM access** (ports 0x4E/0x4F) — Used for the extended temperature sensors. The IT5570's SMFI (Shared Memory Flash Interface) provides indirect access to the full 8KB EC SRAM space via SIO config registers 0x2E/0x2F. The ACPI EC's 256-byte window maps to SRAM 0x400–0x4FF; the additional temperature sensors live outside this window at addresses like 0x05B9, 0x0C44, 0x0C4A, and 0x086A.

### EC register map

#### ACPI EC registers (offset from EC base)

| Offset | R/W | Description |
|---|---|---|
| 0x0E | R | Fan duty status (0–100%) |
| 0x0F | R/W | Fan duty control: 0 = auto, 1–100 = manual % |
| 0x22 | R | Fan RPM high byte |
| 0x23 | R | Fan RPM low byte |
| 0x26 | R | CPU temperature (°C, filtered) |
| 0xF1 | R | Board temperature (°C) |

#### EC SRAM registers (via SIO indirect)

| Address | Description |
|---|---|
| 0x05B9 | CPU die temperature (°C, raw/unfiltered, ~3–5s faster response) |
| 0x086A | EC internal temperature (°C) |
| 0x0C44 | Heatsink temperature (°C) |
| 0x0C4A | Chipset temperature (°C) |

### Reverse engineering methodology

The register map was determined through:

1. **DSDT analysis** — The ACPI tables revealed the EC at `\_SB_.PCI0.SBRG.EC0_` with command port 0x66 and data port 0x62, but contained no fan control methods — the firmware handles everything internally.
2. **EC SRAM diffing** — Dumping the full 8KB SRAM at idle, under CPU stress, and during cooldown, then comparing the dumps to identify registers that track temperature, RPM, and duty cycle.
3. **Brute-force register probing** — Systematically writing to each ACPI EC offset and monitoring fan RPM changes to find the control register (0x0F).
4. **Cross-referencing** — Comparing findings with the [ec-su_axb35](https://github.com/cmetz/ec-su_axb35-linux) driver for a similar ITE EC platform.

## hwmon sysfs interface

| Attribute | Description |
|---|---|
| `fan1_input` | Fan speed in RPM |
| `temp1_input` / `temp1_label` | CPU temperature (filtered) |
| `temp2_input` / `temp2_label` | Board temperature |
| `temp3_input` / `temp3_label` | CPU die temperature (raw, faster response) |
| `temp4_input` / `temp4_label` | Heatsink temperature |
| `temp5_input` / `temp5_label` | Chipset temperature |
| `temp6_input` / `temp6_label` | EC internal temperature |
| `pwm1` | Fan duty cycle (0–255) |
| `pwm1_enable` | 1 = manual, 2 = automatic (EC-controlled) |

## Contributing

If you have a mini PC with an ITE IT5570 EC, please test this driver and report your results by opening an issue with:
- Your device brand and model
- `sudo dmidecode -t system` output
- `sensors` output with the module loaded
- Whether fan control works correctly

## License

GPL-2.0 — see the [SPDX identifier](https://spdx.org/licenses/GPL-2.0-only.html) in the source.
