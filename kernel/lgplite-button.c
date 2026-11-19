// SPDX-License-Identifier: GPL-2.0-only
/* The button on top of the card, reported as a key through the input layer.
 * See "Board MCU events" in docs/PROTOCOL.md.
 */
#include <linux/input.h>
#include <linux/usb/input.h>

#include "lgplite.h"

#define MCU_EVENT_CLICK 6

/* The vendor driver polls every 125 ms. Events queue up in the MCU, so none
 * are lost between polls.
 */
#define POLL_INTERVAL_MS 125
#define POLL_MAX_EVENTS 8

/* The MCU reports one event per click, long or short, and no release. */
static void lgp_button_poll(struct input_dev *input)
{
	struct lgp *d = input_get_drvdata(input);
	unsigned int i;
	int event;

	for (i = 0; i < POLL_MAX_EVENTS; i++) {
		event = lgp_hw_event(d);
		if (event < 0 && !READ_ONCE(d->disconnected) &&
		    READ_ONCE(d->usb->state) != USB_STATE_NOTATTACHED)
			dev_warn_ratelimited(&d->usb->dev,
					     "Cannot poll the button: %d\n",
					     event);
		if (event <= 0)
			return;
		if (event != MCU_EVENT_CLICK) {
			/* HDMI signal changes and other board events. */
			dev_dbg(&d->usb->dev, "Board MCU event %d\n", event);
			continue;
		}
		/* The single button has scancode 0. */
		input_event(input, EV_MSC, MSC_SCAN, 0);
		input_report_key(input, d->keymap[0], 1);
		input_sync(input);
		input_report_key(input, d->keymap[0], 0);
		input_sync(input);
	}
}

int lgp_button_register(struct lgp *d, struct usb_interface *intf)
{
	struct input_dev *input;
	int ret;

	input = input_allocate_device();
	if (!input)
		return -ENOMEM;
	usb_make_path(d->usb, d->button_phys, sizeof(d->button_phys));
	strlcat(d->button_phys, "/button", sizeof(d->button_phys));
	input->name = "AVerMedia GL310 Button";
	input->phys = d->button_phys;
	usb_to_input_id(d->usb, &input->id);
	input->dev.parent = &intf->dev;
	/* A keymap lets udev hwdb or EVIOCSKEYCODE choose another key. */
	d->keymap[0] = KEY_RECORD;
	input->keycode = d->keymap;
	input->keycodesize = sizeof(d->keymap[0]);
	input->keycodemax = ARRAY_SIZE(d->keymap);
	input_set_capability(input, EV_KEY, d->keymap[0]);
	input_set_capability(input, EV_MSC, MSC_SCAN);
	input_set_drvdata(input, d);
	/* The input core polls only while the device is open. */
	ret = input_setup_polling(input, lgp_button_poll);
	if (ret)
		goto free;
	input_set_poll_interval(input, POLL_INTERVAL_MS);
	ret = input_register_device(input);
	if (ret)
		goto free;
	d->button = input;
	return 0;
free:
	input_free_device(input);
	return ret;
}

/* Returns after the last poll has finished. */
void lgp_button_unregister(struct lgp *d)
{
	if (d->button) {
		input_unregister_device(d->button);
		d->button = NULL;
	}
}
