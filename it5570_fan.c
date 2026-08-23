// SPDX-License-Identifier: GPL-2.0
/*
 * LattePanda Sigma fan control driver (ITE IT5570 EC)
 *
 * Copyright (C) 2026 Michael (original IT5570 EC driver,
 *   https://github.com/passiveEndeavour/it5570-fan)
 * Copyright (C) 2026 solderdev (LattePanda Sigma port)
 *
 * hwmon interface for the DFRobot LattePanda Sigma's EC fan control.
 * Sigma-only: the same IT5570 chip ID appears on unrelated boards
 * (including the upstream AMD mini-PCs this driver was forked from)
 * with entirely different firmware register assignments, so the probe
 * is gated on DMI. The Intel ADL RVP reference layout also does not
 * apply to this firmware.
 *
 * EC register map (ACPI EC offsets; the window maps to EC SRAM 0x400+n).
 * Verified by live probing and firmware disassembly — see README
 * "LattePanda Sigma port":
 *   0x23 - fan mode: 0 off, 1 manual, 2 auto curve (default), 3 full
 *   0x2D - manual duty percent 0-100, applied only in mode 1
 *   0x2E - fan RPM high byte (big-endian pair)
 *   0x2F - fan RPM low byte
 *   0x70 - CPU temperature (°C)
 *
 * Write ordering is safety-critical: 0x2D powers up at 0, so duty must
 * be written before mode 1 or the fan would stop on manual-mode entry.
 *
 * pwm1 reports the last commanded manual duty; the EC exposes no
 * readback of the auto curve's live output.
 *
 * The ACPI DSDT declares the EC device with _STA=0, so the kernel's
 * ACPI EC driver never binds and raw port I/O on 0x62/0x66 is safe.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/dmi.h>
#include <linux/pm.h>
#include <linux/hwmon-sysfs.h>

#define DRIVER_NAME	"it5570_fan"

/* Super I/O ports for chip detection */
#define SIO_PORT	0x4E
#define SIO_DATA	0x4F

/* ITE IT5570 chip ID */
#define IT5570_DEVID	0x5570

/* ACPI EC ports */
#define EC_SC		0x66	/* EC status/command */
#define EC_DATA		0x62	/* EC data */

/* EC status bits */
#define EC_SC_IBF	BIT(1)	/* Input buffer full */
#define EC_SC_OBF	BIT(0)	/* Output buffer full */

/* EC commands */
#define EC_CMD_READ	0x80
#define EC_CMD_WRITE	0x81

/* EC register offsets (ACPI EC address space = EC SRAM 0x400 + n) */
#define EC_REG_FAN_MODE		0x23	/* 0 off, 1 manual, 2 auto, 3 full */
#define EC_REG_FAN_DUTY		0x2D	/* manual duty %, applied in mode 1 only */
#define EC_REG_FAN_RPM_HI	0x2E	/* big-endian pair */
#define EC_REG_FAN_RPM_LO	0x2F
#define EC_REG_CPU_TEMP		0x70	/* °C */

/*
 * EC auto-curve parameters (BIOS rewrites these on every boot; the
 * firmware only reads them, so host writes persist until then).
 * duty/255 = start_pwm + (T - start_temp) * slope, jumping to 255 at
 * full_temp; full_temp > 100 is unreachable (EC temp saturates at 100)
 * and turns the jump - the firmware's only thermal-emergency path -
 * into a cap on the curve instead.
 */
#define EC_REG_CURVE_SLOPE	0x28	/* duty steps (/255) per °C, raw multiplier */
#define EC_REG_CURVE_START_PWM	0x29	/* duty (/255) at start_temp */
#define EC_REG_CURVE_START_TEMP	0x2A	/* °C where the curve engages */
#define EC_REG_CURVE_FULL_TEMP	0x2B	/* °C where duty jumps to 255 */

/* Fan mode register values */
#define EC_FAN_MODE_OFF		0	/* comparison only — NEVER written */
#define EC_FAN_MODE_MANUAL	1
#define EC_FAN_MODE_AUTO	2
#define EC_FAN_MODE_FULL	3

/* PWM conversion: hwmon uses 0-255, EC uses percent */
#define EC_DUTY_MIN		10	/* safety floor for manual duty */
#define EC_DUTY_MAX		100
#define EC_DUTY_DEFAULT		50	/* used when no sane cached duty exists */
#define EC_TEMP_MAX_C		120	/* plausibility ceiling; above = EC read glitch */
#define HWMON_PWM_MAX		255

/*
 * Curve validation: the firmware keeps only the low byte of the curve
 * target, so a target > 255 wraps mod 256 and the fan SLOWS DOWN as the
 * CPU heats. The EC temperature saturates at EC_CURVE_TEMP_CAP, which
 * bounds the reachable part of the curve.
 */
