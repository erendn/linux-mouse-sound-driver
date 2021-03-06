#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb/input.h>
#include <linux/hid.h>
#include <linux/kmod.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include "config.h"

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");

struct usb_mouse {
	char name[128];
	char phys[64];
	struct usb_device *usbdev;
	struct input_dev *dev;
	struct urb *irq;

	signed char *data;
	dma_addr_t data_dma;
};

struct task_struct *playback_thread;
int left_play = 0;
int right_play = 0;
int middle_play = 0;

int left_clicked = 0;
int right_clicked = 0;
int middle_clicked = 0;

/**
 * Playback thread's function. Plays sounds in the background when 'play' variables are nonzero.
 */
static int playback_func(void *arg) {
	while (!kthread_should_stop()) {
		if (left_play) {
			int result = -1;
			if (left_clicked)
				result = call_usermodehelper(press_argv[0], press_argv, envp, UMH_NO_WAIT);
			else
				result = call_usermodehelper(release_argv[0], release_argv, envp, UMH_NO_WAIT);
			left_play = 0;
		}
		if (right_play) {
			int result = -1;
			if (right_clicked)
				result = call_usermodehelper(press_argv[0], press_argv, envp, UMH_NO_WAIT);
			else
				result = call_usermodehelper(release_argv[0], release_argv, envp, UMH_NO_WAIT);
			right_play = 0;
		}
		if (middle_play) {
			int result = -1;
			if (middle_clicked)
				result = call_usermodehelper(press_argv[0], press_argv, envp, UMH_NO_WAIT);
			else
				result = call_usermodehelper(release_argv[0], release_argv, envp, UMH_NO_WAIT);
			middle_play = 0;
		}
		/* Sleep the thread to relieve the CPU */
		usleep_range(DELAY_LO, DELAY_HI);
	}
    return 0;
}

/**
 * Interrupt Request Handler of the USB Request Block of this device driver.
 */
static void usb_mouse_irq(struct urb *urb) {
	struct usb_mouse *mouse = urb->context;
	signed char *data = mouse->data;
	struct input_dev *dev = mouse->dev;
	int status;

	switch (urb->status) {
	case 0:			/* success */
		break;
	case -ECONNRESET:	/* unlink */
	case -ENOENT:
	case -ESHUTDOWN:
		return;
	/* -EPIPE:  should clear the halt */
	default:		/* error */
		goto resubmit;
	}

	if (left_clicked && !(data[0] & LEFT_BTN_BIT)) { /* negedge of left click */
		left_clicked = 0;
		left_play    = 1;
	} else if (!left_clicked && data[0] & LEFT_BTN_BIT) { /* posedge of left click */
		left_clicked = 1;
		left_play    = 1;
	}

	if (right_clicked && !(data[0] & RGHT_BTN_BIT)) { /* negedge of right click */
		right_clicked = 0;
		right_play    = 1;
	} else if (!right_clicked && data[0] & RGHT_BTN_BIT) { /* posedge of right click */
		right_clicked = 1;
		right_play    = 1;
	}

	if (middle_clicked && !(data[0] & RGHT_BTN_BIT)) { /* negedge of middle click */
		middle_clicked = 0;
		middle_play    = 1;
	} else if (!middle_clicked && data[0] & RGHT_BTN_BIT) { /* posedge of middle click */
		middle_clicked = 1;
		middle_play    = 1;
	}

	input_report_key(dev, BTN_LEFT,   data[0] & LEFT_BTN_BIT);
	input_report_key(dev, BTN_RIGHT,  data[0] & RGHT_BTN_BIT);
	input_report_key(dev, BTN_MIDDLE, data[0] & MIDL_BTN_BIT);
	input_report_key(dev, BTN_SIDE,   data[0] & 0x08);
	input_report_key(dev, BTN_EXTRA,  data[0] & 0x10);

	/* Data indices might be different for different environments */
	input_report_rel(dev, REL_X,     data[2]);
	input_report_rel(dev, REL_Y,     data[4]);
	input_report_rel(dev, REL_WHEEL, data[6]);

	input_sync(dev);
resubmit:
	status = usb_submit_urb (urb, GFP_ATOMIC);
	if (status)
		dev_err(&mouse->usbdev->dev,
			"can't resubmit intr, %s-%s/input0, status %d\n",
			mouse->usbdev->bus->bus_name,
			mouse->usbdev->devpath, status);
}

static int usb_mouse_open(struct input_dev *dev) {
	struct usb_mouse *mouse = input_get_drvdata(dev);

	mouse->irq->dev = mouse->usbdev;
	if (usb_submit_urb(mouse->irq, GFP_KERNEL))
		return -EIO;
	return 0;
}

static void usb_mouse_close(struct input_dev *dev) {
	struct usb_mouse *mouse = input_get_drvdata(dev);

	usb_kill_urb(mouse->irq);
}

