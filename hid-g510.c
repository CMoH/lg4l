/***************************************************************************
 *   Copyright (C) 2010 by Alistair Buxton				   *
 *   a.j.buxton@gmail.com						   *
 *   based on hid-g13.c							   *
 *									   *
 *   This program is free software: you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation, either version 2 of the License, or	   *
 *   (at your option) any later version.				   *
 *									   *
 *   This driver is distributed in the hope that it will be useful, but	   *
 *   WITHOUT ANY WARRANTY; without even the implied warranty of		   *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU	   *
 *   General Public License for more details.				   *
 *									   *
 *   You should have received a copy of the GNU General Public License	   *
 *   along with this software. If not see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/
#include <linux/fb.h>
#include <linux/hid.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/uaccess.h>
#include <linux/usb.h>
#include <linux/vmalloc.h>
#include <linux/leds.h>
#include <linux/completion.h>
#include <linux/version.h>

#include "hid-ids.h"
#include "hid-gcore.h"
#include "hid-gfb.h"

#define G510_NAME "Logitech G510"

/* Key defines */
#define G510_KEYS 32

/* Backlight defaults */
#define G510_DEFAULT_RED (0)
#define G510_DEFAULT_GREEN (255)
#define G510_DEFAULT_BLUE (0)

#define LED_COUNT 7

/* LED array indices */
#define G510_LED_M1 0
#define G510_LED_M2 1
#define G510_LED_M3 2
#define G510_LED_MR 3
#define G510_LED_BL_R 4
#define G510_LED_BL_G 5
#define G510_LED_BL_B 6

#define G510_READY_SUBSTAGE_1 0x01
#define G510_READY_SUBSTAGE_2 0x02
#define G510_READY_SUBSTAGE_3 0x04
#define G510_READY_STAGE_1    0x07
#define G510_READY_SUBSTAGE_4 0x08
#define G510_READY_SUBSTAGE_5 0x10
#define G510_READY_STAGE_2    0x1F
#define G510_READY_SUBSTAGE_6 0x20
#define G510_READY_SUBSTAGE_7 0x40
#define G510_READY_STAGE_3    0x7F

#define G510_RESET_POST 0x01
#define G510_RESET_MESSAGE_1 0x02
#define G510_RESET_READY 0x03

/* Per device data structure */
struct g510_data {
	/* HID reports */
  	struct hid_report *backlight_report;
	struct hid_report *start_input_report;
	struct hid_report *feature_report_0;
	struct hid_report *feature_report_1;
	struct hid_report *feature_report_3;
	struct hid_report *feature_report_6;
	struct hid_report *feature_report_7;
	struct hid_report *feature_report_8;
	struct hid_report *feature_report_9;
	struct hid_report *feature_report_80;
	struct hid_report *led_report;
	struct hid_report *output_report_0;
  	struct hid_report *output_report_3;
	struct hid_report *output_report_FE;

	/* core state */
	u8 backlight_rgb[3];		/* keyboard illumination */
	u8 led_mbtns;			/* m1, m2, m3 and mr */

	/* initialization stages */
	struct completion ready;
	int ready_stages;
};

/* Convenience macros */
#define hid_get_g510data(hdev) \
	((struct g510_data *)(hid_get_gdata(hdev)->data))
#define dev_get_g510data(dev) \
	((struct g510_data *)(dev_get_gdata(dev)->data))

/*
 * Keymap array indices
 */
static const unsigned int g510_default_keymap[G510_KEYS] = {
	KEY_F1,
	KEY_F2,
	KEY_F3,
	KEY_F4,
	KEY_F5,
	KEY_F6,
	KEY_F7,
	KEY_F8,

	KEY_F9,
	KEY_F10,
	KEY_F11,
	KEY_F12,
	KEY_F13,
	KEY_F14,
	KEY_F15,
	KEY_F16,

	KEY_F17,
	KEY_F18,
	KEY_UNKNOWN,
	KEY_KBDILLUMTOGGLE,
	KEY_PROG1,
	KEY_PROG2,
	KEY_PROG3,
	KEY_RECORD,
	KEY_OK, /* L1 */
	KEY_LEFT, /* L2 */
	KEY_UP, /* L3 */
	KEY_DOWN, /* L4 */
	KEY_RIGHT, /* L5 */
	KEY_UNKNOWN,
	KEY_UNKNOWN,
	KEY_UNKNOWN
};

