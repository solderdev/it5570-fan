// SPDX-License-Identifier: GPL-2.0
/*
 * LattePanda Sigma fan control driver (ITE IT5570 EC)
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
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/jiffies.h>
#include <linux/platform_device.h>
#include <linux/dmi.h>

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

/* Fan mode register values */
#define EC_FAN_MODE_OFF		0	/* comparison only — NEVER written */
#define EC_FAN_MODE_MANUAL	1
#define EC_FAN_MODE_AUTO	2
#define EC_FAN_MODE_FULL	3

/* PWM conversion: hwmon uses 0-255, EC uses percent */
#define EC_DUTY_MIN		10	/* safety floor for manual duty */
#define EC_DUTY_MAX		100
#define EC_DUTY_DEFAULT		50	/* used when no sane cached duty exists */
#define HWMON_PWM_MAX		255

struct it5570_data {
	struct mutex lock;
	unsigned long last_updated;
	bool valid;

	/* Cached sensor values */
	unsigned int fan_rpm;
	unsigned int fan_duty;	/* 0-100, cached 0x2D */
	unsigned int fan_mode;	/* cached 0x23 */
	unsigned int cpu_temp;	/* °C */
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
static int ec_wait_ibf_clear(void)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (!(inb(EC_SC) & EC_SC_IBF))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int ec_wait_obf_set(void)
{
	int i;

	for (i = 0; i < 10000; i++) {
		if (inb(EC_SC) & EC_SC_OBF)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
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
 * Update cached sensor data (rate-limited to 1 Hz)
 */
static int it5570_update(struct it5570_data *data)
{
	u8 hi, lo, duty, mode, temp;
	int ret;

	mutex_lock(&data->lock);

	if (data->valid && time_before(jiffies, data->last_updated + HZ))
		goto out;

	ret = ec_read_byte(EC_REG_FAN_RPM_HI, &hi);
	if (ret)
		goto err;
	ret = ec_read_byte(EC_REG_FAN_RPM_LO, &lo);
	if (ret)
		goto err;
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
	data->cpu_temp = temp;

	if (mode < EC_FAN_MODE_MANUAL || mode > EC_FAN_MODE_FULL)
		pr_warn_ratelimited(DRIVER_NAME ": unexpected fan mode %u\n",
				    mode);

	data->last_updated = jiffies;
	data->valid = true;

out:
	mutex_unlock(&data->lock);
	return 0;

err:
	data->valid = false;
	mutex_unlock(&data->lock);
	return ret;
}

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

	ret = it5570_update(data);
	if (ret)
		return ret;

	switch (type) {
	case hwmon_fan:
		*val = data->fan_rpm;
		return 0;

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
			return 0;
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
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	case hwmon_temp:
		/* hwmon temperatures are in millidegrees C */
		*val = data->cpu_temp * 1000;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
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
	/* Transitional stub: write paths land in the next commit */
	return -EOPNOTSUPP;
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
static int it5570_probe(struct platform_device *pdev)
{
	struct it5570_data *data;
	struct device *hwmon_dev;
	int ret;

	data = devm_kzalloc(&pdev->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	mutex_init(&data->lock);

	hwmon_dev = devm_hwmon_device_register_with_info(
		&pdev->dev, DRIVER_NAME, data,
		&it5570_chip_info, NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	platform_set_drvdata(pdev, data);

	/* Initial read */
	ret = it5570_update(data);
	if (ret)
		dev_warn(&pdev->dev, "initial EC read failed (%d)\n", ret);
	else
		dev_info(&pdev->dev,
			 "CPU: %u°C, fan: %u RPM (%u%% duty, mode %u)\n",
			 data->cpu_temp, data->fan_rpm, data->fan_duty,
			 data->fan_mode);

	return 0;
}

static void it5570_remove(struct platform_device *pdev)
{
	/* Restore EC auto fan control on unload */
	if (ec_write_byte(EC_REG_FAN_MODE, EC_FAN_MODE_AUTO))
		dev_warn(&pdev->dev, "failed to restore auto fan mode\n");
	else
		dev_info(&pdev->dev, "fan control restored to auto mode\n");
}

static struct platform_driver it5570_driver = {
	.driver = {
		.name = DRIVER_NAME,
	},
	.probe = it5570_probe,
	.remove = it5570_remove,
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

	ret = platform_driver_register(&it5570_driver);
	if (ret)
		return ret;

	it5570_pdev = platform_device_register_simple(DRIVER_NAME, -1,
						       NULL, 0);
	if (IS_ERR(it5570_pdev)) {
		ret = PTR_ERR(it5570_pdev);
		platform_driver_unregister(&it5570_driver);
		return ret;
	}

	return 0;
}

static void __exit it5570_exit(void)
{
	platform_device_unregister(it5570_pdev);
	platform_driver_unregister(&it5570_driver);
}

module_init(it5570_init);
module_exit(it5570_exit);

MODULE_AUTHOR("Michael");
MODULE_DESCRIPTION("ITE IT5570 EC Fan Control Driver");
MODULE_LICENSE("GPL");
