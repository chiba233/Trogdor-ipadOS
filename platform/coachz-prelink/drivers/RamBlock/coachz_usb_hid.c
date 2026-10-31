/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The keyboard of the Zed keyboard base (USB 18d1:504c, interface 0, a boot keyboard), for the libpayload stack the RamBlock
 * driver runs. libpayload attaches a device by its first interface and calls usb_hid_init() for a HID one (CONFIG_LP_USB_HID).
 * This puts the keyboard in boot protocol (8-byte reports: modifiers, reserved, six key usages), asks it to report only changes,
 * and opens one interrupt-IN queue. coachz_usb_hid_poll() takes the reports, under the same gate as disk I/O, and hands each to
 * coachz_input_keyboard_report() (iokit/Kernel/CoachzInput.cpp), which sends it to iPadOS as report 1 of the keyboard device.
 *
 * The root disk is never polled from here: usb_poll() would also run TEST UNIT READY on it, and a failed one detaches it.
 * coachz_usb_enumerate_hubs() polls only hubs, once at boot, so a keyboard enumerated after the disk is still attached.
 * Not in the build yet: see README.md beside this file.
 */
#include <libpayload.h>
#include <usb/usb.h>

extern int coachz_input_keyboard_report(const unsigned char *boot);
void coachz_usb_hid_poll(void);
void coachz_usb_enumerate_hubs(unsigned ms, usbdev_t *disk);
extern void kprintf(const char *, ...);
extern hci_t *usb_hcs;   /* usb.c: the registered controllers */

#define HID_SET_IDLE 0x0a
#define HID_SET_PROTOCOL 0x0b
#define HID_BOOT_PROTOCOL 0

static usbdev_t *keyboard;
static endpoint_t *keyboard_endpoint;
static void *keyboard_queue;
static unsigned keyboard_reports, keyboard_failures;

static void hid_nop_poll(usbdev_t *dev) { (void)dev; }

static void hid_destroy(usbdev_t *dev) {
	if (dev == keyboard && keyboard_queue) {
		static const unsigned char released[8];
		dev->controller->destroy_intr_queue(keyboard_endpoint, keyboard_queue);
		keyboard_queue = NULL;
		keyboard_endpoint = NULL;
		keyboard = NULL;
		(void)coachz_input_keyboard_report(released);   /* no key stays down after the base is removed */
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
	/* The default is report protocol: an 8-byte reading is only valid after SET_PROTOCOL(boot). */
	int protocol = hid_class_request(dev, HID_SET_PROTOCOL, HID_BOOT_PROTOCOL, intf->bInterfaceNumber);
	int idle = hid_class_request(dev, HID_SET_IDLE, 0, intf->bInterfaceNumber);
	void *queue = protocol < 0 ? NULL : dev->controller->create_intr_queue(in, 8, 8, in->interval);
	kprintf("COACHZ USB HID: keyboard at %d, vendor %04x product %04x, interface %d endpoint 0x%x interval %d: protocol %d idle %d queue %d\n",
	    dev->address, dev->descriptor->idVendor, dev->descriptor->idProduct, intf->bInterfaceNumber, in->endpoint, in->interval,
	    protocol, idle, queue != NULL);
	if (!queue) {
		return;
	}
	keyboard = dev;
	keyboard_endpoint = in;
	keyboard_queue = queue;
}

/* Every report waiting in the queue, in order. The caller holds the RamBlock gate. */
void coachz_usb_hid_poll(void) {
	if (!keyboard || !keyboard_queue) {
		return;
	}
	const unsigned char *report;
	while ((report = keyboard->controller->poll_intr_queue(keyboard_queue)) != NULL) {
		if (coachz_input_keyboard_report(report) == 0) {
			keyboard_reports++;
		} else if (!keyboard_failures++) {
			kprintf("COACHZ USB HID: the keyboard device is not ready for report %u\n", keyboard_reports);
		}
	}
}

/* Once, after the disk is found: poll only the hubs for ms milliseconds, so the devices behind them are attached. */
void coachz_usb_enumerate_hubs(unsigned ms, usbdev_t *disk) {
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
