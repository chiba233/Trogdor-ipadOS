/* SPDX-License-Identifier: BSD-2-Clause */
/* The boot menu's keyboard: the libpayload xHCI and hub stack from the probe, as boot/usb_read.c runs it, with no mass storage driver,
   so the disk is only enumerated and no command reaches it. A boot keyboard (the Zed base's interface 0) is put in boot protocol for
   its 8-byte reports; port reset returns it to the default before XNU or Linux enumerates it. The caches are off, so DMA memory is
   coherent. usb_exit() stops the controller before either system starts, as Depthcharge's own copy of this library does. */
#include <libpayload.h>
#include <usb/usb.h>
#include "xhci.h"

#define XHCI_BASE 0x0a600000ul
#define HEAP_BYTES (4u * 1024u * 1024u)
#define HID_SET_IDLE 0x0a
#define HID_SET_PROTOCOL 0x0b
#define USAGE_ENTER 0x28
#define USAGE_KEYPAD_ENTER 0x58
#define USAGE_DOWN 0x51
#define USAGE_UP 0x52

static uint8_t image_heap[HEAP_BYTES] __attribute__((aligned(4096)));
static size_t heap_used;
static uint64_t ticks_per_us;
static hci_t *controller;
static usbdev_t *keyboard;
static endpoint_t *keyboard_endpoint;
static void *keyboard_queue;
static unsigned keys;

void usb_read_fatal(void) {}
void *memset(void *d, int c, size_t n) {
    uint8_t *p = d;
    while (n--) *p++ = (uint8_t)c;
    return d;
}
void *memcpy(void *d, const void *s, size_t n) {
    uint8_t *to = d;
    const uint8_t *from = s;
    while (n--) *to++ = *from++;
    return d;
}
int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++) if (*x != *y) return *x < *y ? -1 : 1;
    return 0;
}
void *memalign(size_t align, size_t size) {
    size_t at = (heap_used + align - 1) & ~(align - 1);
    if (at > HEAP_BYTES || size > HEAP_BYTES - at) return NULL;
    heap_used = at + size;
    return image_heap + at;
}
void *malloc(size_t size) { return memalign(16, size); }
void *calloc(size_t count, size_t size) {
    void *p = memalign(16, count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
void free(void *p) { (void)p; }
void *dma_malloc(size_t size) { return memalign(16, size); }
void *dma_memalign(size_t align, size_t size) { return memalign(align, size); }
int dma_coherent(const void *p) { (void)p; return 1; }
static uint64_t now(void) {
    uint64_t t;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
    return t;
}
void udelay(unsigned us) { const uint64_t start = now(); while (now() - start < us * ticks_per_us) {} }
void mdelay(unsigned ms) { while (ms--) udelay(1000); }
int gettimeofday(struct timeval *tv, void *zone) {
    const uint64_t us = now() / ticks_per_us;
    (void)zone;
    tv->tv_sec = (time_t)(us / 1000000);
    tv->tv_usec = (long)(us % 1000000);
    return 0;
}
void usb_poll_prepare(void) {}
void usb_generic_create(usbdev_t *dev) { (void)dev; }
void usb_generic_remove(usbdev_t *dev) { (void)dev; }
void usbdisk_create(usbdev_t *dev) { (void)dev; }
void usbdisk_remove(usbdev_t *dev) { (void)dev; }

/* The menu's button bits for what a boot report holds: enter as power, up and down as the volume buttons. */
static void keyboard_poll(usbdev_t *dev) {
    const unsigned char *report;
    if (dev != keyboard || !keyboard_queue) return;
    while ((report = dev->controller->poll_intr_queue(keyboard_queue)) != NULL) {
        keys = 0;
        for (unsigned i = 2; i < 8; i++) {
            if (report[i] == USAGE_ENTER || report[i] == USAGE_KEYPAD_ENTER) keys |= 1;
            if (report[i] == USAGE_UP) keys |= 2;
            if (report[i] == USAGE_DOWN) keys |= 4;
        }
    }
}
static void keyboard_destroy(usbdev_t *dev) {
    if (dev != keyboard) return;
    if (keyboard_queue) dev->controller->destroy_intr_queue(keyboard_endpoint, keyboard_queue);
    keyboard = NULL;
    keyboard_queue = NULL;
    keyboard_endpoint = NULL;
    keys = 0;
}
static int class_request(usbdev_t *dev, unsigned char request, unsigned short value, unsigned short interface) {
    dev_req_t dr;
    memset(&dr, 0, sizeof(dr));
    dr.data_dir = host_to_device;
    dr.req_type = class_type;
    dr.req_recp = iface_recp;
    dr.bRequest = request;
    dr.wValue = value;
    dr.wIndex = interface;
    return dev->controller->control(dev, OUT, sizeof(dr), &dr, 0, NULL);
}
/* libpayload attaches a device by its first interface and calls this for a HID one. */
void usb_hid_init(usbdev_t *dev) {
    dev->poll = keyboard_poll;
    dev->destroy = keyboard_destroy;
    const unsigned char *at = (const unsigned char *)dev->configuration;
    const unsigned char *end = at + dev->configuration->wTotalLength;
    const interface_descriptor_t *intf = NULL;
    for (; at + 2 <= end && at[0] >= 2 && at + at[0] <= end; at += at[0]) {
        if (at[1] == 4 && at[0] >= sizeof(interface_descriptor_t)) {
            intf = (const interface_descriptor_t *)at;
            break;
        }
    }
    if (keyboard || !intf || intf->bInterfaceClass != 3 || intf->bInterfaceSubClass != 1 || intf->bInterfaceProtocol != 1) return;
    endpoint_t *in = NULL;
    for (int i = 0; i < dev->num_endp; i++) {
        if (dev->endpoints[i].type == INTERRUPT && dev->endpoints[i].direction == IN) {
            in = &dev->endpoints[i];
            break;
        }
    }
    if (!in || class_request(dev, HID_SET_PROTOCOL, 0, intf->bInterfaceNumber) < 0) return;
    (void)class_request(dev, HID_SET_IDLE, 0, intf->bInterfaceNumber);
    keyboard_queue = dev->controller->create_intr_queue(in, 8, 8, in->interval);
    if (!keyboard_queue) return;
    keyboard = dev;
    keyboard_endpoint = in;
}

void usb_menu_start(uint64_t freq) {
    ticks_per_us = freq / 1000000;
    controller = xhci_init(XHCI_BASE);
}
/* Finds what changed on the hubs, which blocks while a device is attached, and returns the button bits held on the keyboard. */
unsigned usb_menu_poll(void) {
    if (!controller) return 0;
    usb_poll();
    return keys;
}
void usb_menu_stop(void) {
    if (controller) usb_exit();
    controller = NULL;
}