static void g510_led_send(struct hid_device *hdev, u8 msg, u8 value)
{
	struct g510_data *g510data = hid_get_g510data(hdev);

	g510data->led_report->field[0]->value[1] = msg;
	g510data->led_report->field[0]->value[0] = value;

	hid_hw_request(hdev, g510data->led_report, HID_REQ_SET_REPORT);
}

static void g510_led_mbtns_send(struct hid_device *hdev)
{
	struct g510_data *g510data = hid_get_g510data(hdev);

	g510_led_send(hdev, 0x04, g510data->led_mbtns);
}

static void g510_led_mbtns_brightness_set(struct led_classdev *led_cdev,
					  enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g510_data *g510data = gdata->data;
	int led_nr = 0;
	
	if (led_cdev == gdata->led_cdev[G510_LED_M1])
	  led_nr = 3;
	else if (led_cdev == gdata->led_cdev[G510_LED_M2])
	  led_nr = 2;	
	else if (led_cdev == gdata->led_cdev[G510_LED_M3])
	  led_nr = 1;	
	/* if it is not M1, M2 or M3 then it is MR */
	
	if (value == LED_OFF)
	  g510data->led_mbtns &= ~(1 << (led_nr + 4));
	else
	  g510data->led_mbtns |= 1 << (led_nr+4);

	g510_led_mbtns_send(hdev);
}

static enum led_brightness
g510_led_mbtns_brightness_get(struct led_classdev *led_cdev)
{
        struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g510_data *g510data = gdata->data;
	int led_nr = 0;
	
	if (led_cdev == gdata->led_cdev[G510_LED_M1])
	  led_nr = 3;
	else if (led_cdev == gdata->led_cdev[G510_LED_M2])
	  led_nr = 2;	
	else if (led_cdev == gdata->led_cdev[G510_LED_M3])
	  led_nr = 1;	
	/* if it is not M1, M2 or M3 then it is MR */

	return g510data->led_mbtns & (1 << (led_nr + 4))
	  ? LED_FULL
	  : LED_OFF;
}


static void g510_led_bl_send(struct hid_device *hdev)
{
	struct g510_data *g510data = hid_get_g510data(hdev);

	struct hid_field *field0 = g510data->backlight_report->field[0];

	field0->value[0] = g510data->backlight_rgb[0];
	field0->value[1] = g510data->backlight_rgb[1];
	field0->value[2] = g510data->backlight_rgb[2];
	field0->value[3] = 0x05;

	hid_hw_request(hdev, g510data->backlight_report, HID_REQ_SET_REPORT);
}

static void g510_led_bl_brightness_set(struct led_classdev *led_cdev,
				       enum led_brightness value)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g510_data *g510data = gdata->data;

	if (led_cdev == gdata->led_cdev[G510_LED_BL_R])
		g510data->backlight_rgb[0] = value;
	else if (led_cdev == gdata->led_cdev[G510_LED_BL_G])
		g510data->backlight_rgb[1] = value;
	else if (led_cdev == gdata->led_cdev[G510_LED_BL_B])
		g510data->backlight_rgb[2] = value;

	g510_led_bl_send(hdev);
}

static enum led_brightness
g510_led_bl_brightness_get(struct led_classdev *led_cdev)
{
	struct hid_device *hdev = gcore_led_classdev_to_hdev(led_cdev);
	struct gcore_data *gdata = hid_get_gdata(hdev);
	struct g510_data *g510data = gdata->data;

	if (led_cdev == gdata->led_cdev[G510_LED_BL_R])
		return g510data->backlight_rgb[0];
	else if (led_cdev == gdata->led_cdev[G510_LED_BL_G])
		return g510data->backlight_rgb[1];
	else if (led_cdev == gdata->led_cdev[G510_LED_BL_B])
		return g510data->backlight_rgb[2];

	dev_err(&hdev->dev, G510_NAME " error retrieving LED brightness\n");
	return LED_OFF;
}

