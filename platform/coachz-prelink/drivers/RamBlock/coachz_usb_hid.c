/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The keyboard of the Zed keyboard base (USB 18d1:504c, interface 0), for the libpayload stack the RamBlock driver runs. libpayload
 * attaches a device by its first interface and calls usb_hid_init() for a HID one (CONFIG_LP_USB_HID). This asks it to report only
 * changes and opens one interrupt-IN queue. It stays in the report protocol a device starts in, as Linux's usbhid leaves it: 12-byte
 * reports, modifiers, reserved and six key usages as in a boot report, then the 18 action key bits of the top row and 14 padding bits.
 * coachz_usb_hid_poll() takes the reports, under the same gate as disk I/O, and hands them to coachz_input_keyboard_report() and
 * coachz_input_action_report() (iokit/Kernel/CoachzInput.cpp), which send them to iPadOS as reports 1 and 4 of the keyboard device.
 *
 * The root disk is never polled from here: usb_poll() would also run TEST UNIT READY on it, and a failed one detaches it.
 * coachz_usb_enumerate_hubs() polls only hubs, once at boot, so a keyboard enumerated after the disk is still attached. After boot
 * the hubs off the disk's path keep being polled, as the Linux hub driver watches its ports, so the base can be attached and detached:
 * the disk's own path is left alone, because a port change there would detach it.
 */
#include <libpayload.h>
#include <usb/usb.h>

extern int coachz_input_keyboard_report(const unsigned char *boot);
extern int coachz_input_action_report(const unsigned char *bits);
void coachz_usb_hid_poll(void);
void coachz_usb_enumerate_hubs(unsigned ms, usbdev_t *disk);
extern void kprintf(const char *, ...);
extern hci_t *usb_hcs;   /* usb.c: the registered controllers */

#define HID_SET_IDLE 0x0a
#define KEYBOARD_REPORT 12

static usbdev_t *keyboard;
static endpoint_t *keyboard_endpoint;
static void *keyboard_queue;
static unsigned keyboard_reports, keyboard_failures, keyboard_ticks, keyboard_attaches, keyboard_actions;
static usbdev_t *root_disk;
static unsigned char action_last[4];

static void hid_nop_poll(usbdev_t *dev) { (void)dev; }

static void hid_destroy(usbdev_t *dev) {
	if (dev == keyboard && keyboard_queue) {
		static const unsigned char released[8], none[4];
		dev->controller->destroy_intr_queue(keyboard_endpoint, keyboard_queue);
		keyboard_queue = NULL;
		keyboard_endpoint = NULL;
		keyboard = NULL;
		(void)coachz_input_keyboard_report(released);   /* no key stays down after the base is removed */
		(void)coachz_input_action_report(none);
		memset(action_last, 0, sizeof(action_last));
		kprintf("COACHZ USB HID: keyboard detached after %u reports, %u action key changes\n", keyboard_reports, keyboard_actions);
	}
}

static int hid_class_request(usbdev_t *dev, unsigned char request, unsigned short value, unsigned short interface) {
	dev_req_t dr;
	memset(&dr, 0, sizeof(dr));
	dr.data_dir = host_to_device;
	dr.req_type = class_type;
	dr.req_recp = iface_recp;
	dr.bRequest = request;
	dr.wValue = value;
	dr.wIndex = interface;
	dr.wLength = 0;
	return dev->controller->control(dev, OUT, sizeof(dr), &dr, 0, NULL);
}

/* The first interface descriptor of the device's configuration. */
static const interface_descriptor_t *first_interface(usbdev_t *dev) {
	const unsigned char *at = (const unsigned char *)dev->configuration;
	const unsigned char *end = at + dev->configuration->wTotalLength;
	while (at + 2 <= end && at[0] >= 2 && at + at[0] <= end) {
		if (at[1] == 4 && at[0] >= sizeof(interface_descriptor_t)) {
			return (const interface_descriptor_t *)at;
		}
		at += at[0];
	}
	return NULL;
}