#define EC_CURVE_TEMP_CAP	100	/* °C; EC temp = 100 - DTS margin */
/*
 * EC_DUTY_MIN in /255 units (= 26): floors the curve's *engaged* duty.
 * Below start_temp - 5 the firmware still parks the fan at 0 by design.
 */
#define EC_CURVE_PWM_FLOOR	DIV_ROUND_UP(EC_DUTY_MIN * HWMON_PWM_MAX, \
					     EC_DUTY_MAX)

enum {
	CURVE_SLOPE,
	CURVE_START_PWM,
	CURVE_START_TEMP,
	CURVE_FULL_TEMP,
	EC_CURVE_NREGS
};

static const u8 it5570_curve_regs[EC_CURVE_NREGS] = {
	EC_REG_CURVE_SLOPE, EC_REG_CURVE_START_PWM,
	EC_REG_CURVE_START_TEMP, EC_REG_CURVE_FULL_TEMP,
};

/*
 * Optional boot-time curve, normally set in
 * /etc/modprobe.d/it5570_fan-curve.conf. Negative = unset; ignored with
 * a warning unless all four are set and pass it5570_curve_check().
 * Not runtime-writable (perm 0): the sysfs curve_* attributes are the
 * runtime interface.
 */
static int curve_slope = -1;
module_param(curve_slope, int, 0);
MODULE_PARM_DESC(curve_slope, "EC auto-curve slope: duty steps (/255) per °C, 0-255 (all four curve_* parameters must be set together)");
static int curve_start_pwm = -1;
module_param(curve_start_pwm, int, 0);
MODULE_PARM_DESC(curve_start_pwm, "EC auto-curve duty (/255) at curve_start_temp, 26-255");
static int curve_start_temp = -1;
module_param(curve_start_temp, int, 0);
MODULE_PARM_DESC(curve_start_temp, "EC auto-curve start temperature in °C, 0-255");
static int curve_full_temp = -1;
module_param(curve_full_temp, int, 0);
MODULE_PARM_DESC(curve_full_temp, "temperature in °C where the EC jumps to full speed; >100 disables the jump and caps the curve, 0-255");

struct it5570_data {
	struct mutex lock;	/* guards cached fields and cache->EC-write ordering */
	unsigned long last_updated;
	bool valid;
	bool temp_valid;	/* at least one plausible temp sample cached */
	bool shutting_down;	/* shutdown restored auto mode; refuse new writes */

	/* Cached sensor values */
	unsigned int fan_rpm;
	unsigned int fan_duty;	/* 0-100, cached 0x2D */
	unsigned int fan_mode;	/* cached 0x23 */
	unsigned int cpu_temp;	/* °C */

	/* EC auto-curve staging (offsets 0x28-0x2B) */
	u8 curve_staged[EC_CURVE_NREGS];	/* sysfs-visible values */
	u8 curve_ec[EC_CURVE_NREGS];	/* last curve known to be in the EC */
	u8 curve_written;	/* bitmask: staged slot written since load */
	bool curve_valid;	/* curve_ec reflects the EC */
};

static struct platform_device *it5570_pdev;
static DEFINE_MUTEX(ec_io_mutex);

/*
 * Low-level ACPI EC access
 *
 * We use direct port I/O rather than the kernel's ACPI EC interface
 * to avoid potential conflicts with the ACPI EC driver's transaction
 * handling. The kernel ec_read/ec_write functions may not be exported
 * on all configurations.
 */
/*
 * Poll the EC status register until (status & mask) == val, for up to
 * 100 ms. Sleeping poll: every caller is process context holding
 * ec_io_mutex, and per-byte EC latency dwarfs scheduler wakeup jitter.
 */
static int ec_wait_status(u8 mask, u8 val)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(100);

	do {
		if ((inb(EC_SC) & mask) == val)
			return 0;
		usleep_range(50, 150);
	} while (time_before(jiffies, timeout));

	/* one last look: we may have slept past the deadline */
	return (inb(EC_SC) & mask) == val ? 0 : -ETIMEDOUT;
}

static int ec_wait_ibf_clear(void)
{
	return ec_wait_status(EC_SC_IBF, 0);
}

static int ec_wait_obf_set(void)
{
	return ec_wait_status(EC_SC_OBF, EC_SC_OBF);
}

/*
 * A previously timed-out transaction can leave a stale byte in OBF,
 * which would be misattributed to the next read. Drain before starting
 * any transaction. Caller must hold ec_io_mutex.
 */
static void ec_drain_obf(void)
{
	int i;

	for (i = 0; i < 100 && (inb(EC_SC) & EC_SC_OBF); i++)
		inb(EC_DATA);
}

