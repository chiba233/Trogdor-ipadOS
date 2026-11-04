/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The Zed keyboard base's input (USB 18d1:504c), whichever USB stack carries its reports: the libpayload stack (coachz_usb_hid.c) or
 * Apple's host stack through IOUSBHostHIDDevice and the HID bridge (CoachzUsbHost.cpp). Both hand the raw interrupt reports here, and
 * iPadOS gets them as reports of the one keyboard device (iokit/Kernel/CoachzInput.cpp): 1 the keyboard, 4 the top row's action keys,
 * 5 the touchpad as a mouse.
 *
 * Interface 0, the keyboard, stays in the report protocol a device starts in, as Linux's usbhid leaves it: 12-byte reports, modifiers,
 * reserved and six key usages as in a boot report, then the 18 action key bits of the top row and 14 padding bits. Interface 2 is the
 * touchpad, which Linux's hid-multitouch drives: 44-byte reports with no mode to set (the descriptor has no Input Mode feature), five
 * 8-byte contacts from byte 1 (confidence, tip, in range, 4 bits of identifier, 9 of pressure, 12 each of width, height, X up to 3340
 * and Y up to 1811), then 7 bits of contact count and the button in byte 41. One finger moves the pointer, two scroll, a quick touch
 * clicks, and the button clicks, on the right with two fingers down.
 */
#include <stddef.h>
#include <string.h>

extern int coachz_input_keyboard_report(const unsigned char *boot);
extern int coachz_input_action_report(const unsigned char *bits);
extern int coachz_input_mouse_report(int buttons, int dx, int dy, int wheel, int pan);
extern unsigned long long mach_absolute_time(void);
extern void kprintf(const char *, ...);

void coachz_zed_keyboard(const unsigned char *report, unsigned length);
void coachz_zed_pad(const unsigned char *report, unsigned length);
void coachz_zed_release(int keyboard, int pad);
void coachz_zed_counts(unsigned *keyboard_reports, unsigned *actions, unsigned *pad_reports, unsigned *clicks);

#define KEYBOARD_BOOT 8
#define KEYBOARD_REPORT 12
#define PAD_REPORT 44
#define PAD_TAP_TICKS (19200000ull * 180 / 1000)   /* a touch shorter than 180 ms is a click */
#define PAD_TAP_TRAVEL 96                          /* and it moved less than about 3 mm */
#define PAD_MOVE_DIVISOR 2                         /* 33 units a millimetre to mouse counts */
#define PAD_SCROLL_DIVISOR 40

static unsigned keyboard_reports, keyboard_failures, keyboard_actions, keyboard_short;
static unsigned char action_last[4];
static unsigned pad_reports, pad_clicks;
static struct {
	int valid[16], x[16], y[16];          /* per contact identifier */
	int peak, travel, buttons, carry_x, carry_y, scroll_x, scroll_y, clicked;
	unsigned long long began;
} pad;

void coachz_zed_keyboard(const unsigned char *report, unsigned length) {
	if (length < KEYBOARD_BOOT) {
		return;
	}
	if (coachz_input_keyboard_report(report) == 0) {
		keyboard_reports++;
	} else if (!keyboard_failures++) {
		kprintf("COACHZ USB HID: the keyboard device is not ready for report %u\n", keyboard_reports);
	}
	if (length < KEYBOARD_REPORT) {
		/* A boot protocol report has no action key bits. */
		if (!keyboard_short++) {
			kprintf("COACHZ USB HID: %u-byte keyboard reports, no action keys\n", length);
		}
		return;
	}
	if (memcmp(report + 8, action_last, sizeof(action_last))) {
		memcpy(action_last, report + 8, sizeof(action_last));
		(void)coachz_input_action_report(action_last);
		keyboard_actions++;
	}
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
void coachz_zed_pad(const unsigned char *r, unsigned length) {
	if (length < 42 || r[0] != 1) {
		return;
	}
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
		/* A plain mouse wheel's direction: iPadOS applies its own natural scrolling setting on top, so inverting here too scrolled backwards. */
		pad.scroll_x += dx / 2;
		pad.scroll_y += dy / 2;
		wheel = pad_clip(-pad.scroll_y / PAD_SCROLL_DIVISOR);
		sideways = pad_clip(pad.scroll_x / PAD_SCROLL_DIVISOR);
		pad.scroll_y += wheel * PAD_SCROLL_DIVISOR;
		pad.scroll_x -= sideways * PAD_SCROLL_DIVISOR;
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

/* The base was removed: no key or button stays down. */
void coachz_zed_release(int keyboard, int touchpad) {
	if (keyboard) {
		static const unsigned char released[8], none[4];
		(void)coachz_input_keyboard_report(released);
		(void)coachz_input_action_report(none);
		memset(action_last, 0, sizeof(action_last));
	}
	if (touchpad) {
		(void)coachz_input_mouse_report(0, 0, 0, 0, 0);
		memset(&pad, 0, sizeof(pad));
	}
}

void coachz_zed_counts(unsigned *reports, unsigned *actions, unsigned *touches, unsigned *clicks) {
	*reports = keyboard_reports;
	*actions = keyboard_actions;
	*touches = pad_reports;
	*clicks = pad_clicks;
}
