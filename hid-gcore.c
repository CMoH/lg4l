/***************************************************************************
 *   Copyright (C) 2014 by Ciprian Ciubotariu <cheepeero@gmx.net>	   *
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
#include <linux/hid.h>
#include <linux/hiddev.h>
#include <linux/input.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

#include "hid-gcore.h"

struct gcore_data *gcore_alloc_data(const char *name, struct hid_device *hdev)
{
	struct gcore_data *gdata = kzalloc(sizeof(struct gcore_data),
					   GFP_KERNEL);

	if (gdata == NULL) {
		dev_err(&hdev->dev,
			"%s error allocating memory for device attributes\n",
			name);
		return NULL;
	}

	gdata->name = kzalloc((strlen(name) + 1) * sizeof(char), GFP_KERNEL);
	if (gdata->name == NULL) {
		kfree(gdata);
		return NULL;
	}
	strcpy(gdata->name, name);

	spin_lock_init(&gdata->lock);

	gdata->hdev = hdev;
	hid_set_drvdata(hdev, gdata);

	return gdata;
}
EXPORT_SYMBOL_GPL(gcore_alloc_data);


void gcore_free_data(struct gcore_data *gdata)
{
  if (gdata != NULL){
	kfree(gdata->name);
	kfree(gdata);
	gdata = NULL;
  }
}
EXPORT_SYMBOL_GPL(gcore_free_data);


int gcore_hid_open(struct gcore_data *gdata)
{
	struct hid_device *hdev = gdata->hdev;
	int error;

	dbg_hid("Preparing to parse %s hid reports\n", gdata->name);

	/* Parse the device reports and start it up */
	error = hid_parse(hdev);
	if (error) {
		dev_err(&hdev->dev, "%s device report parse failed\n",
			gdata->name);
		error = -EINVAL;
		goto err_no_cleanup;
	}

	error = hid_hw_start(hdev,
			     HID_CONNECT_DEFAULT | HID_CONNECT_HIDINPUT_FORCE);
	if (error) {
		dev_err(&hdev->dev, "%s hardware start failed\n", gdata->name);
		error = -EINVAL;
		goto err_cleanup_hid;
	}

	dbg_hid("%s claimed: %d\n", gdata->name, hdev->claimed);

	error = hdev->ll_driver->open(hdev);
	if (error) {
		dev_err(&hdev->dev,
			"%s failed to open input interrupt pipe for key and joystick events\n",
			gdata->name);
		error = -EINVAL;
		goto err_cleanup_hid;
	}

	return 0;

err_cleanup_hid:
	hid_hw_stop(hdev);

err_no_cleanup:
	return error;
}
EXPORT_SYMBOL_GPL(gcore_hid_open);


void gcore_hid_close(struct gcore_data *gdata)
{
  struct hid_device *hdev = NULL;

  if (gdata != NULL){
    hdev = gdata->hdev;
    hdev->ll_driver->close(hdev);
    hid_hw_stop(hdev);
  }
}
EXPORT_SYMBOL_GPL(gcore_hid_close);