static const struct led_classdev g510_led_cdevs[LED_COUNT] = {
	{
		.name			= "g510_%d:orange:m1",
		.brightness_set		= g510_led_mbtns_brightness_set,
		.brightness_get		= g510_led_mbtns_brightness_get,
	},
	{
		.name			= "g510_%d:orange:m2",
		.brightness_set		= g510_led_mbtns_brightness_set,
		.brightness_get		= g510_led_mbtns_brightness_get,
	},
	{
		.name			= "g510_%d:orange:m3",
		.brightness_set		= g510_led_mbtns_brightness_set,
		.brightness_get		= g510_led_mbtns_brightness_get,
	},
	{
		.name			= "g510_%d:red:mr",
		.brightness_set		= g510_led_mbtns_brightness_set,
		.brightness_get		= g510_led_mbtns_brightness_get,
	},
	{
		.name			= "g510_%d:red:bl",
		.brightness_set		= g510_led_bl_brightness_set,
		.brightness_get		= g510_led_bl_brightness_get,
	},
	{
		.name			= "g510_%d:green:bl",
		.brightness_set		= g510_led_bl_brightness_set,
		.brightness_get		= g510_led_bl_brightness_get,
	},
	{
		.name			= "g510_%d:blue:bl",
		.brightness_set		= g510_led_bl_brightness_set,
		.brightness_get		= g510_led_bl_brightness_get,
	},
};

static DEVICE_ATTR(fb_node, 0444, gfb_fb_node_show, NULL);
static DEVICE_ATTR(fb_update_rate, 0664,
		   gfb_fb_update_rate_show, gfb_fb_update_rate_store);
static DEVICE_ATTR(name, 0664, gcore_name_show, gcore_name_store);
static DEVICE_ATTR(minor, 0444, gcore_minor_show, NULL);

/*
 * Create a group of attributes so that we can create and destroy them all
 * at once.
 */
static struct attribute *g510_attrs[] = {
	&dev_attr_name.attr,
	&dev_attr_minor.attr,
	&dev_attr_fb_update_rate.attr,
	&dev_attr_fb_node.attr,
	NULL,	/* need to NULL terminate the list of attributes */
};

static struct attribute_group g510_attr_group = {
	.attrs = g510_attrs,
};

static void g510_raw_event_process_input(struct hid_device *hdev,
	struct gcore_data *gdata,
	u8 *raw_data)
{
	struct input_dev *idev = gdata->input_dev;
	int scancode;
	int value;
	int i;
	int mask;

	/*
	 * This bit turns on and off at random.
	 * G510: does it do this? seems safe to leave here in case
	 */
	raw_data[4] &= 0xFE;

	for (i = 0, mask = 0x01; i < 8; i++, mask <<= 1) {
		scancode = i;
		value = raw_data[1] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 8;
		value = raw_data[2] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 16;
		value = raw_data[3] & mask;
		gcore_input_report_key(gdata, scancode, value);

		scancode = i + 24;
		value = raw_data[4] & mask;
		gcore_input_report_key(gdata, scancode, value);
	}

	input_sync(idev);
}

static int g510_raw_event(struct hid_device *hdev,
			  struct hid_report *report,
			  u8 *raw_data, int size)
{
	/*
	* On initialization receive a 258 byte message with
	* data = 6 0 255 255 255 255 255 255 255 255 ...
	*/
	//unsigned long irq_flags;
	struct gcore_data *gdata = dev_get_gdata(&hdev->dev);
	//	struct g510_data *g510data = gdata->data;

	if (likely(report->id == 3)) {
		g510_raw_event_process_input(hdev, gdata, raw_data);
		return 1;
	}

	return 0;
}

#ifdef CONFIG_PM

static int g510_resume(struct hid_device *hdev)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = hid_get_gdata(hdev);

	spin_lock_irqsave(&gdata->lock, irq_flags);
	g510_led_mbtns_send(hdev);
	g510_led_bl_send(hdev);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return 0;
}

static int g510_reset_resume(struct hid_device *hdev)
{
	return g510_resume(hdev);
}

#endif /* CONFIG_PM */

/***** probe-related functions *****/

