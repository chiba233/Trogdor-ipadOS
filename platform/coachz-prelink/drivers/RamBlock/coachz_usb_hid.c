/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The keyboard of the Zed keyboard base (USB 18d1:504c, interface 0), for the libpayload stack the RamBlock driver runs. libpayload
 * attaches a device by its first interface and calls usb_hid_init() for a HID one (CONFIG_LP_USB_HID). This asks it to report only
 * changes and opens one interrupt-IN queue. It stays in the report protocol a device starts in, as Linux's usbhid leaves it: 12-byte
 * reports, modifiers, reserved and six key usages as in a boot report, then the 18 action key bits of the top row and 14 padding bits.
 * coachz_usb_hid_poll() takes the reports, under the same gate as disk I/O, and hands them to coachz_input_keyboard_report() and
 * coachz_input_action_report() (iokit/Kernel/CoachzInput.cpp), which send them to iPadOS as reports 1 and 4 of the keyboard device.
 * Interface 2 is the touchpad, which Linux's hid-multitouch drives: 44-byte reports with no mode to set (the descriptor has no Input Mode
 * feature), five 8-byte contacts from byte 1 (confidence, tip, in range, 4 bits of identifier, 9 of pressure, 12 each of width, height, X up to
 * 3340 and Y up to 1811), then 7 bits of contact count and the button in byte 41. iPadOS gets a mouse from it (report 5): one finger moves the
 * pointer, two scroll, a quick touch clicks, and the button clicks, on the right with two fingers down.
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
extern int coachz_input_mouse_report(int buttons, int dx, int dy, int wheel, int pan);
extern unsigned long long mach_absolute_time(void);
void coachz_usb_hid_poll(void);
void coachz_usb_enumerate_hubs(unsigned ms, usbdev_t *disk);
extern void kprintf(const char *, ...);
extern hci_t *usb_hcs;   /* usb.c: the registered controllers */

#define HID_SET_IDLE 0x0a
#define KEYBOARD_REPORT 12
#define PAD_REPORT 44
#define PAD_ZED_INTERFACE 2
#define PAD_TAP_TICKS (19200000ull * 180 / 1000)   /* a touch shorter than 180 ms is a click */
#define PAD_TAP_TRAVEL 96                          /* and it moved less than about 3 mm */
#define PAD_MOVE_DIVISOR 2                         /* 33 units a millimetre to mouse counts */
#define PAD_SCROLL_DIVISOR 40

static usbdev_t *keyboard;
static endpoint_t *keyboard_endpoint;
static void *keyboard_queue;
static unsigned keyboard_reports, keyboard_failures, keyboard_ticks, keyboard_attaches, keyboard_actions;
static usbdev_t *root_disk;
static unsigned char action_last[4];
static endpoint_t *pad_endpoint;
static void *pad_queue;
static unsigned pad_reports, pad_clicks;
static struct {
	int valid[16], x[16], y[16];          /* per contact identifier */
	int peak, travel, buttons, carry_x, carry_y, scroll_x, scroll_y, clicked;
	unsigned long long began;
} pad;

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
		if (pad_queue) {
			dev->controller->destroy_intr_queue(pad_endpoint, pad_queue);
			pad_queue = NULL;
			pad_endpoint = NULL;
			(void)coachz_input_mouse_report(0, 0, 0, 0, 0);
			memset(&pad, 0, sizeof(pad));
		}
		kprintf("COACHZ USB HID: keyboard detached after %u reports, %u action key changes; touchpad %u reports, %u clicks\n", keyboard_reports, keyboard_actions,
		    pad_reports, pad_clicks);
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

/* The touchpad's interrupt-IN endpoint: the first one in its interface's descriptors. */
static endpoint_t *pad_find(usbdev_t *dev) {
	const unsigned char *at = (const unsigned char *)dev->configuration;
	const unsigned char *end = at + dev->configuration->wTotalLength;
	int inside = 0;
	for (; at + 2 <= end && at[0] >= 2 && at + at[0] <= end; at += at[0]) {
		if (at[1] == 4 && at[0] >= sizeof(interface_descriptor_t)) {
			const interface_descriptor_t *intf = (const interface_descriptor_t *)at;
			inside = intf->bInterfaceNumber == PAD_ZED_INTERFACE && !intf->bAlternateSetting && intf->bInterfaceClass == 3;
		} else if (inside && at[1] == 5 && (at[2] & 0x80)) {
			for (int i = 0; i < dev->num_endp; i++) {
				if (dev->endpoints[i].endpoint == at[2]) return &dev->endpoints[i];
			}
		}
	}
	return NULL;
}

