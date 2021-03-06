/*
 * leds_an30259a.c - driver for panasonic AN30259A led control chip
 *
 * Copyright (C) 2012, Samsung Electronics Co. Ltd. All Rights Reserved.
 *
 * Contact: Kamaldeep Singla <kamal.singla@samsung.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2 of the License.
 *
 */
/* Extended sysfs interface to allow for full control of LED operations
 *
 * Extension Author: Jean-Pierre Rasquin <yank555.lu@gmail.com>
 * Further extended by andip71, 23.01.2014
 *
 * SysFS interface :
 * -----------------
 *
 * /sys/class/sec/led/led_fade (rw)
 *
 *   0 : blink (Samsung style)
 *   1 : fade (CyanogenMod style)
 *
 * /sys/class/sec/led/led_intensity (rw)
 *
 *        0 : stock CM behaviour
 *    1- 39 : darker than Samsung stock
 *       40 : stock Samsung behaviour
 *   41-255 : brighter than Samsung stock
 *
 *    NB: Low power mode respected, applied brightness is divided by 0x8, except in CM mode, where it's never applied
 *
 * /sys/class/sec/led/led_speed (rw)
 *
 *   0 : continuous light
 *   1 : normal rate
 *   2 - 60: faster rate
 *
 * /sys/class/sec/led/led_slope (rw)
 *
 *   takes 4 parameters, each between 0 and 5 (steps of 4ms) for :
 *
 *      slope up operation 1
 *      slope up operation 2
 *      slope down operation 1
 *      slope down operation 2
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/leds.h>
#include <linux/leds-an30259a.h>
#include <linux/workqueue.h>
#include <linux/wakelock.h>
#include <linux/gpio.h>

/* AN30259A register map */
#define AN30259A_REG_SRESET		0x00
#define AN30259A_REG_LEDON		0x01
#define AN30259A_REG_SEL		0x02

#define AN30259A_REG_LED1CC		0x03
#define AN30259A_REG_LED2CC		0x04
#define AN30259A_REG_LED3CC		0x05

#define AN30259A_REG_LED1SLP		0x06
#define AN30259A_REG_LED2SLP		0x07
#define AN30259A_REG_LED3SLP		0x08

#define AN30259A_REG_LED1CNT1		0x09
#define AN30259A_REG_LED1CNT2		0x0a
#define AN30259A_REG_LED1CNT3		0x0b
#define AN30259A_REG_LED1CNT4		0x0c

#define AN30259A_REG_LED2CNT1		0x0d
#define AN30259A_REG_LED2CNT2		0x0e
#define AN30259A_REG_LED2CNT3		0x0f
#define AN30259A_REG_LED2CNT4		0x10

#define AN30259A_REG_LED3CNT1		0x11
#define AN30259A_REG_LED3CNT2		0x12
#define AN30259A_REG_LED3CNT3		0x13
#define AN30259A_REG_LED3CNT4		0x14
#define AN30259A_REG_MAX		0x15
/* MASK */
#define AN30259A_MASK_IMAX		0xc0
#define AN30259A_MASK_DELAY		0xf0
#define AN30259A_SRESET			0x01
#define LED_SLOPE_MODE			0x10
#define LED_ON				0x01

#define DUTYMAX_MAX_VALUE		0x7f
#define DUTYMIN_MIN_VALUE		0x00
#define SLPTT_MAX_VALUE			7500

#define AN30259A_TIME_UNIT		500

#define LED_R_MASK			0x00ff0000
#define LED_G_MASK			0x0000ff00
#define LED_B_MASK			0x000000ff
#define LED_R_SHIFT			16
#define LED_G_SHIFT			8
#define LED_IMAX_SHIFT			6
#define AN30259A_CTN_RW_FLG		0x80

#define LED_MAX_CURRENT		0xFF
#define LED_OFF				0x00

#define	MAX_NUM_LEDS	3

#define MIN(a, b) ((a) < (b) ? (a) : (b))

u8 LED_DYNAMIC_CURRENT = 0x28;
u8 LED_LOWPOWER_MODE = 0x0;

u32 LED_R_CURRENT = 0x28;
u32 LED_G_CURRENT = 0x28;
u32 LED_B_CURRENT = 0x28;

u32 led_default_cur = 0x28;
u32 led_lowpower_cur = 0x05;

unsigned long disabled_samsung_pattern = 0;

u32 led_offset[MAX_NUM_LEDS] = {0,};

static struct an30259_led_conf led_conf[] = {
	{
		.name = "led_r",
		.brightness = LED_OFF,
		.max_brightness = 0,
		.flags = 0,
	}, {
		.name = "led_g",
		.brightness = LED_OFF,
		.max_brightness = 0,
		.flags = 0,
	}, {
		.name = "led_b",
		.brightness = LED_OFF,
		.max_brightness = 0,
		.flags = 0,
	}
};

enum an30259a_led_enum {
	LED_R,
	LED_G,
	LED_B,
};

enum an30259a_pattern {
	PATTERN_OFF,
	CHARGING,
	CHARGING_ERR,
	MISSED_NOTI,
	LOW_BATTERY,
	FULLY_CHARGED,
	POWERING,
};

struct an30259a_led {
	u8	channel;
	u8	brightness;
	struct led_classdev	cdev;
	struct work_struct	brightness_work;
	unsigned long delay_on_time_ms;
	unsigned long delay_off_time_ms;
};