static int ec_read_byte(u8 offset, u8 *val)
{
	int ret;

	mutex_lock(&ec_io_mutex);
	ec_drain_obf();

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(EC_CMD_READ, EC_SC);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(offset, EC_DATA);

	ret = ec_wait_obf_set();
	if (ret)
		goto out;
	*val = inb(EC_DATA);

out:
	mutex_unlock(&ec_io_mutex);
	return ret;
}

static int ec_write_byte(u8 offset, u8 val)
{
	int ret;

	mutex_lock(&ec_io_mutex);
	ec_drain_obf();

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(EC_CMD_WRITE, EC_SC);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(offset, EC_DATA);

	ret = ec_wait_ibf_clear();
	if (ret)
		goto out;
	outb(val, EC_DATA);

out:
	mutex_unlock(&ec_io_mutex);
	return ret;
}

/*
 * Super I/O chip detection
 */
static void sio_enter(void)
{
	outb(0x87, SIO_PORT);
	outb(0x01, SIO_PORT);
	outb(0x55, SIO_PORT);
	outb(0xaa, SIO_PORT);
}

static void sio_exit(void)
{
	outb(0x02, SIO_PORT);
	outb(0x02, SIO_DATA);
}

static u8 sio_read(u8 reg)
{
	outb(reg, SIO_PORT);
	return inb(SIO_DATA);
}

static int it5570_detect(void)
{
	u16 devid;

	if (!request_region(SIO_PORT, 2, DRIVER_NAME))
		return -EBUSY;

	sio_enter();
	devid = (sio_read(0x20) << 8) | sio_read(0x21);
	sio_exit();

	release_region(SIO_PORT, 2);

	if (devid != IT5570_DEVID) {
		pr_info(DRIVER_NAME ": chip ID 0x%04x not supported\n", devid);
		return -ENODEV;
	}

	pr_info(DRIVER_NAME ": found ITE IT5570 (ID 0x%04x)\n", devid);
	return 0;
}

/*
 * Update cached sensor data (rate-limited to 1 Hz).
 * Caller must hold data->lock.
 */
static int it5570_update(struct it5570_data *data)
{
	u8 hi, hi2, lo, duty, mode, temp;
	int ret;

	lockdep_assert_held(&data->lock);

	if (data->valid && time_before(jiffies, data->last_updated + HZ))
		return 0;

	ret = ec_read_byte(EC_REG_FAN_RPM_HI, &hi);
	if (ret)
		goto err;
	ret = ec_read_byte(EC_REG_FAN_RPM_LO, &lo);
	if (ret)
		goto err;
	/*
	 * hi/lo are two separate EC transactions; if the count crossed a
	 * 256-count boundary in between, the pair is torn. Re-read hi and
	 * retry lo once - a second consecutive tear is vanishingly rare
	 * and costs one glitchy 1 Hz sample, not a control decision.
	 */
	ret = ec_read_byte(EC_REG_FAN_RPM_HI, &hi2);
	if (ret)
		goto err;
	if (hi2 != hi) {
		hi = hi2;
		ret = ec_read_byte(EC_REG_FAN_RPM_LO, &lo);
		if (ret)
			goto err;
	}
	ret = ec_read_byte(EC_REG_FAN_DUTY, &duty);
	if (ret)
		goto err;
	ret = ec_read_byte(EC_REG_FAN_MODE, &mode);
	if (ret)
		goto err;
	ret = ec_read_byte(EC_REG_CPU_TEMP, &temp);
	if (ret)
		goto err;

	data->fan_rpm = (hi << 8) | lo;
	if (data->fan_rpm == 0xFFFF)
		data->fan_rpm = 0;	/* saturated tach: stopped/unplugged */
	data->fan_duty = duty;
	data->fan_mode = mode;
	if (temp == 0 || temp > EC_TEMP_MAX_C) {
		pr_warn_ratelimited(DRIVER_NAME ": implausible CPU temp %u, keeping last value\n",
				    temp);
	} else {
		data->cpu_temp = temp;
		data->temp_valid = true;
	}

	if (mode < EC_FAN_MODE_MANUAL || mode > EC_FAN_MODE_FULL)
		pr_warn_ratelimited(DRIVER_NAME ": unexpected fan mode %u\n",
				    mode);

	data->last_updated = jiffies;
	data->valid = true;
	return 0;

err:
	data->valid = false;
	return ret;
}

/*
 * Fan control helpers. All require data->lock held: the read-cache ->
 * EC-write -> update-cache sequences below must not interleave between
 * two writers, or a stale duty could land in the EC after a fresh one.
 * Lock nesting is data->lock -> ec_io_mutex throughout the driver.
 */

/* Single enforcement point for the manual-duty safety floor. */
static int it5570_write_duty(struct it5570_data *data, unsigned int percent)
{
	int ret;

	percent = clamp_val(percent, EC_DUTY_MIN, EC_DUTY_MAX);
	ret = ec_write_byte(EC_REG_FAN_DUTY, percent);
	if (ret) {
		data->valid = false;
		return ret;
	}
	data->fan_duty = percent;
	return 0;
}