static int read_feature_reports(struct gcore_data *gdata)
{
	struct hid_device *hdev = gdata->hdev;
	struct g510_data *g510data = gdata->data;

	struct list_head *feature_report_list =
			    &hdev->report_enum[HID_FEATURE_REPORT].report_list;
	struct list_head *output_report_list =
			    &hdev->report_enum[HID_OUTPUT_REPORT].report_list;
	struct hid_report *report;

	if (list_empty(feature_report_list)) {
		dev_err(&hdev->dev, "no feature report found\n");
		return -ENODEV;
	}
	dbg_hid(G510_NAME " feature report found\n");

	list_for_each_entry(report, feature_report_list, list) {
		switch (report->id) {
		case 0x01:
			g510data->feature_report_1 = report;
			break;
		case 0x03:
			g510data->feature_report_3 = report;
			break;
		case 0x04:
			g510data->led_report = report;
			break;
		case 0x05:
			g510data->backlight_report = report;
			break;
		case 0x06:
			g510data->feature_report_6 = report;
			break;
		case 0x07:
			g510data->feature_report_7 = report;
			break;
		case 0x08:
			g510data->feature_report_8 = report;
			break;
		case 0x09:
			g510data->feature_report_9 = report;
			break;
			
		case 0x80:
			g510data->feature_report_80 = report;
			break;
		default:
			break;
		}
		dbg_hid("%s Feature report: id=%u type=%u size=%u maxfield=%u report_count=%u\n",
			gdata->name,
			report->id, report->type, report->size,
			report->maxfield, report->field[0]->report_count);
	}

	if (list_empty(output_report_list)) {
		dev_err(&hdev->dev, "no output report found\n");
		return -ENODEV;
	}
	dbg_hid("%s output report found\n", gdata->name);

	list_for_each_entry(report, output_report_list, list) {
		dbg_hid("%s output report %d found size=%u maxfield=%u\n",
			gdata->name,
			report->id, report->size, report->maxfield);
		if (report->maxfield > 0) {
			dbg_hid("%s offset=%u size=%u count=%u type=%u\n",
				gdata->name,
				report->field[0]->report_offset,
				report->field[0]->report_size,
				report->field[0]->report_count,
				report->field[0]->report_type);
		}
		switch (report->id) {
		case 0x03:
			g510data->output_report_3 = report;
			break;
		case 0xFE:
			g510data->output_report_FE = report;
			break;
		}
	}

	dbg_hid("Found all reports\n");

	return 0;
}

static void g510_get_report8(struct hid_device *hdev, struct hid_report *report)
{
  //struct mt_device *td = hid_get_drvdata(hdev);
	int ret, size = hid_report_len(report);
	u8 *buf;

	buf = hid_alloc_report_buf(report, GFP_KERNEL);
	if (!buf)
		return;

	ret = hid_hw_raw_request(hdev, report->id, buf, size,
				 HID_FEATURE_REPORT, HID_REQ_GET_REPORT);
	if (ret < 0) {
		dev_warn(&hdev->dev, "failed to fetch feature %d\n",
			 report->id);
	} else {
		ret = hid_report_raw_event(hdev, HID_FEATURE_REPORT, buf,
					   size, 0);
		if (ret)
			dev_warn(&hdev->dev, "failed to report feature\n");
	}

	kfree(buf);
}