static int usb_mouse_probe(struct usb_interface *intf, const struct usb_device_id *id) {
	struct usb_device *dev = interface_to_usbdev(intf);
	struct usb_host_interface *interface;
	struct usb_endpoint_descriptor *endpoint;
	struct usb_mouse *mouse;
	struct input_dev *input_dev;
	int pipe, maxp;
	int error = -ENOMEM;

	interface = intf->cur_altsetting;

	if (interface->desc.bNumEndpoints != 1)
		return -ENODEV;

	endpoint = &interface->endpoint[0].desc;
	if (!usb_endpoint_is_int_in(endpoint))
		return -ENODEV;

	pipe = usb_rcvintpipe(dev, endpoint->bEndpointAddress);
	maxp = usb_maxpacket(dev, pipe, usb_pipeout(pipe));

	mouse = kzalloc(sizeof(struct usb_mouse), GFP_KERNEL);
	input_dev = input_allocate_device();
	if (!mouse || !input_dev)
		goto fail1;

	mouse->data = usb_alloc_coherent(dev, 8, GFP_ATOMIC, &mouse->data_dma);
	if (!mouse->data)
		goto fail1;

	mouse->irq = usb_alloc_urb(0, GFP_KERNEL);
	if (!mouse->irq)
		goto fail2;

	mouse->usbdev = dev;
	mouse->dev = input_dev;

	if (dev->manufacturer)
		strlcpy(mouse->name, dev->manufacturer, sizeof(mouse->name));

	if (dev->product) {
		if (dev->manufacturer)
			strlcat(mouse->name, " ", sizeof(mouse->name));
		strlcat(mouse->name, dev->product, sizeof(mouse->name));
	}

	if (!strlen(mouse->name))
		snprintf(mouse->name, sizeof(mouse->name),
			 "USB HIDBP Mouse %04x:%04x",
			 le16_to_cpu(dev->descriptor.idVendor),
			 le16_to_cpu(dev->descriptor.idProduct));

	usb_make_path(dev, mouse->phys, sizeof(mouse->phys));
	strlcat(mouse->phys, "/input0", sizeof(mouse->phys));

	input_dev->name = mouse->name;
	input_dev->phys = mouse->phys;
	usb_to_input_id(dev, &input_dev->id);
	input_dev->dev.parent = &intf->dev;

	input_dev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
		BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_MIDDLE);
	input_dev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	input_dev->keybit[BIT_WORD(BTN_MOUSE)] |= BIT_MASK(BTN_SIDE) |
		BIT_MASK(BTN_EXTRA);
	input_dev->relbit[0] |= BIT_MASK(REL_WHEEL);

	input_set_drvdata(input_dev, mouse);

	input_dev->open = usb_mouse_open;
	input_dev->close = usb_mouse_close;

	usb_fill_int_urb(mouse->irq, dev, pipe, mouse->data,
			 (maxp > 8 ? 8 : maxp),
			 usb_mouse_irq, mouse, endpoint->bInterval);
	mouse->irq->transfer_dma = mouse->data_dma;
	mouse->irq->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	error = input_register_device(mouse->dev);
	if (error)
		goto fail3;

	usb_set_intfdata(intf, mouse);

	/* Create the kthread for sound playback */
	playback_thread = kthread_run(playback_func, NULL, "sound_playback_thread");
	if (IS_ERR(playback_thread)) {
		printk(KERN_ERR "Could not create the playback thread.\n");
	} else {
		printk(KERN_INFO "Playback thread created.\n");
	}

	return 0;

fail3:	
	usb_free_urb(mouse->irq);
fail2:	
	usb_free_coherent(dev, 8, mouse->data, mouse->data_dma);
fail1:	
	input_free_device(input_dev);
	kfree(mouse);
	return error;
}

static void usb_mouse_disconnect(struct usb_interface *intf) {
	struct usb_mouse *mouse = usb_get_intfdata (intf);

	/* Stop the playback thread */
	kthread_stop(playback_thread);

	usb_set_intfdata(intf, NULL);
	if (mouse) {
		usb_kill_urb(mouse->irq);
		input_unregister_device(mouse->dev);
		usb_free_urb(mouse->irq);
		usb_free_coherent(interface_to_usbdev(intf), 8, mouse->data, mouse->data_dma);
		kfree(mouse);
	}
}

static const struct usb_device_id usb_mouse_id_table[] = {
	{ USB_INTERFACE_INFO(USB_INTERFACE_CLASS_HID, USB_INTERFACE_SUBCLASS_BOOT,
		USB_INTERFACE_PROTOCOL_MOUSE) },
	{ }	/* Terminating entry */
};

MODULE_DEVICE_TABLE (usb, usb_mouse_id_table);

static struct usb_driver usb_mouse_driver = {
	.name		= "usbmouse",
	.probe		= usb_mouse_probe,
	.disconnect	= usb_mouse_disconnect,
	.id_table	= usb_mouse_id_table,
};

module_usb_driver(usb_mouse_driver);