static inline unsigned pad_bits(const unsigned char *r, unsigned bit, unsigned size) {
	unsigned value = 0;
	for (unsigned i = 0; i < size; i++) {
		value |= (unsigned)((r[(bit + i) / 8] >> ((bit + i) % 8)) & 1) << i;
	}
	return value;
}

static inline int pad_clip(int v) { return v > 127 ? 127 : v < -127 ? -127 : v; }

/* hid-multitouch's reading of one report (confidence and tip make a contact), turned into a mouse. */
static void pad_report(const unsigned char *r) {
	unsigned char seen[16] = {0};
	int fingers = 0, dx = 0, dy = 0;
	for (unsigned k = 0; k < 5; k++) {
		const unsigned base = 8 + 64 * k;
		if (!pad_bits(r, base, 1) || !pad_bits(r, base + 1, 1)) continue;
		const unsigned id = pad_bits(r, base + 3, 4);
		const int x = (int)pad_bits(r, base + 40, 12), y = (int)pad_bits(r, base + 52, 12);
		if (pad.valid[id] && fingers < 2) {   /* the first two contacts carry the gesture */
			dx += x - pad.x[id];
			dy += y - pad.y[id];
		}
		pad.valid[id] = 1;
		pad.x[id] = x;
		pad.y[id] = y;
		seen[id] = 1;
		fingers++;
	}
	for (unsigned id = 0; id < 16; id++) if (!seen[id]) pad.valid[id] = 0;
	const int pressed = (r[41] & 0x80) != 0;
	const unsigned long long now = mach_absolute_time();
	if (fingers && !pad.peak) {
		pad.began = now;
		pad.travel = 0;
		pad.clicked = 0;
		pad.carry_x = pad.carry_y = pad.scroll_x = pad.scroll_y = 0;
	}
	if (fingers > pad.peak) pad.peak = fingers;
	pad.travel += (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
	if (pressed) pad.clicked = 1;
	int mx = 0, my = 0, wheel = 0, sideways = 0;
	if (fingers == 1) {
		pad.carry_x += dx;
		pad.carry_y += dy;
		mx = pad_clip(pad.carry_x / PAD_MOVE_DIVISOR);
		my = pad_clip(pad.carry_y / PAD_MOVE_DIVISOR);
		pad.carry_x -= mx * PAD_MOVE_DIVISOR;
		pad.carry_y -= my * PAD_MOVE_DIVISOR;
	} else if (fingers >= 2) {
		/* Natural scrolling: content follows the fingers. */
		pad.scroll_x += dx / 2;
		pad.scroll_y += dy / 2;
		wheel = pad_clip(pad.scroll_y / PAD_SCROLL_DIVISOR);
		sideways = pad_clip(-pad.scroll_x / PAD_SCROLL_DIVISOR);
		pad.scroll_y -= wheel * PAD_SCROLL_DIVISOR;
		pad.scroll_x += sideways * PAD_SCROLL_DIVISOR;
	}
	const int buttons = pressed ? (fingers >= 2 ? 2 : 1) : 0;
	if (buttons != pad.buttons || mx || my || wheel || sideways) {
		(void)coachz_input_mouse_report(buttons, mx, my, wheel, sideways);
		if (buttons && !pad.buttons) pad_clicks++;
		pad.buttons = buttons;
	}
	if (!fingers && pad.peak) {
		if (!pad.clicked && now - pad.began < PAD_TAP_TICKS && pad.travel < PAD_TAP_TRAVEL) {
			const int button = pad.peak >= 2 ? 2 : 1;
			(void)coachz_input_mouse_report(button, 0, 0, 0, 0);
			(void)coachz_input_mouse_report(0, 0, 0, 0, 0);
			pad_clicks++;
		}
		pad.peak = 0;
	}
	pad_reports++;
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
	endpoint_t *touch = pad_find(dev);
	if (touch) {
		const int touch_idle = hid_class_request(dev, HID_SET_IDLE, 0, PAD_ZED_INTERFACE);
		pad_queue = dev->controller->create_intr_queue(touch, PAD_REPORT, 8, touch->interval);
		pad_endpoint = pad_queue ? touch : NULL;
		kprintf("COACHZ USB HID: touchpad endpoint 0x%x interval %d: idle %d queue %d\n", touch->endpoint, touch->interval, touch_idle, pad_queue != NULL);
	}
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
		if (pad_queue) {
			while ((report = keyboard->controller->poll_intr_queue(pad_queue)) != NULL) {
				if (report[0] == 1) pad_report(report);
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