struct an30259a_data {
	struct	i2c_client	*client;
	struct	mutex	mutex;
	struct	an30259a_led	leds[MAX_NUM_LEDS];
	u8		shadow_reg[AN30259A_REG_MAX];
};

struct i2c_client *b_client;

#define SEC_LED_SPECIFIC
#define LED_DEEP_DEBUG

#ifdef SEC_LED_SPECIFIC
struct device *led_dev;
int led_enable_fade;
u8 led_intensity;
int led_speed;
int led_slope_up_1;
int led_slope_up_2;
int led_slope_down_1;
int led_slope_down_2;
/*path : /sys/class/sec/led/led_pattern*/
/*path : /sys/class/sec/led/led_blink*/
/*path : /sys/class/sec/led/led_fade*/
/*path : /sys/class/sec/led/led_intensity*/
/*path : /sys/class/sec/led/led_speed*/
/*path : /sys/class/sec/led/led_slope*/
/*path : /sys/class/sec/led/disable_samsung_pattern*/
/*path : /sys/class/leds/led_r/brightness*/
/*path : /sys/class/leds/led_g/brightness*/
/*path : /sys/class/leds/led_b/brightness*/
#endif

#if defined (CONFIG_SEC_FACTORY)
#if defined (CONFIG_SEC_S_PROJECT)
static int f_jig_cable;
extern int get_lcd_attached(void);

static int __init get_jig_cable_cmdline(char *mode)
{
	f_jig_cable = mode[0]-48;
	return 0;
}

__setup( "uart_dbg=", get_jig_cable_cmdline);
#endif
#endif

static void leds_on(enum an30259a_led_enum led, bool on, bool slopemode,
					u8 ledcc);

static inline struct an30259a_led *cdev_to_led(struct led_classdev *cdev)
{
	return container_of(cdev, struct an30259a_led, cdev);
}

#ifdef LED_DEEP_DEBUG
static void an30259a_debug(struct i2c_client *client)
{
	struct an30259a_data *data = i2c_get_clientdata(client);
	int ret;
	u8 buff[21] = {0,};
	ret = i2c_smbus_read_i2c_block_data(client,
		AN30259A_REG_SRESET|AN30259A_CTN_RW_FLG,
		sizeof(buff), buff);
	if (ret != sizeof(buff)) {
		dev_err(&data->client->dev,
			"%s: failure on i2c_smbus_read_i2c_block_data\n",
			__func__);
	}
	print_hex_dump(KERN_ERR, "an30259a: ",
		DUMP_PREFIX_OFFSET, 32, 1, buff,
		sizeof(buff), false);
}
#endif

static int leds_i2c_write_all(struct i2c_client *client)
{
	struct an30259a_data *data = i2c_get_clientdata(client);
	int ret;

	/*we need to set all the configs setting first, then LEDON later*/
	mutex_lock(&data->mutex);
	ret = i2c_smbus_write_i2c_block_data(client,
			AN30259A_REG_SEL | AN30259A_CTN_RW_FLG,
			AN30259A_REG_MAX - AN30259A_REG_SEL,
			&data->shadow_reg[AN30259A_REG_SEL]);
	if (ret < 0) {
		dev_err(&client->adapter->dev,
			"%s: failure on i2c block write\n",
			__func__);
		goto exit;
	}
	ret = i2c_smbus_write_byte_data(client, AN30259A_REG_LEDON,
					data->shadow_reg[AN30259A_REG_LEDON]);
	if (ret < 0) {
		dev_err(&client->adapter->dev,
			"%s: failure on i2c byte write\n",
			__func__);
		goto exit;
	}
	mutex_unlock(&data->mutex);
	return 0;

exit:
	mutex_unlock(&data->mutex);
	return ret;
}

void an30259a_set_brightness(struct led_classdev *cdev,
			enum led_brightness brightness)
{
		struct an30259a_led *led = cdev_to_led(cdev);
		led->brightness = (u8)brightness;
		schedule_work(&led->brightness_work);
}

static void an30259a_led_brightness_work(struct work_struct *work)
{
		struct i2c_client *client = b_client;
		struct an30259a_led *led = container_of(work,
				struct an30259a_led, brightness_work);
		leds_on(led->channel, true, false, led->brightness);
		leds_i2c_write_all(client);
}

/*
 * leds_set_slope_mode() sets correct values to corresponding shadow registers.
 * led: stands for LED_R or LED_G or LED_B.
 * delay: represents for starting delay time in multiple of .5 second.
 * dutymax: led at slope lighting maximum PWM Duty setting.
 * dutymid: led at slope lighting middle PWM Duty setting.
 * dutymin: led at slope lighting minimum PWM Duty Setting.
 * slptt1: total time of slope operation 1 and 2, in multiple of .5 second.
 * slptt2: total time of slope operation 3 and 4, in multiple of .5 second.
 * dt1: detention time at each step in slope operation 1, in multiple of 4ms.
 * dt2: detention time at each step in slope operation 2, in multiple of 4ms.
 * dt3: detention time at each step in slope operation 3, in multiple of 4ms.
 * dt4: detention time at each step in slope operation 4, in multiple of 4ms.
 */