int gcore_input_probe(struct gcore_data *gdata,
		      const unsigned int default_keymap[],
		      int keymap_size)
{
	struct hid_device *hdev = gdata->hdev;
	int i, error;
	unsigned int *keycode;

	/* Set up the input device for the key I/O */
	gdata->input_dev = input_allocate_device();
	if (gdata->input_dev == NULL) {
		dev_err(&hdev->dev,
			"%s error initializing the input device",
			gdata->name);
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	input_set_drvdata(gdata->input_dev, gdata);

	gdata->input_dev->name = gdata->name;
	gdata->input_dev->phys = hdev->phys;
	gdata->input_dev->uniq = hdev->uniq;
	gdata->input_dev->id.bustype = hdev->bus;
	gdata->input_dev->id.vendor = hdev->vendor;
	gdata->input_dev->id.product = hdev->product;
	gdata->input_dev->id.version = hdev->version;
	gdata->input_dev->dev.parent = hdev->dev.parent;

	input_set_capability(gdata->input_dev, EV_KEY, KEY_UNKNOWN);
	gdata->input_dev->evbit[0] |= BIT_MASK(EV_REP);

	/* Initialize keymap */
	gdata->input_dev->keycode = kcalloc(keymap_size, sizeof(unsigned int),
					    GFP_KERNEL);
	if (gdata->input_dev->keycode == NULL) {
		error = -ENOMEM;
		goto err_cleanup_input_dev;
	}

	keycode = gdata->input_dev->keycode;
	gdata->input_dev->keycodemax = keymap_size;
	gdata->input_dev->keycodesize = sizeof(unsigned int);
	for (i = 0; i < keymap_size; i++) {
		keycode[i] = default_keymap[i];
		__set_bit(keycode[i], gdata->input_dev->keybit);
	}

	__clear_bit(KEY_RESERVED, gdata->input_dev->keybit);

	/* Register input device */
	error = input_register_device(gdata->input_dev);
	if (error) {
		dev_err(&hdev->dev,
			"%s error registering the input device",
			gdata->name);
		error = -EINVAL;
		goto err_cleanup_input_dev_keycode;
	}

	return 0;

err_cleanup_input_dev_keycode:
	kfree(gdata->input_dev->keycode);

err_cleanup_input_dev:
	input_free_device(gdata->input_dev);

err_no_cleanup:
	return error;
}
EXPORT_SYMBOL_GPL(gcore_input_probe);


void gcore_input_report_key(struct gcore_data *gdata, int scancode, int value)
{
	struct input_dev *idev = gdata->input_dev;
	int error;

	struct input_keymap_entry ke = {
		.flags	  = 0,
		.len	  = sizeof(scancode),
	};
	*((int *) ke.scancode) = scancode;

	error = input_get_keycode(idev, &ke);
	if (!error && ke.keycode != KEY_UNKNOWN && ke.keycode != KEY_RESERVED) {
		/* Only report mapped keys */
		input_report_key(idev, ke.keycode, value);
	} else if (!!value) {
		/* Or report MSC_SCAN on keypress of an unmapped key */
		input_event(idev, EV_MSC, MSC_SCAN, scancode);
	}
}
EXPORT_SYMBOL_GPL(gcore_input_report_key);


void gcore_input_remove(struct gcore_data *gdata)
{
  if (gdata != NULL){
	input_unregister_device(gdata->input_dev);
	kfree(gdata->input_dev->keycode);
	gdata->input_dev->keycode = NULL;
  }
}
EXPORT_SYMBOL_GPL(gcore_input_remove);

static int get_hdev_minor(struct hid_device *hdev) {
	return ((struct hiddev *)hdev->hiddev)->minor;
}

int gcore_leds_probe(struct gcore_data *gdata,
		     const struct led_classdev led_templates[],
		     int led_count)
{
	struct hid_device *hdev = gdata->hdev;
	int error, i, registered_leds;
	char *led_name;

	gdata->led_count = led_count;

	gdata->led_cdev = kcalloc(led_count,
				  sizeof(struct led_classdev *),
				  GFP_KERNEL);
	if (gdata->led_cdev == NULL) {
		error = -ENOMEM;
		goto err_no_cleanup;
	}

	for (i = 0; i < led_count; i++) {
		gdata->led_cdev[i] = kzalloc(sizeof(struct led_classdev),
					     GFP_KERNEL);
		if (gdata->led_cdev[i] == NULL) {
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}

		/* Set the accessor functions by copying from template*/
		*(gdata->led_cdev[i]) = led_templates[i];

		/*
		 * Allocate memory for the LED name
		 *
		 * Since led_classdev->name is a const char* we'll use an
		 * intermediate until the name is formatted with sprintf().
		 */
		led_name = kzalloc(sizeof(char)*20, GFP_KERNEL);
		if (led_name == NULL) {
			error = -ENOMEM;
			goto err_cleanup_led_structs;
		}
		sprintf(led_name, led_templates[i].name, get_hdev_minor(hdev));
		gdata->led_cdev[i]->name = led_name;
	}

	for (i = 0; i < led_count; i++) {
		registered_leds = i;
		error = led_classdev_register(&hdev->dev, gdata->led_cdev[i]);
		if (error < 0) {
			dev_err(&hdev->dev,
				"%s error registering led %d",
				gdata->name, i);
			error = -EINVAL;
			goto err_cleanup_registered_leds;
		}
	}

	return 0;

err_cleanup_registered_leds:
	for (i = 0; i < registered_leds; i++)
		led_classdev_unregister(gdata->led_cdev[i]);

err_cleanup_led_structs:
	for (i = 0; i < led_count; i++) {
		if (gdata->led_cdev[i] != NULL) {
			if (gdata->led_cdev[i]->name != NULL)
				kfree(gdata->led_cdev[i]->name);
			kfree(gdata->led_cdev[i]);
		}
	}

/* err_cleanup_led_array: */
	kfree(gdata->led_cdev);

err_no_cleanup:

	return error;
}
EXPORT_SYMBOL_GPL(gcore_leds_probe);


void gcore_leds_remove(struct gcore_data *gdata)
{
	int i;

	if (gdata != NULL){
	  for (i = 0; i < gdata->led_count; i++) {
		led_classdev_unregister(gdata->led_cdev[i]);
		kfree(gdata->led_cdev[i]->name);
		kfree(gdata->led_cdev[i]);
	  }
	  kfree(gdata->led_cdev);
	  gdata->led_cdev = NULL;
	}
}
EXPORT_SYMBOL_GPL(gcore_leds_remove);


struct hid_device *gcore_led_classdev_to_hdev(struct led_classdev *led_cdev)
{
	struct device *dev;

	/* Get the device associated with the led */
	dev = led_cdev->dev->parent;

	/* Get the hid associated with the device */
	return container_of(dev, struct hid_device, dev);
}
EXPORT_SYMBOL_GPL(gcore_led_classdev_to_hdev);


ssize_t gcore_name_show(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = dev_get_gdata(dev);
	int result;

	spin_lock_irqsave(&gdata->lock, irq_flags);
	result = sprintf(buf, "%s", gdata->name);
	spin_unlock_irqrestore(&gdata->lock, irq_flags);

	return result;
}
EXPORT_SYMBOL_GPL(gcore_name_show);


ssize_t gcore_name_store(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t count)
{
	unsigned long irq_flags;
	struct gcore_data *gdata = dev_get_gdata(dev);
	size_t limit = count;
	char *end;

	end = strpbrk(buf, "\n\r");
	if (end != NULL)
		limit = end - buf;

	if (end != buf) {
		if (limit > 100)
			limit = 100;

		spin_lock_irqsave(&gdata->lock, irq_flags);

		kfree(gdata->name);
		gdata->name = kzalloc(limit+1, GFP_ATOMIC);

		strncpy(gdata->name, buf, limit);

		spin_unlock_irqrestore(&gdata->lock, irq_flags);
	}

	return count;
}
EXPORT_SYMBOL_GPL(gcore_name_store);


ssize_t gcore_minor_show(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	struct gcore_data *gdata = dev_get_gdata(dev);

	return sprintf(buf, "%d\n", get_hdev_minor(gdata->hdev));
}
EXPORT_SYMBOL_GPL(gcore_minor_show);



MODULE_DESCRIPTION("Logitech HID core functions");
MODULE_AUTHOR("Rick L Vinyard Jr (rvinyard@cs.nmsu.edu)");
MODULE_AUTHOR("Alistair Buxton (a.j.buxton@gmail.com)");
MODULE_AUTHOR("Thomas Berger (tbe@boreus.de)");
MODULE_AUTHOR("Ciubotariu Ciprian (cheepeero@gmx.net)");
MODULE_LICENSE("GPL");