static void wait_ready(struct gcore_data *gdata)
{
         struct g510_data *g510data = gdata->data;
	struct hid_device *hdev = gdata->hdev;
	//unsigned long irq_flags;

	dbg_hid("Waiting for G510 to activate\n");
	
	/*
	  host->keyb (2.1.0) ---> urb control, bmRequestType: 0x21, bRequest: 9, wValue: 0x0301, wIndex: 1, wLength: 19    (Report ID: 1, Feature)
	  DATA: 01 00 00 
	        00 00 00 00 00 00 00 00
   	        00 00 00 00 00 00 00 00
	*/
	g510data->feature_report_1->field[0]->value[0] = 0x01;
	g510data->feature_report_1->field[0]->value[1] = 0x00;
	g510data->feature_report_1->field[0]->value[2] = 0x00;
	g510data->feature_report_1->field[0]->value[3] = 0x00;
	g510data->feature_report_1->field[0]->value[4] = 0x00;
	g510data->feature_report_1->field[0]->value[5] = 0x00;
	g510data->feature_report_1->field[0]->value[6] = 0x00;
	g510data->feature_report_1->field[0]->value[7] = 0x00;
	g510data->feature_report_1->field[0]->value[8] = 0x00;
	g510data->feature_report_1->field[0]->value[9] = 0x00;
	g510data->feature_report_1->field[0]->value[10] = 0x00;
	g510data->feature_report_1->field[0]->value[11] = 0x00;
	g510data->feature_report_1->field[0]->value[12] = 0x00;
	g510data->feature_report_1->field[0]->value[13] = 0x00;
	g510data->feature_report_1->field[0]->value[14] = 0x00;
	g510data->feature_report_1->field[0]->value[15] = 0x00;
	g510data->feature_report_1->field[0]->value[16] = 0x00;
	g510data->feature_report_1->field[0]->value[17] = 0x00;
	g510data->feature_report_1->field[0]->value[18] = 0x00;


	hid_hw_request(hdev, g510data->feature_report_1, HID_REQ_SET_REPORT);		

	/*
	 * Clear the LEDs
	 */
	g510data->backlight_rgb[0] = G510_DEFAULT_RED;
	g510data->backlight_rgb[1] = G510_DEFAULT_GREEN;
	g510data->backlight_rgb[2] = G510_DEFAULT_BLUE;
	g510_led_bl_send(hdev);

	/* set M1 key */
	g510data->led_mbtns = 0x80;
	g510_led_mbtns_send(hdev);

	/*
	  host->keyb (2.1.0) ---> urb control, bmRequestType: 0x21, bRequest: 9, wValue: 0x0301, wIndex: 1, wLength: 19    (Report ID: 1, Feature)
	  DATA: 01 00 00 
	        00 00 00 00 00 00 00 00
   	        00 00 00 00 00 00 00 00
	*/
        g510data->feature_report_1->field[0]->value[0] = 0x01;
        g510data->feature_report_1->field[0]->value[1] = 0x00;
        g510data->feature_report_1->field[0]->value[2] = 0x00;
        g510data->feature_report_1->field[0]->value[3] = 0x00;
        g510data->feature_report_1->field[0]->value[4] = 0x00;
        g510data->feature_report_1->field[0]->value[5] = 0x00;
        g510data->feature_report_1->field[0]->value[6] = 0x00;
        g510data->feature_report_1->field[0]->value[7] = 0x00;
        g510data->feature_report_1->field[0]->value[8] = 0x00;
        g510data->feature_report_1->field[0]->value[9] = 0x00;
        g510data->feature_report_1->field[0]->value[10] = 0x00;
        g510data->feature_report_1->field[0]->value[11] = 0x00;
        g510data->feature_report_1->field[0]->value[12] = 0x00;
        g510data->feature_report_1->field[0]->value[13] = 0x00;
        g510data->feature_report_1->field[0]->value[14] = 0x00;
        g510data->feature_report_1->field[0]->value[15] = 0x00;
        g510data->feature_report_1->field[0]->value[16] = 0x00;
        g510data->feature_report_1->field[0]->value[17] = 0x00;
        g510data->feature_report_1->field[0]->value[18] = 0x00;

	hid_hw_request(hdev, g510data->feature_report_1, HID_REQ_SET_REPORT);

	/*
	  host->keyb (2.1.0) ---> urb control, bmRequestType: 0x21, bRequest: 9, wValue: 0x0309, wIndex: 1, wLength: 8    (Report ID: 9, Feature)
	  DATA: 09 02 00 00 00 00 00 00
	*/
	g510data->feature_report_9->field[0]->value[0] = 0x09;
	g510data->feature_report_9->field[0]->value[1] = 0x02;
	g510data->feature_report_9->field[0]->value[2] = 0x00;
	g510data->feature_report_9->field[0]->value[3] = 0x00;
	g510data->feature_report_9->field[0]->value[4] = 0x00;
	g510data->feature_report_9->field[0]->value[5] = 0x00;
	g510data->feature_report_9->field[0]->value[6] = 0x00;
	g510data->feature_report_9->field[0]->value[7] = 0x00;

        hid_hw_request(hdev, g510data->feature_report_9, HID_REQ_SET_REPORT);

	/* Now the keyboard will transfer its memory to the host */
	g510_get_report8(hdev, g510data->feature_report_8);
	
}