static void leds_set_slope_mode(struct i2c_client *client,
				enum an30259a_led_enum led, u8 delay,
				u8 dutymax, u8 dutymid, u8 dutymin,
				u8 slptt1, u8 slptt2,
				u8 dt1, u8 dt2, u8 dt3, u8 dt4)
{

	struct an30259a_data *data = i2c_get_clientdata(client);

	data->shadow_reg[AN30259A_REG_LED1CNT1 + led * 4] =
							dutymax << 4 | dutymid;
	data->shadow_reg[AN30259A_REG_LED1CNT2 + led * 4] =
							delay << 4 | dutymin;
	data->shadow_reg[AN30259A_REG_LED1CNT3 + led * 4] = dt2 << 4 | dt1;
	data->shadow_reg[AN30259A_REG_LED1CNT4 + led * 4] = dt4 << 4 | dt3;
	data->shadow_reg[AN30259A_REG_LED1SLP + led] = slptt2 << 4 | slptt1;
}

static void leds_on(enum an30259a_led_enum led, bool on, bool slopemode,
			u8 ledcc)
{
	struct an30259a_data *data = i2c_get_clientdata(b_client);

	if (ledcc > 0)
		ledcc += led_offset[led];

	if (on)
		data->shadow_reg[AN30259A_REG_LEDON] |= LED_ON << led;
	else {
		data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_ON << led);
		data->shadow_reg[AN30259A_REG_LED1CNT2 + led * 4] &=
							~AN30259A_MASK_DELAY;
	}
	if ((slopemode) && (led_speed != 0))
		data->shadow_reg[AN30259A_REG_LEDON] |= LED_SLOPE_MODE << led;
	else
		data->shadow_reg[AN30259A_REG_LEDON] &=
						~(LED_SLOPE_MODE << led);

	data->shadow_reg[AN30259A_REG_LED1CC + led] = ledcc;
}

static int leds_set_imax(struct i2c_client *client, u8 imax)
{
	int ret;
	struct an30259a_data *data = i2c_get_clientdata(client);

	data->shadow_reg[AN30259A_REG_SEL] &= ~AN30259A_MASK_IMAX;
	data->shadow_reg[AN30259A_REG_SEL] |= imax << LED_IMAX_SHIFT;

	ret = i2c_smbus_write_byte_data(client, AN30259A_REG_SEL,
			data->shadow_reg[AN30259A_REG_SEL]);
	if (ret < 0) {
		dev_err(&client->adapter->dev,
			"%s: failure on i2c write\n",
			__func__);
	}
	return 0;
}

#ifdef SEC_LED_SPECIFIC
static void an30259a_reset_register_work(struct work_struct *work)
{
	int retval;
	struct i2c_client *client;
	client = b_client;

	leds_on(LED_R, false, false, 0);
	leds_on(LED_G, false, false, 0);
	leds_on(LED_B, false, false, 0);

	retval = leds_i2c_write_all(client);
	if (retval)
		printk(KERN_WARNING "leds_i2c_write_all failed\n");
}

static void an30259a_start_led_pattern(int mode)
{
	int retval;
	u8 r_brightness;          /* Yank555.lu : Control LED intensity (normal, bright) */
	u8 g_brightness;
	u8 b_brightness;
	struct i2c_client *client;
	struct work_struct *reset = 0;
	client = b_client;

	if (mode > POWERING)
		return;

	if(disabled_samsung_pattern) {
		return;
	}

	/* Set all LEDs Off */
	an30259a_reset_register_work(reset);
	if (mode == LED_OFF)
		return;

	/* Set to low power consumption mode */
	if (LED_LOWPOWER_MODE == 1)
		LED_DYNAMIC_CURRENT = (u8)led_lowpower_cur;
	else
		LED_DYNAMIC_CURRENT = (u8)led_default_cur;

	/* Yank555.lu : Control LED intensity (normal, bright) */
	if (led_intensity == 0) {
		r_brightness = LED_R_CURRENT; /* CM stock behaviour */
		g_brightness = LED_G_CURRENT;
		b_brightness = LED_B_CURRENT;
	} else {
		r_brightness = MIN(led_intensity, LED_MAX_CURRENT);
		g_brightness = MIN(led_intensity, LED_MAX_CURRENT);
		b_brightness = MIN(led_intensity, LED_MAX_CURRENT);
	}

	switch (mode) {
	/* leds_set_slope_mode(client, LED_SEL, DELAY,  MAX, MID, MIN,
		SLPTT1, SLPTT2, DT1, DT2, DT3, DT4) */
	case CHARGING:
		pr_info("LED Battery Charging Pattern on\n");
		leds_on(LED_R, true, false, r_brightness);
		break;

	case CHARGING_ERR:
		pr_info("LED Battery Charging error Pattern on\n");
		leds_on(LED_R, true, true, r_brightness);
		/* Yank555.lu : Handle fading / blinking */
		if (led_enable_fade == 1) {
			leds_set_slope_mode(client, LED_R,
					1, (15 / led_speed),  (7 / led_speed), 0, 1, 1, led_slope_up_1, led_slope_up_2, led_slope_down_1, led_slope_down_2);
		} else {
			leds_set_slope_mode(client, LED_R,
					1, (15 / led_speed), (15 / led_speed), 0, 1, 1, 0, 0, 0, 0);
		}
		break;

	case MISSED_NOTI:
		pr_info("LED Missed Notifications Pattern on\n");
		leds_on(LED_B, true, true, b_brightness);
		/* Yank555.lu : Handle fading / blinking */
		if (led_enable_fade == 1) {
			leds_set_slope_mode(client, LED_B,
						10, (15 / led_speed),  (7 / led_speed), 0, 1, (10 / led_speed), led_slope_up_1, led_slope_up_2, led_slope_down_1, led_slope_down_2);
		} else {
			leds_set_slope_mode(client, LED_B,
						10, (15 / led_speed), (15 / led_speed), 0, 1, (10 / led_speed), 0, 0, 0, 0);
		}
		break;
	case LOW_BATTERY:
		pr_info("LED Low Battery Pattern on\n");
		leds_on(LED_R, true, true, r_brightness);
		/* Yank555.lu : Handle fading / blinking */
		if (led_enable_fade == 1) {
			leds_set_slope_mode(client, LED_R,
						10, (15 / led_speed),  (7 / led_speed), 0, 1, (10 / led_speed), led_slope_up_1, led_slope_up_2, led_slope_down_1, led_slope_down_2);
		} else {
			leds_set_slope_mode(client, LED_R,
						10, (15 / led_speed), (15 / led_speed), 0, 1, (10 / led_speed), 0, 0, 0, 0);
		}
		break;

	case FULLY_CHARGED:
		pr_info("LED full Charged battery Pattern on\n");
		leds_on(LED_G, true, false, g_brightness);
		break;

	case POWERING:
		pr_info("LED Powering Pattern on\n");
		leds_on(LED_B, true, true, LED_DYNAMIC_CURRENT);
		leds_set_slope_mode(client, LED_B,
				0, 15, 12, 8, 2, 2, 3, 3, 3, 3);
		break;

	default:
		return;
		break;
	}
	retval = leds_i2c_write_all(client);
	if (retval)
		printk(KERN_WARNING "leds_i2c_write_all failed\n");
}