/*
 * The only path into manual mode. Duty is written before mode 1:
 * 0x2D powers up at 0, so mode-first would stop the fan.
 */
static int it5570_set_manual(struct it5570_data *data, unsigned int percent)
{
	int ret;

	ret = it5570_write_duty(data, percent);
	if (ret)
		return ret;	/* duty not set -> do not enter manual mode */

	ret = ec_write_byte(EC_REG_FAN_MODE, EC_FAN_MODE_MANUAL);
	if (ret) {
		data->valid = false;	/* duty landed, mode unknown */
		return ret;
	}
	data->fan_mode = EC_FAN_MODE_MANUAL;
	return 0;
}

/*
 * Direct mode setting for auto/full only. Manual goes through
 * it5570_set_manual(); mode 0 (fan off) is never written, enforced here.
 */
static int it5570_set_mode(struct it5570_data *data, unsigned int mode)
{
	int ret;

	if (mode != EC_FAN_MODE_AUTO && mode != EC_FAN_MODE_FULL)
		return -EINVAL;

	ret = ec_write_byte(EC_REG_FAN_MODE, mode);
	if (ret) {
		data->valid = false;
		return ret;
	}
	data->fan_mode = mode;
	return 0;
}

/*
 * EC auto-curve helpers. All require data->lock held (same nesting as
 * the fan-control helpers: data->lock -> ec_io_mutex).
 */

/*
 * Read EC 0x28-0x2B into the last-known copy, and into every staged
 * slot too unless keep_staged is set, in which case slots the user has
 * written since load are preserved.
 */
static int it5570_curve_refresh(struct it5570_data *data, bool keep_staged)
{
	u8 vals[EC_CURVE_NREGS];
	int i, ret;

	lockdep_assert_held(&data->lock);

	for (i = 0; i < EC_CURVE_NREGS; i++) {
		ret = ec_read_byte(it5570_curve_regs[i], &vals[i]);
		if (ret)
			return ret;
	}
	for (i = 0; i < EC_CURVE_NREGS; i++) {
		data->curve_ec[i] = vals[i];
		if (!keep_staged || !(data->curve_written & BIT(i)))
			data->curve_staged[i] = vals[i];
	}
	if (!keep_staged)
		data->curve_written = 0;
	data->curve_valid = true;
	return 0;
}

/*
 * Validate a curve 4-tuple; logs the rejection reason ("what" names the
 * caller for the log). start_pwm is floored like manual duty: without
 * it, a curve such as slope=0/start_pwm=0 would hold the fan off at
 * 100 °C with the emergency branch disabled (full_temp > 100).
 */
static bool it5570_curve_check(const u8 *curve, const char *what)
{
	int ceiling = min((int)curve[CURVE_FULL_TEMP] - 1, EC_CURVE_TEMP_CAP);
	int peak = curve[CURVE_START_PWM] +
		   max(0, ceiling - (int)curve[CURVE_START_TEMP]) *
		   curve[CURVE_SLOPE];

	if (curve[CURVE_START_PWM] < EC_CURVE_PWM_FLOOR) {
		pr_warn(DRIVER_NAME ": %s: start_pwm %u below floor %u (10%% duty)\n",
			what, curve[CURVE_START_PWM], EC_CURVE_PWM_FLOOR);
		return false;
	}
	if (curve[CURVE_START_TEMP] > EC_CURVE_TEMP_CAP) {
		pr_warn(DRIVER_NAME ": %s: start_temp %u above the %u °C EC ceiling - the fan would never start\n",
			what, curve[CURVE_START_TEMP], EC_CURVE_TEMP_CAP);
		return false;
	}
	if (peak > 255) {
		pr_warn(DRIVER_NAME ": %s: curve target reaches %d (>255) at %d °C and would wrap - the fan would slow down when hot\n",
			what, peak, ceiling);
		return false;
	}
	return true;
}

/*
 * Write a validated curve to the EC with per-byte read-back. On any
 * failure the last-known curve is rewritten: a half-applied mix of two
 * individually valid curves can itself violate the overflow invariant.
 * "curve" must not alias data->curve_ec (rollback source).
 */