static int g510_probe(struct hid_device *hdev,
		      const struct hid_device_id *id)
{
	int error;
	struct gcore_data *gdata;
	struct g510_data *g510data;

	dev_dbg(&hdev->dev, "Logitech G510 HID hardware probe...");

	gdata = gcore_alloc_data(G510_NAME, hdev);
	if (gdata == NULL) {
		dev_err(&hdev->dev,
			G510_NAME
			" can't allocate space for device attributes\n");
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	g510data = kzalloc(sizeof(struct g510_data), GFP_KERNEL);
	if (g510data == NULL) {
		error = -ENOMEM;
		goto err_cleanup_gdata;
	}
	gdata->data = g510data;

	error = gcore_hid_open(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error opening hid device\n",
			gdata->name);
		goto err_cleanup_g510data;
	}

	error = gcore_input_probe(gdata, g510_default_keymap,
				  ARRAY_SIZE(g510_default_keymap));
	if (error) {
		dev_err(&hdev->dev,
			"%s error registering input device\n",
			gdata->name);
		goto err_cleanup_hid;
	}

	error = read_feature_reports(gdata);
	if (error) {
		dev_err(&hdev->dev,
			"%s error reading feature reports\n",
			gdata->name);
		goto err_cleanup_input;
	}

	error = gcore_leds_probe(gdata, g510_led_cdevs,
				 ARRAY_SIZE(g510_led_cdevs));
	if (error) {
		dev_err(&hdev->dev, "%s error registering leds\n", gdata->name);
		goto err_cleanup_input;
	}

	gdata->gfb_data = gfb_probe(hdev, GFB_PANEL_TYPE_160_43_1);
	if (gdata->gfb_data == NULL) {
		dev_err(&hdev->dev, G510_NAME " error registering framebuffer\n");
		goto err_cleanup_leds;
	}

	error = sysfs_create_group(&(hdev->dev.kobj), &g510_attr_group);
	if (error) {
		dev_err(&hdev->dev, G510_NAME " failed to create sysfs group attributes\n");
		goto err_cleanup_gfb;
	}

	wait_ready(gdata);

	dbg_hid("G510 activated and initialized\n");

	/* Everything went well */
	return 0;


err_cleanup_gfb:
	gfb_remove(gdata->gfb_data);

err_cleanup_leds:
	gcore_leds_remove(gdata);

err_cleanup_input:
	gcore_input_remove(gdata);

err_cleanup_hid:
	gcore_hid_close(gdata);

err_cleanup_g510data:
	kfree(g510data);

err_cleanup_gdata:
	gcore_free_data(gdata);

err_no_cleanup:
	hid_set_drvdata(hdev, NULL);
	return error;
}

static void g510_remove(struct hid_device *hdev)
{
	struct gcore_data *gdata = hid_get_drvdata(hdev);
	struct g510_data *g510data = NULL;

	if (gdata != NULL) g510data = gdata->data;

	sysfs_remove_group(&(hdev->dev.kobj), &g510_attr_group);

	if (gdata != NULL) gfb_remove(gdata->gfb_data);

	gcore_leds_remove(gdata);
	gcore_input_remove(gdata);
	gcore_hid_close(gdata);

	kfree(g510data);
	gcore_free_data(gdata);
}


static const struct hid_device_id g510_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G510_LCD) },
	{ HID_USB_DEVICE(USB_VENDOR_ID_LOGITECH, USB_DEVICE_ID_LOGITECH_G510_AUDIO_LCD) },
	{ }
};
MODULE_DEVICE_TABLE(hid, g510_devices);

static struct hid_driver g510_driver = {
	.name			= "hid-g510",
	.id_table		= g510_devices,
	.probe			= g510_probe,
	.remove			= g510_remove,
	.raw_event		= g510_raw_event,

#ifdef CONFIG_PM
	.resume			= g510_resume,
	.reset_resume		= g510_reset_resume,
#endif
};

static int __init g510_init(void)
{
	return hid_register_driver(&g510_driver);
}

static void __exit g510_exit(void)
{
	hid_unregister_driver(&g510_driver);
}

module_init(g510_init);
module_exit(g510_exit);
MODULE_DESCRIPTION("Logitech G510 HID Driver");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_AUTHOR("Ciubotariu Ciprian (cheepeero@gmx.net)");
MODULE_AUTHOR("Carlos Guidugli (guidugli@gmail.com)");
MODULE_LICENSE("GPL");