static void an30259a_set_led_blink(enum an30259a_led_enum led,
					unsigned int delay_on_time,
					unsigned int delay_off_time,
					u8 brightness)
{
	struct i2c_client *client;
	client = b_client;

	if (brightness == LED_OFF) {
		leds_on(led, false, false, brightness);
		return;
	}

	if (brightness > LED_MAX_CURRENT)
		brightness = LED_MAX_CURRENT;

	if (led == LED_R)
		LED_DYNAMIC_CURRENT = LED_R_CURRENT;
	else if (led == LED_G)
		LED_DYNAMIC_CURRENT = LED_G_CURRENT;
	else if (led == LED_B)
		LED_DYNAMIC_CURRENT = LED_B_CURRENT;

	/* Yank555.lu : Control LED intensity (CM, Samsung, override) */
	if (led_intensity == 40) /* Samsung stock behaviour */
		brightness = (brightness * LED_DYNAMIC_CURRENT) / LED_MAX_CURRENT;
	else if (led_intensity != 0) /* CM stock behaviour */
		brightness = (brightness * led_intensity) / LED_MAX_CURRENT; /* override, darker or brighter */

	if (delay_on_time > SLPTT_MAX_VALUE)
		delay_on_time = SLPTT_MAX_VALUE;

	if (delay_off_time > SLPTT_MAX_VALUE)
		delay_off_time = SLPTT_MAX_VALUE;

	if (delay_off_time == LED_OFF) {
		leds_on(led, true, false, brightness);
		if (brightness == LED_OFF)
			leds_on(led, false, false, brightness);
		return;
	} else
		leds_on(led, true, true, brightness);

	/* Yank555.lu : Handle fading / blinking */
	if (led_enable_fade == 1) {
		leds_set_slope_mode(client, led, 0, (15 / led_speed), (7 / led_speed), 0,
					((delay_on_time / led_speed) + AN30259A_TIME_UNIT - 1) /
					AN30259A_TIME_UNIT,
					((delay_off_time / led_speed) + AN30259A_TIME_UNIT - 1) /
					AN30259A_TIME_UNIT,
					led_slope_up_1, led_slope_up_2, led_slope_down_1, led_slope_down_2);
	} else {
		leds_set_slope_mode(client, led, 0, (15 / led_speed), (15 / led_speed), 0,
					((delay_on_time / led_speed) + AN30259A_TIME_UNIT - 1) /
					AN30259A_TIME_UNIT,
					((delay_off_time / led_speed) + AN30259A_TIME_UNIT - 1) /
					AN30259A_TIME_UNIT,
					0, 0, 0, 0);
	}
}

static ssize_t store_an30259a_led_lowpower(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int retval;
	u8 led_lowpower;
	struct an30259a_data *data = dev_get_drvdata(dev);

	retval = kstrtou8(buf, 0, &led_lowpower);
	if (retval != 0) {
		dev_err(&data->client->dev, "fail to get led_lowpower.\n");
		return count;
	}

	LED_LOWPOWER_MODE = led_lowpower;

	printk(KERN_DEBUG "led_lowpower mode set to %i\n", led_lowpower);

	return count;
}

static ssize_t store_an30259a_led_br_lev(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int retval;
	unsigned long brightness_lev;
	struct i2c_client *client;
	struct an30259a_data *data = dev_get_drvdata(dev);
	client = b_client;

	retval = kstrtoul(buf, 16, &brightness_lev);
	if (retval != 0) {
		dev_err(&data->client->dev, "fail to get led_br_lev.\n");
		return count;
	}

	leds_set_imax(client, brightness_lev);

	return count;
}