void usb_hid_init(usbdev_t *dev) {
	extern int coachz_usb_keyboard_enabled(void);
	dev->poll = hid_nop_poll;
	dev->destroy = hid_destroy;
	if (!coachz_usb_keyboard_enabled()) {
		return;
	}
	const interface_descriptor_t *intf = first_interface(dev);
	if (!intf || intf->bInterfaceClass != 3 || intf->bInterfaceSubClass != 1 || intf->bInterfaceProtocol != 1) {
		kprintf("COACHZ USB HID: address %d is not a boot keyboard (%d/%d/%d), left alone\n", dev->address,
		    intf ? intf->bInterfaceClass : -1, intf ? intf->bInterfaceSubClass : -1, intf ? intf->bInterfaceProtocol : -1);
		return;
	}
	if (keyboard) {
		kprintf("COACHZ USB HID: a second keyboard at %d, left alone\n", dev->address);
		return;
	}
	endpoint_t *in = NULL;
	for (int i = 0; i < dev->num_endp; i++) {
		if (dev->endpoints[i].type == INTERRUPT && dev->endpoints[i].direction == IN) {
			in = &dev->endpoints[i];
			break;
		}
	}
	if (!in) {
		kprintf("COACHZ USB HID: keyboard at %d has no interrupt-IN endpoint\n", dev->address);
		return;
	}
	int idle = hid_class_request(dev, HID_SET_IDLE, 0, intf->bInterfaceNumber);
	void *queue = dev->controller->create_intr_queue(in, KEYBOARD_REPORT, 8, in->interval);
	kprintf("COACHZ USB HID: keyboard at %d, vendor %04x product %04x, interface %d endpoint 0x%x interval %d: idle %d queue %d\n",
	    dev->address, dev->descriptor->idVendor, dev->descriptor->idProduct, intf->bInterfaceNumber, in->endpoint, in->interval,
	    idle, queue != NULL);
	if (!queue) {
		return;
	}
	keyboard = dev;
	keyboard_endpoint = in;
	keyboard_queue = queue;
	keyboard_attaches++;
}

/* Whether dev is the disk or a hub between the disk and the controller. */
static int on_disk_path(hci_t *controller, usbdev_t *dev) {
	if (!root_disk || root_disk->controller != controller) {
		return 0;
	}
	for (usbdev_t *at = root_disk; at; at = at->address && at->hub >= 0 ? controller->devices[at->hub] : NULL) {
		if (at == dev) {
			return 1;
		}
	}
	return 0;
}

/* Hubs off the disk's path, a few times a second: generic_hub attaches and detaches what changed on their ports. */
static void poll_other_hubs(void) {
	for (hci_t *controller = usb_hcs; controller; controller = controller->next) {
		for (int i = 0; i < 128; i++) {
			usbdev_t *dev = controller->devices[i];
			if (dev && dev->poll && dev->poll != hid_nop_poll && !on_disk_path(controller, dev)) {
				dev->poll(dev);
			}
		}
	}
}

/* Every report waiting in the queue, in order. The caller holds the RamBlock gate. */
void coachz_usb_hid_poll(void) {
	if (keyboard && keyboard_queue) {
		const unsigned char *report;
		while ((report = keyboard->controller->poll_intr_queue(keyboard_queue)) != NULL) {
			if (coachz_input_keyboard_report(report) == 0) {
				keyboard_reports++;
			} else if (!keyboard_failures++) {
				kprintf("COACHZ USB HID: the keyboard device is not ready for report %u\n", keyboard_reports);
			}
			if (memcmp(report + 8, action_last, sizeof(action_last))) {
				memcpy(action_last, report + 8, sizeof(action_last));
				(void)coachz_input_action_report(action_last);
				keyboard_actions++;
			}
		}
	}
	if (root_disk && !(++keyboard_ticks % 32)) {
		poll_other_hubs();
	}
}

/* Once, after the disk is found: poll only the hubs for ms milliseconds, so the devices behind them are attached. */
void coachz_usb_enumerate_hubs(unsigned ms, usbdev_t *disk) {
	root_disk = disk;
	for (unsigned waited = 0; waited < ms; waited += 10) {
		for (hci_t *controller = usb_hcs; controller; controller = controller->next) {
			for (int i = 0; i < 128; i++) {
				usbdev_t *dev = controller->devices[i];
				/* Hubs (and the root hub) find new devices; other classes poll as no-ops. Never the disk. */
				if (dev && dev != disk && dev->poll && dev->poll != hid_nop_poll) {
					dev->poll(dev);
				}
			}
		}
		mdelay(10);
	}
	kprintf("COACHZ USB HID: keyboard %s after the hub pass\n", keyboard ? "attached" : "not found");
}