static int it5570_curve_apply(struct it5570_data *data, const u8 *curve)
{
	int i, ret = 0;
	u8 rb;

	lockdep_assert_held(&data->lock);

	for (i = 0; i < EC_CURVE_NREGS; i++) {
		ret = ec_write_byte(it5570_curve_regs[i], curve[i]);
		if (!ret) {
			ret = ec_read_byte(it5570_curve_regs[i], &rb);
			if (!ret && rb != curve[i])
				ret = -EIO;
		}
		if (ret)
			break;
	}
	if (ret) {
		bool rb_failed = false;

		for (i = 0; i < EC_CURVE_NREGS; i++)
			if (ec_write_byte(it5570_curve_regs[i],
					  data->curve_ec[i]))
				rb_failed = true;
		if (rb_failed) {
			pr_err(DRIVER_NAME ": curve write failed (%d) and rollback failed - EC curve state unknown\n",
			       ret);
			data->curve_valid = false;
			data->valid = false;
		} else {
			pr_warn(DRIVER_NAME ": curve write failed (%d), previous curve restored\n",
				ret);
		}
		return ret;
	}

	memcpy(data->curve_ec, curve, EC_CURVE_NREGS);
	data->curve_valid = true;
	pr_info(DRIVER_NAME ": applied fan curve: slope=%u start_pwm=%u start_temp=%u full_temp=%u\n",
		curve[CURVE_SLOPE], curve[CURVE_START_PWM],
		curve[CURVE_START_TEMP], curve[CURVE_FULL_TEMP]);
	return 0;
}

/*
 * Apply the modprobe.d curve at probe/boot. All-or-nothing: partial or
 * invalid parameter sets are ignored with a warning naming the reason,
 * and probe still succeeds. Requires data->lock.
 */
static const char *const it5570_curve_param_names[EC_CURVE_NREGS] = {
	"curve_slope", "curve_start_pwm", "curve_start_temp",
	"curve_full_temp",
};

static void it5570_apply_param_curve(struct it5570_data *data)
{
	const int params[EC_CURVE_NREGS] = { curve_slope, curve_start_pwm,
					     curve_start_temp,
					     curve_full_temp };
	u8 curve[EC_CURVE_NREGS];
	int i, nset = 0;

	lockdep_assert_held(&data->lock);

	for (i = 0; i < EC_CURVE_NREGS; i++)
		if (params[i] >= 0)
			nset++;
	if (nset == 0)
		return;
	if (nset < EC_CURVE_NREGS) {
		pr_warn(DRIVER_NAME ": ignoring fan-curve parameters: not set:%s%s%s%s\n",
			params[CURVE_SLOPE] < 0 ? " curve_slope" : "",
			params[CURVE_START_PWM] < 0 ? " curve_start_pwm" : "",
			params[CURVE_START_TEMP] < 0 ? " curve_start_temp" : "",
			params[CURVE_FULL_TEMP] < 0 ? " curve_full_temp" : "");
		return;
	}
	for (i = 0; i < EC_CURVE_NREGS; i++) {
		if (params[i] > 255) {
			pr_warn(DRIVER_NAME ": ignoring fan-curve parameters: %s=%d out of range 0-255\n",
				it5570_curve_param_names[i], params[i]);
			return;
		}
		curve[i] = params[i];
	}
	if (!it5570_curve_check(curve, "module parameters"))
		return;
	/*
	 * Need a known-good curve to roll back to - never write blind.
	 * Retry the read once (mirrors the sysfs commit path) in case the
	 * probe-time snapshot hit a transient EC timeout.
	 */
	if (!data->curve_valid && it5570_curve_refresh(data, false)) {
		pr_warn(DRIVER_NAME ": ignoring fan-curve parameters: EC curve read failed\n");
		return;
	}
	if (it5570_curve_apply(data, curve) == 0) {
		memcpy(data->curve_staged, curve, EC_CURVE_NREGS);
		data->curve_written = 0;
	}
}

/*
 * Sysfs curve interface: the four value attrs read/write a staged
 * buffer only; curve_commit=1 validates and applies the whole 4-tuple
 * atomically, curve_commit=0 re-reads the EC (race-free live view,
 * discarding staged edits). Reading curve_commit returns the dirty flag.
 */
static ssize_t curve_val_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int idx = to_sensor_dev_attr(attr)->index;
	u8 val;

	mutex_lock(&data->lock);
	val = data->curve_staged[idx];
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t curve_val_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int idx = to_sensor_dev_attr(attr)->index;
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 255)
		return -EINVAL;

	mutex_lock(&data->lock);
	if (data->shutting_down) {
		mutex_unlock(&data->lock);
		return -ENODEV;
	}
	data->curve_staged[idx] = val;
	data->curve_written |= BIT(idx);
	mutex_unlock(&data->lock);
	return count;
}

static ssize_t curve_commit_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	bool dirty;

	mutex_lock(&data->lock);
	dirty = !data->curve_valid ||
		memcmp(data->curve_staged, data->curve_ec, EC_CURVE_NREGS);
	mutex_unlock(&data->lock);
	return sysfs_emit(buf, "%u\n", dirty);
}