static ssize_t store_an30259a_led_pattern(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int retval;
	unsigned int mode = 0;
	unsigned int type = 0;
	struct an30259a_data *data = dev_get_drvdata(dev);

	retval = sscanf(buf, "%d %d", &mode, &type);

	if (retval == 0) {
		dev_err(&data->client->dev, "fail to get led_pattern mode.\n");
		return count;
	}

	an30259a_start_led_pattern(mode);
	printk(KERN_DEBUG "led pattern : %d is activated\n", mode);

	return count;
}

static ssize_t store_an30259a_led_blink(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int retval;
	unsigned int led_brightness = 0;
	unsigned int delay_on_time = 0;
	unsigned int delay_off_time = 0;
	struct an30259a_data *data = dev_get_drvdata(dev);
	u8 led_r_brightness = 0;
	u8 led_g_brightness = 0;
	u8 led_b_brightness = 0;
	struct work_struct *reset = 0;

	retval = sscanf(buf, "0x%x %d %d", &led_brightness,
				&delay_on_time, &delay_off_time);

	if (retval == 0) {
		dev_err(&data->client->dev, "fail to get led_blink value.\n");
		return count;
	}
	/*Reset an30259a*/
	an30259a_reset_register_work(reset);

	/*Set LED blink mode*/
	led_r_brightness = ((u32)led_brightness & LED_R_MASK)
					>> LED_R_SHIFT;
	led_g_brightness = ((u32)led_brightness & LED_G_MASK)
					>> LED_G_SHIFT;
	led_b_brightness = ((u32)led_brightness & LED_B_MASK);

	an30259a_set_led_blink(LED_R, delay_on_time,
				delay_off_time, led_r_brightness);
	an30259a_set_led_blink(LED_G, delay_on_time,
				delay_off_time, led_g_brightness);
	an30259a_set_led_blink(LED_B, delay_on_time,
				delay_off_time, led_b_brightness);

	leds_i2c_write_all(data->client);

	printk(KERN_DEBUG "led_blink is called, Color:0x%X Brightness:%i\n",
			led_brightness, LED_DYNAMIC_CURRENT);

	return count;
}

static ssize_t show_an30259a_led_fade(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
	switch(led_enable_fade) {
		case 0:		return sprintf(buf, "%d - LED fading is disabled\n", led_enable_fade);
		case 1:		return sprintf(buf, "%d - LED fading is enabled\n", led_enable_fade);
		default:	return sprintf(buf, "%d - LED fading is in undefined status\n", led_enable_fade);
	}
}

static ssize_t store_an30259a_led_fade(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int enabled = -1; /* default to not set a new value */

	sscanf(buf, "%d", &enabled);

	switch(enabled) { /* Accept only if 0 or 1 */
		case 0:
		case 1:		led_enable_fade = enabled;
		default:	return count;
	}
}

static ssize_t show_an30259a_led_intensity(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
	switch(led_intensity) {
		case  0:	return sprintf(buf, "%d - CM stock LED intensity\n", led_intensity);
		case 40:	return sprintf(buf, "%d - Samsung stock LED intensity\n", led_intensity);
		default:	if (led_intensity < 40) 
					return sprintf(buf, "%d - LED intesity darker by %d steps\n", led_intensity, 40-led_intensity);
				else
					return sprintf(buf, "%d - LED intesity brighter by %d steps\n", led_intensity, led_intensity-40);
	}
}

static ssize_t store_an30259a_led_intensity(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int new_intensity = -1; /* default to not set a new value */

	sscanf(buf, "%d", &new_intensity);

	/* Only values between 0 and 255 are accepted */
	if (new_intensity >= 0 && new_intensity <= 255)

		led_intensity = (u8)new_intensity;

	return count;

}

static ssize_t show_an30259a_led_speed(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%d - LED blinking/fading speed\n", led_speed);
}

static ssize_t store_an30259a_led_speed(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int new_led_speed = -1; /* default to not set a new value */

	sscanf(buf, "%d", &new_led_speed);

	// only accept if between 0 and 15
	if ((new_led_speed >= 0) && (new_led_speed <= 15))
		led_speed = new_led_speed;
		
	return count;
}

static ssize_t show_an30259a_led_slope(struct device *dev,
                    struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "Slope up : (%d,%d) - Slope down (%d,%d)\n", led_slope_up_1, led_slope_up_2, led_slope_down_1, led_slope_down_2);
}

static ssize_t store_an30259a_led_slope(struct device *dev,
					struct device_attribute *devattr,
					const char *buf, size_t count)
{
	int new_led_slope_up_1;
	int new_led_slope_up_2;
	int new_led_slope_down_1;
	int new_led_slope_down_2;
	int retval;

	retval = sscanf(buf, "%d %d %d %d", &new_led_slope_up_1, &new_led_slope_up_2, &new_led_slope_down_1, &new_led_slope_down_2);

	if (retval) {
		/* allow only values between 0 and 5 (steps of 4ms) */
		led_slope_up_1   = min(max(new_led_slope_up_1  , 0), 5);
		led_slope_up_2   = min(max(new_led_slope_up_2  , 0), 5);
		led_slope_down_1 = min(max(new_led_slope_down_1, 0), 5);
		led_slope_down_2 = min(max(new_led_slope_down_2, 0), 5);
	}

	return count;
}

