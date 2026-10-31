# Zed keyboard over USB: not in the build yet

`coachz_usb_hid.c` is the USB half of the keyboard. The iPadOS half, `coachz_input_keyboard_report()`, is in
`patches/xnu/coachz-prelink-input.patch`. The file sits here because `drivers/RamBlock/` is copied straight into the build,
so a file there would be built at once. It compiles cleanly, with warnings on, under the USB stack's compile flags.

## Wiring it in

1. Move `coachz_usb_hid.c` into `platform/coachz-prelink/drivers/RamBlock/`, and in that folder's `build-usb-stack.py`, add
   `HERE / 'coachz_usb_hid.c'` next to `HERE / 'coachz_usb.c'` in the source list.
2. In `boot/usb_compat/libpayload-config.h`, set `CONFIG_LP_USB_HID 1`. The probe's own USB reader (`boot/usb_read.c`) shares
   this header: give it an empty `void usb_hid_init(usbdev_t *dev) { (void)dev; }`, or give the kernel build its own
   override of the macro instead.
3. In `coachz_usb.c`, `coachz_usb_open()`, once the disk is found and checked, call `coachz_usb_enumerate_hubs(1000, disk)`.
   This polls only the hubs for one second, so a keyboard that enumerates after the disk still gets attached.
4. In `NativeRamBlock.cpp`, add a thread call every 8 ms that runs `coachz_usb_hid_poll()` under `gate`, the same lock as block
   I/O. Start it with the log flusher.
5. Enable `coachz-prelink-input.patch` (after the battery patch) and `coachz-buttons=1`. The keyboard device it publishes is
   what the reports go to.

Never call `usb_poll()` after boot: it also runs TEST UNIT READY on the root disk, and libpayload detaches a disk whose test
fails.

## On hardware

- `COACHZ USB HID: keyboard at N, vendor 18d1 product 504c, interface 0 ...: protocol 0 idle 0 queue 1`, then
  `keyboard attached after the hub pass`.
- `COACHZ input: keyboard/consumer device: ... started 1`.
- Typing in a text field. Letters, digits, Shift/Ctrl/Alt/Command and release first; the top row (Vivaldi) and the touchpad
  (interface 2, multi-touch, not a boot mouse) come later.