static ssize_t curve_commit_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	unsigned int val;
	int ret;

	ret = kstrtouint(buf, 0, &val);
	if (ret)
		return ret;
	if (val > 1)
		return -EINVAL;

	mutex_lock(&data->lock);
	if (data->shutting_down) {
		ret = -ENODEV;
		goto out;
	}
	if (val == 0) {
		ret = it5570_curve_refresh(data, false);
		goto out;
	}
	/*
	 * Never validate or apply against unknown EC state: a bare commit
	 * over zeroed buffers would e.g. write full_temp=0 (permanent full
	 * speed). Re-read first, keeping the user's staged writes.
	 */
	if (!data->curve_valid) {
		ret = it5570_curve_refresh(data, true);
		if (ret)
			goto out;
	}
	if (!it5570_curve_check(data->curve_staged, "curve_commit")) {
		ret = -EINVAL;
		goto out;
	}
	ret = it5570_curve_apply(data, data->curve_staged);
	if (ret == 0)
		data->curve_written = 0;
out:
	mutex_unlock(&data->lock);
	return ret ? ret : count;
}

static SENSOR_DEVICE_ATTR_RW(curve_slope, curve_val, CURVE_SLOPE);
static SENSOR_DEVICE_ATTR_RW(curve_start_pwm, curve_val, CURVE_START_PWM);
static SENSOR_DEVICE_ATTR_RW(curve_start_temp, curve_val, CURVE_START_TEMP);
static SENSOR_DEVICE_ATTR_RW(curve_full_temp, curve_val, CURVE_FULL_TEMP);
static DEVICE_ATTR_RW(curve_commit);

static struct attribute *it5570_curve_attrs[] = {
	&sensor_dev_attr_curve_slope.dev_attr.attr,
	&sensor_dev_attr_curve_start_pwm.dev_attr.attr,
	&sensor_dev_attr_curve_start_temp.dev_attr.attr,
	&sensor_dev_attr_curve_full_temp.dev_attr.attr,
	&dev_attr_curve_commit.attr,
	NULL
};
ATTRIBUTE_GROUPS(it5570_curve);

/*
 * hwmon interface
 */
static umode_t it5570_is_visible(const void *drvdata,
				 enum hwmon_sensor_types type,
				 u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		return 0444;
	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
		case hwmon_pwm_enable:
			return 0644;
		default:
			return 0;
		}
	case hwmon_temp:
		switch (attr) {
		case hwmon_temp_input:
		case hwmon_temp_label:
			return 0444;
		default:
			return 0;
		}
	default:
		return 0;
	}
}

static int it5570_read(struct device *dev, enum hwmon_sensor_types type,
		       u32 attr, int channel, long *val)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);

	ret = it5570_update(data);
	if (ret)
		goto out;

	switch (type) {
	case hwmon_fan:
		*val = data->fan_rpm;
		break;

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			if (data->fan_mode == EC_FAN_MODE_FULL)
				*val = HWMON_PWM_MAX;
			else if (data->fan_mode == EC_FAN_MODE_OFF)
				*val = 0;	/* stopped fan must not report a speed */
			else
				/* min() guards a corrupt >100 duty in 0x2D */
				*val = min(DIV_ROUND_CLOSEST(data->fan_duty *
							     HWMON_PWM_MAX,
							     EC_DUTY_MAX),
					   (unsigned int)HWMON_PWM_MAX);
			break;
		case hwmon_pwm_enable:
			switch (data->fan_mode) {
			case EC_FAN_MODE_AUTO:
				*val = 2;
				break;
			case EC_FAN_MODE_FULL:
				*val = 0;
				break;
			default:
				*val = 1;
				break;
			}
			break;
		default:
			ret = -EOPNOTSUPP;
			break;
		}
		break;

	case hwmon_temp:
		if (!data->temp_valid) {
			ret = -EIO;	/* never seen a plausible sample */
			break;
		}
		/* hwmon temperatures are in millidegrees C */
		*val = data->cpu_temp * 1000;
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

out:
	mutex_unlock(&data->lock);
	return ret;
}

static int it5570_read_string(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, const char **str)
{
	if (type == hwmon_temp && attr == hwmon_temp_label && channel == 0) {
		*str = "CPU";
		return 0;
	}
	return -EOPNOTSUPP;
}

static int it5570_write(struct device *dev, enum hwmon_sensor_types type,
			u32 attr, int channel, long val)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	unsigned int duty;
	int ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	mutex_lock(&data->lock);

	if (data->shutting_down) {
		mutex_unlock(&data->lock);
		return -ENODEV;
	}

	switch (attr) {
	case hwmon_pwm_input:
		/* Convert hwmon 0-255 to EC percent; never switches modes */
		duty = clamp_val(val, 0, HWMON_PWM_MAX);
		duty = DIV_ROUND_CLOSEST(duty * EC_DUTY_MAX, HWMON_PWM_MAX);
		ret = it5570_write_duty(data, duty);
		break;

	case hwmon_pwm_enable:
		switch (val) {
		case 0:	/* hwmon convention: full speed */
			ret = it5570_set_mode(data, EC_FAN_MODE_FULL);
			break;
		case 1:	/* manual */
			duty = data->fan_duty;
			if (duty < EC_DUTY_MIN)
				duty = EC_DUTY_DEFAULT;
			ret = it5570_set_manual(data, duty);
			break;
		case 2:	/* EC auto curve */
			ret = it5570_set_mode(data, EC_FAN_MODE_AUTO);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		break;

	default:
		ret = -EOPNOTSUPP;
		break;
	}

	mutex_unlock(&data->lock);
	return ret;
}