static ssize_t store_led_r(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct an30259a_data *data = dev_get_drvdata(dev);
	int ret;
	u8 brightness;

	ret = kstrtou8(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(&data->client->dev, "fail to get brightness.\n");
		goto out;
	}

	if (brightness == 0)
		leds_on(LED_R, false, false, 0);
	else
		leds_on(LED_R, true, false, brightness);

	leds_i2c_write_all(data->client);
	an30259a_debug(data->client);
out:
	return count;
}

static ssize_t store_led_g(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct an30259a_data *data = dev_get_drvdata(dev);
	int ret;
	u8 brightness;

	ret = kstrtou8(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(&data->client->dev, "fail to get brightness.\n");
		goto out;
	}

	if (brightness == 0)
		leds_on(LED_G, false, false, 0);
	else
		leds_on(LED_G, true, false, brightness);

	leds_i2c_write_all(data->client);
	an30259a_debug(data->client);
out:
	return count;
}

static ssize_t store_led_b(struct device *dev,
	struct device_attribute *devattr, const char *buf, size_t count)
{
	struct an30259a_data *data = dev_get_drvdata(dev);
	int ret;
	u8 brightness;

	ret = kstrtou8(buf, 0, &brightness);
	if (ret != 0) {
		dev_err(&data->client->dev, "fail to get brightness.\n");
		goto out;
	}

	if (brightness == 0)
		leds_on(LED_B, false, false, 0);
	else
		leds_on(LED_B, true, false, brightness);

	leds_i2c_write_all(data->client);
	an30259a_debug(data->client);
out:
	return count;

}
#endif

/* Added for led common class */
static ssize_t led_delay_on_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct an30259a_led *led = cdev_to_led(led_cdev);

	return snprintf(buf, 10, "%lu\n", led->delay_on_time_ms);
}

static ssize_t led_delay_on_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct an30259a_led *led = cdev_to_led(led_cdev);
	unsigned long time;

	if (kstrtoul(buf, 0, &time))
		return -EINVAL;

	led->delay_on_time_ms = (int)time;
	return len;
}

static ssize_t led_delay_off_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct an30259a_led *led = cdev_to_led(led_cdev);

	return snprintf(buf, 10, "%lu\n", led->delay_off_time_ms);
}

static ssize_t led_delay_off_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct an30259a_led *led = cdev_to_led(led_cdev);
	unsigned long time;

	if (kstrtoul(buf, 0, &time))
		return -EINVAL;

	led->delay_off_time_ms = (int)time;

	return len;
}

static ssize_t led_blink_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t len)
{
	struct led_classdev *led_cdev = dev_get_drvdata(dev);
	struct an30259a_led *led = cdev_to_led(led_cdev);
	unsigned long blink_set;

	if (kstrtoul(buf, 0, &blink_set))
		return -EINVAL;

	if (!blink_set) {
		led->delay_on_time_ms = LED_OFF;
		an30259a_set_brightness(led_cdev, LED_OFF);
	}

	led_blink_set(led_cdev,
		&led->delay_on_time_ms, &led->delay_off_time_ms);

	return len;
}

static ssize_t disable_samsung_pattern_on_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	return snprintf(buf, 10, "%lu\n", disabled_samsung_pattern);
}

static ssize_t disable_samsung_pattern_on_store(struct device *dev,
			struct device_attribute *attr,
			const char *buf, size_t len)
{

	if (kstrtoul(buf, 0, &disabled_samsung_pattern))
		return -EINVAL;

	return 1;
}

/* permission for sysfs node */
static DEVICE_ATTR(delay_on, 0644, led_delay_on_show, led_delay_on_store);
static DEVICE_ATTR(delay_off, 0644, led_delay_off_show, led_delay_off_store);
static DEVICE_ATTR(blink, 0644, NULL, led_blink_store);
static DEVICE_ATTR(disable_samsung_pattern, 0644, disable_samsung_pattern_on_show, disable_samsung_pattern_on_store);

#ifdef SEC_LED_SPECIFIC
/* below nodes is SAMSUNG specific nodes */
static DEVICE_ATTR(led_r, 0664, NULL, store_led_r);
static DEVICE_ATTR(led_g, 0664, NULL, store_led_g);
static DEVICE_ATTR(led_b, 0664, NULL, store_led_b);
/* led_pattern node permission is 664 */
/* To access sysfs node from other groups */
static DEVICE_ATTR(led_pattern, 0664, NULL, \
					store_an30259a_led_pattern);
static DEVICE_ATTR(led_blink, 0664, NULL, \
					store_an30259a_led_blink);
static DEVICE_ATTR(led_fade, 0664, show_an30259a_led_fade, \
					store_an30259a_led_fade);
static DEVICE_ATTR(led_intensity, 0664, show_an30259a_led_intensity, \
					store_an30259a_led_intensity);
static DEVICE_ATTR(led_speed, 0664, show_an30259a_led_speed, \
					store_an30259a_led_speed);
static DEVICE_ATTR(led_slope, 0664, show_an30259a_led_slope, \
					store_an30259a_led_slope);
static DEVICE_ATTR(led_br_lev, 0664, NULL, \
					store_an30259a_led_br_lev);