static const struct hwmon_channel_info * const it5570_info[] = {
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT | HWMON_PWM_ENABLE),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	NULL
};

static const struct hwmon_ops it5570_ops = {
	.is_visible = it5570_is_visible,
	.read = it5570_read,
	.read_string = it5570_read_string,
	.write = it5570_write,
};

static const struct hwmon_chip_info it5570_chip_info = {
	.ops = &it5570_ops,
	.info = it5570_info,
};

/*
 * Platform driver
 */
/*
 * devm-action counterpart of the old it5570_remove(): restores EC auto
 * fan control. Registered on the platform device BEFORE the hwmon device
 * is registered, so devres unwind order guarantees hwmon is torn down
 * (sysfs writers can no longer re-enter manual mode) before this runs.
 * No struct device * is available here, so log via pr_* rather than
 * dev_*, matching the file's existing pr_* style.
 */
static void it5570_restore_auto_action(void *arg)
{
	struct it5570_data *data = arg;
	int ret;

	mutex_lock(&data->lock);
	ret = it5570_set_mode(data, EC_FAN_MODE_AUTO);
	mutex_unlock(&data->lock);
	if (ret)
		pr_warn(DRIVER_NAME ": failed to restore auto fan mode (%d)\n",
			ret);
	else
		pr_info(DRIVER_NAME ": fan control restored to auto mode\n");
}

static int it5570_probe(struct platform_device *pdev)
{
	struct it5570_data *data;
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);

	ret = devm_add_action_or_reset(&pdev->dev, it5570_restore_auto_action,
				       data);
	if (ret)
		return ret;

	/*
	 * Snapshot the EC curve before hwmon registration puts the curve
	 * sysfs attrs live. Failure is non-fatal: curve_valid stays false
	 * and curve_commit re-reads on first use.
	 */
	mutex_lock(&data->lock);
	ret = it5570_curve_refresh(data, false);
	mutex_unlock(&data->lock);
	if (ret)
		dev_warn(&pdev->dev, "initial curve read failed (%d)\n", ret);

	hwmon_dev = devm_hwmon_device_register_with_info(&pdev->dev,
							 DRIVER_NAME, data,
							 &it5570_chip_info,
							 it5570_curve_groups);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	platform_set_drvdata(pdev, data);

	/* Initial read; hwmon sysfs is already live, so snapshot under lock */
	mutex_lock(&data->lock);
	ret = it5570_update(data);
	if (ret == 0 && data->temp_valid)
		dev_info(&pdev->dev,
			 "CPU: %u°C, fan: %u RPM (%u%% duty, mode %u)\n",
			 data->cpu_temp, data->fan_rpm, data->fan_duty,
			 data->fan_mode);
	mutex_unlock(&data->lock);
	if (ret)
		dev_warn(&pdev->dev, "initial EC read failed (%d)\n", ret);

	/*
	 * hwmon sysfs is already live: a staged curve_* write landing in
	 * this window is overwritten by a successful param apply, which
	 * syncs curve_staged to the applied curve. Acceptable - the params
	 * are the boot-time intent.
	 */
	mutex_lock(&data->lock);
	it5570_apply_param_curve(data);
	mutex_unlock(&data->lock);

	return 0;
}

/*
 * A warm reboot does not reset the EC, so manual mode (and the last
 * commanded duty) would otherwise survive into BIOS and the next boot
 * with no host thermal management running. Force the EC back to its
 * auto curve directly - cache state is meaningless once we're tearing
 * down for reboot/poweroff, and a failure here must not abort shutdown.
 */
static void it5570_shutdown(struct platform_device *pdev)
{
	struct it5570_data *data = platform_get_drvdata(pdev);
	int ret;

	mutex_lock(&data->lock);
	data->shutting_down = true;
	ret = ec_write_byte(EC_REG_FAN_MODE, EC_FAN_MODE_AUTO);
	mutex_unlock(&data->lock);
	if (ret)
		dev_warn(&pdev->dev,
			 "failed to restore auto fan mode on shutdown (%d)\n",
			 ret);
}