static DEVICE_ATTR(led_lowpower, 0664, NULL, \
					store_an30259a_led_lowpower);


#endif
static struct attribute *led_class_attrs[] = {
	&dev_attr_delay_on.attr,
	&dev_attr_delay_off.attr,
	&dev_attr_blink.attr,
	NULL,
};

static struct attribute_group common_led_attr_group = {
	.attrs = led_class_attrs,
};

#ifdef SEC_LED_SPECIFIC
static struct attribute *sec_led_attributes[] = {
	&dev_attr_led_r.attr,
	&dev_attr_led_g.attr,
	&dev_attr_led_b.attr,
	&dev_attr_led_pattern.attr,
	&dev_attr_led_blink.attr,
	&dev_attr_led_fade.attr,
	&dev_attr_led_intensity.attr,
	&dev_attr_led_speed.attr,
	&dev_attr_led_slope.attr,
	&dev_attr_led_br_lev.attr,
	&dev_attr_led_lowpower.attr,
	&dev_attr_disable_samsung_pattern.attr,
	NULL,
};

static struct attribute_group sec_led_attr_group = {
	.attrs = sec_led_attributes,
};
#endif

#ifdef CONFIG_OF
static int an30259a_parse_dt(struct device *dev) {
	struct device_node *np = dev->of_node;
	int ret;
	u32 read_dt_property;

	ret = of_property_read_u32(np,
			"an30259a,default_current", &led_default_cur);
	if (ret < 0) {
		led_default_cur = 0x28;
		pr_warning("%s warning default dt parse[%d]\n", __func__, ret);
	}

	ret = of_property_read_u32(np,
			"an30259a,lowpower_current", &led_lowpower_cur);
	if (ret < 0) {
		led_lowpower_cur = 0x05;
		pr_warning("%s warning lowpower dt parse[%d]\n", __func__, ret);
	}

	ret = of_property_read_u32(np,
			"an30259a,offset_current", &read_dt_property);
	if (ret < 0) {
		led_offset[LED_R] = 0;
		led_offset[LED_G] = 0;
		led_offset[LED_B] = 0;
		pr_warning("%s warning offset dt parse[%d]\n", __func__, ret);
	} else {
		led_offset[LED_R] = ((read_dt_property >> LED_R_SHIFT) & 0xff);
		led_offset[LED_G] = ((read_dt_property >> LED_G_SHIFT) & 0xff);
		led_offset[LED_B] = (read_dt_property & 0xff);
	}

	pr_info("%s LED default 0x%x, lowpower 0x%x\n", __func__,
			led_default_cur, led_lowpower_cur);
	pr_info("%s LED R_off[0x%x] G_off[0x%x] B_off[0x%x]\n", __func__,
			led_offset[LED_R], led_offset[LED_G], led_offset[LED_B]);
	return 0;
}
#endif

static int __devinit an30259a_initialize(struct i2c_client *client,
					struct an30259a_led *led, int channel)
{
	struct an30259a_data *data = i2c_get_clientdata(client);
	struct device *dev = &client->dev;
	int ret;

	/* reset an30259a*/
	ret = i2c_smbus_write_byte_data(client, AN30259A_REG_SRESET,
					AN30259A_SRESET);
	if (ret < 0) {
		dev_err(&client->adapter->dev,
			"%s: failure on i2c write (reg = 0x%2x)\n",
			__func__, AN30259A_REG_SRESET);
		return ret;
	}
	ret = i2c_smbus_read_i2c_block_data(client,
			AN30259A_REG_SRESET | AN30259A_CTN_RW_FLG,
			AN30259A_REG_MAX, data->shadow_reg);
	if (ret < 0) {
		dev_err(&client->adapter->dev,
			"%s: failure on i2c read block(ledxcc)\n",
			__func__);
		return ret;
	}
	led->channel = channel;
	led->cdev.brightness_set = an30259a_set_brightness;
	led->cdev.name = led_conf[channel].name;
	led->cdev.brightness = led_conf[channel].brightness;
	led->cdev.max_brightness = led_conf[channel].max_brightness;
	led->cdev.flags = led_conf[channel].flags;

	ret = led_classdev_register(dev, &led->cdev);

	if (ret < 0) {
		dev_err(dev, "can not register led channel : %d\n", channel);
		return ret;
	}

	ret = sysfs_create_group(&led->cdev.dev->kobj,
			&common_led_attr_group);

	if (ret < 0) {
		dev_err(dev, "can not register sysfs attribute\n");
		return ret;
	}

	leds_set_imax(client, 0x00);

	return 0;
}

static int __devinit an30259a_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	struct an30259a_data *data;
	int ret, i;

	dev_err(&client->adapter->dev, "%s\n", __func__);
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C)) {
		dev_err(&client->dev, "need I2C_FUNC_I2C.\n");
		return -ENODEV;
	}
 
	data = kzalloc(sizeof(*data), GFP_KERNEL);
	if (!data) {
		dev_err(&client->adapter->dev,
			"failed to allocate driver data.\n");
		return -ENOMEM;
	}
#ifdef CONFIG_OF
	ret = an30259a_parse_dt(&client->dev);
	if (ret) {
		pr_err("[%s] an30259a parse dt failed\n", __func__);
		kfree(data);
		return ret;
	}