static int it5570_suspend(struct device *dev)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int ret;

	mutex_lock(&data->lock);
	/*
	 * Hand the fan to the EC auto curve for the transition. Written
	 * directly (not via it5570_set_mode) so the cached fan_mode and
	 * fan_duty survive for resume to re-apply.
	 */
	ret = ec_write_byte(EC_REG_FAN_MODE, EC_FAN_MODE_AUTO);
	mutex_unlock(&data->lock);
	if (ret)
		dev_warn(dev, "failed to set auto fan mode on suspend (%d)\n",
			 ret);
	/*
	 * Deliberate exception to the "failed write invalidates the cache"
	 * rule: a failed write here means the EC still matches the cache,
	 * and resume invalidates unconditionally anyway.
	 */
	return 0;	/* never abort system suspend over an EC timeout */
}

static int it5570_resume(struct device *dev)
{
	struct it5570_data *data = dev_get_drvdata(dev);
	int ret = 0;
	int curve_ret = 0;

	mutex_lock(&data->lock);
	if (data->fan_mode == EC_FAN_MODE_MANUAL)
		ret = it5570_set_manual(data, data->fan_duty);
	else if (data->fan_mode == EC_FAN_MODE_FULL)
		ret = it5570_set_mode(data, EC_FAN_MODE_FULL);
	/*
	 * The BIOS rewrites the curve at boot; whether S3 exit does too is
	 * unproven, so re-apply the last curve known to be in the EC (the
	 * modprobe.d one unless live-tuned since). Skipped if no curve was
	 * ever successfully read or applied.
	 */
	if (data->curve_valid) {
		u8 curve[EC_CURVE_NREGS];

		memcpy(curve, data->curve_ec, sizeof(curve));
		curve_ret = it5570_curve_apply(data, curve);
	}
	/* jiffies froze across suspend; force a fresh read next access */
	data->valid = false;
	mutex_unlock(&data->lock);
	if (ret)
		dev_warn(dev, "failed to restore fan state on resume (%d)\n",
			 ret);
	if (curve_ret)
		dev_warn(dev, "failed to restore fan curve on resume (%d)\n",
			 curve_ret);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(it5570_pm_ops, it5570_suspend, it5570_resume);

static struct platform_driver it5570_driver = {
	.driver = {
		.name = DRIVER_NAME,
		.pm = pm_sleep_ptr(&it5570_pm_ops),
	},
	.probe = it5570_probe,
	.shutdown = it5570_shutdown,
};

/*
 * The IT5570 chip ID alone is not sufficient identification: the upstream
 * AMD mini-PC boards carry the same EC with entirely different firmware
 * register assignments (their fan-RPM low byte is our mode register).
 * This driver's register map is Sigma-only, so refuse anything else.
 */
static const struct dmi_system_id it5570_dmi_table[] = {
	{
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "LattePanda"),
			DMI_MATCH(DMI_PRODUCT_NAME, "LattePanda Sigma"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, it5570_dmi_table);

static int __init it5570_init(void)
{
	int ret;

	if (!dmi_check_system(it5570_dmi_table)) {
		pr_info(DRIVER_NAME ": not a LattePanda Sigma, aborting\n");
		return -ENODEV;
	}

	ret = it5570_detect();
	if (ret)
		return ret;

	/*
	 * Hold the EC ports for the driver's lifetime so no other
	 * port-banging driver can interleave with our multi-step EC
	 * transactions. The ACPI EC driver never binds here (_STA=0),
	 * so the ports are unclaimed. Not adjacent - two regions.
	 */
	if (!request_region(EC_DATA, 1, DRIVER_NAME)) {
		pr_err(DRIVER_NAME ": EC data port 0x%02x busy\n", EC_DATA);
		return -EBUSY;
	}
	if (!request_region(EC_SC, 1, DRIVER_NAME)) {
		pr_err(DRIVER_NAME ": EC command port 0x%02x busy\n", EC_SC);
		ret = -EBUSY;
		goto err_release_data;
	}

	ret = platform_driver_register(&it5570_driver);
	if (ret)
		goto err_release_sc;

	it5570_pdev = platform_device_register_simple(DRIVER_NAME, -1,
						      NULL, 0);
	if (IS_ERR(it5570_pdev)) {
		ret = PTR_ERR(it5570_pdev);
		goto err_driver;
	}

	return 0;

err_driver:
	platform_driver_unregister(&it5570_driver);
err_release_sc:
	release_region(EC_SC, 1);
err_release_data:
	release_region(EC_DATA, 1);
	return ret;
}

static void __exit it5570_exit(void)
{
	platform_device_unregister(it5570_pdev);
	platform_driver_unregister(&it5570_driver);
	release_region(EC_SC, 1);
	release_region(EC_DATA, 1);
}

module_init(it5570_init);
module_exit(it5570_exit);

MODULE_AUTHOR("Michael");
MODULE_AUTHOR("solderdev");
MODULE_DESCRIPTION("LattePanda Sigma fan control driver (ITE IT5570 EC)");
MODULE_LICENSE("GPL");