#endif

	i2c_set_clientdata(client, data);
	data->client = client;
	b_client = client;

	mutex_init(&data->mutex);
	/* initialize LED */

	LED_R_CURRENT = LED_G_CURRENT = LED_B_CURRENT = led_default_cur;
	led_conf[0].max_brightness = LED_R_CURRENT;
	led_conf[1].max_brightness = LED_G_CURRENT;
	led_conf[2].max_brightness = LED_B_CURRENT;

	for (i = 0; i < MAX_NUM_LEDS; i++) {

		ret = an30259a_initialize(client, &data->leds[i], i);

		if (ret < 0) {
			dev_err(&client->adapter->dev, "failure on initialization\n");
			goto exit;
		}
		INIT_WORK(&(data->leds[i].brightness_work),
				 an30259a_led_brightness_work);
	}

#if defined (CONFIG_SEC_FACTORY)
#if defined (CONFIG_SEC_S_PROJECT)
	if ( (f_jig_cable == 0) && (get_lcd_attached() == 0) ) {
		pr_info("%s:Factory MODE - No OCTA, Battery BOOTING\n", __func__);
		leds_on(LED_R, true, false, LED_R_CURRENT);
		leds_i2c_write_all(data->client);
	}
#endif
#endif

#ifdef SEC_LED_SPECIFIC
	led_enable_fade = 0;  /* default to stock behaviour = blink */
//	led_intensity =  0;   /* default to CM behaviour = brighter blink intensity allowed */
	led_intensity = 40;   /* default to Samsung behaviour = normal intensity */
	led_speed = 1;        /* default to stock behaviour = normal blinking/fading speed */
	led_slope_up_1 = 1;   /* default slope durations for fading */
	led_slope_up_2 = 1;
	led_slope_down_1 = 1;
	led_slope_down_2 = 1;

	led_dev = device_create(sec_class, NULL, 0, data, "led");
	if (IS_ERR(led_dev)) {
		dev_err(&client->dev,
			"Failed to create device for samsung specific led\n");
		ret = -ENODEV;
		goto exit1;
	}
	ret = sysfs_create_group(&led_dev->kobj, &sec_led_attr_group);
	if (ret) {
		dev_err(&client->dev,
			"Failed to create sysfs group for samsung specific led\n");
		goto exit;
	}
#endif
	return ret;

#ifdef SEC_LED_SPECIFIC
exit1:
   device_destroy(sec_class, 0);
#endif
exit:
	mutex_destroy(&data->mutex);
	kfree(data);
	return ret;
}

static int __devexit an30259a_remove(struct i2c_client *client)
{
	struct an30259a_data *data = i2c_get_clientdata(client);
	int i;
	dev_dbg(&client->adapter->dev, "%s\n", __func__);
	
	// this is not an ugly hack to shutdown led.
	data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_ON << 0);
	data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_ON << 1);
	data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_ON << 2);
	data->shadow_reg[AN30259A_REG_LED1CNT2 + 0 * 4] &= ~AN30259A_MASK_DELAY;
	data->shadow_reg[AN30259A_REG_LED1CNT2 + 1 * 4] &= ~AN30259A_MASK_DELAY;
	data->shadow_reg[AN30259A_REG_LED1CNT2 + 2 * 4] &= ~AN30259A_MASK_DELAY;
	data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_SLOPE_MODE << 0);
	data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_SLOPE_MODE << 1);
	data->shadow_reg[AN30259A_REG_LEDON] &= ~(LED_SLOPE_MODE << 2);
	data->shadow_reg[AN30259A_REG_LED1CC + 0] = 0;
	data->shadow_reg[AN30259A_REG_LED1CC + 1] = 0;
	data->shadow_reg[AN30259A_REG_LED1CC + 2] = 0;
	msleep(200);	
	
#ifdef SEC_LED_SPECIFIC
	sysfs_remove_group(&led_dev->kobj, &sec_led_attr_group);
#endif
	for (i = 0; i < MAX_NUM_LEDS; i++) {
		sysfs_remove_group(&data->leds[i].cdev.dev->kobj,
						&common_led_attr_group);
		led_classdev_unregister(&data->leds[i].cdev);
		cancel_work_sync(&data->leds[i].brightness_work);
	}
	
	mutex_destroy(&data->mutex);
	kfree(data);
	return 0;
}

static struct i2c_device_id an30259a_id[] = {
	{"an30259a", 0},
	{},
};

MODULE_DEVICE_TABLE(i2c, an30259a_id);

static struct of_device_id an30259a_match_table[] = {
	{ .compatible = "an30259a,led",},
	{ },
};

static struct i2c_driver an30259a_i2c_driver = {
	.driver = {
		.owner	= THIS_MODULE,
		.name	= "an30259a",
		.of_match_table = an30259a_match_table,
	},
	.id_table = an30259a_id,
	.probe = an30259a_probe,
	.remove = __devexit_p(an30259a_remove),
};

static int __init an30259a_init(void)
{
	return i2c_add_driver(&an30259a_i2c_driver);
}

static void __exit an30259a_exit(void)
{
	i2c_del_driver(&an30259a_i2c_driver);
}

module_init(an30259a_init);
module_exit(an30259a_exit);

MODULE_DESCRIPTION("AN30259A LED driver");
MODULE_AUTHOR("Kamaldeep Singla <kamal.singla@samsung.com");
MODULE_LICENSE("GPL v2");
