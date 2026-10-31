// SPDX-License-Identifier: BSD-2-Clause-Patent
// Start-up of CoachzBacklight (CoachzBacklight.cpp) below the original Legacy framebuffer, before launchd, so CoreBrightness finds
// it the first time it looks. Called by coachz_display_start() once the display is active; coachz-backlight=1 or =2 enables it.
// The original AppleARMPlatform must be loaded before our class registers: its static initializer builds the AppleARMBacklight
// metaclass that ours names as superclass. Loading starts no matching, as for IOSurface and MobileGraphics in CoachzDisplay.cpp.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <sys/errno.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void coachz_backlight_static_init(void) __asm__("__GLOBAL__sub_I_CoachzBacklight.cpp");
extern "C" IOService *coachz_backlight_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);

extern "C" int
coachz_backlight_start(IOService *display)
{
	static unsigned stage;
	unsigned mode = 0;
	if (!PE_parse_boot_argn("coachz-backlight", &mode, sizeof(mode)) || (mode != 1 && mode != 2)) {
		return 0;
	}
	if (stage) {
		return stage == 3 ? 0 : EIO;
	}
	stage = 1;
	kern_return_t platform = OSKextLoadKextWithIdentifier("com.apple.driver.AppleARMPlatform");
	if (platform) {
		kprintf("COACHZ backlight: original AppleARMPlatform 0x%x\n", platform);
		return EIO;
	}
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (!handle) {
		return EIO;
	}
	coachz_backlight_static_init();
	OSReturn meta = OSMetaClass::postModLoad(handle);
	if (meta) {
		kprintf("COACHZ backlight: late class registration 0x%x\n", meta);
		return EIO;
	}
	stage = 2;
	IOService *service = coachz_backlight_allocate();
	const bool conforms = service && service->metaCast("AppleARMBacklight");
	bool attached = conforms && service->init() && service->attach(display);
	bool started = attached && service->start(display);
	if (started) {
		service->registerService();
		stage = 3;
	} else if (attached) {
		service->detach(display);
	}
	kprintf("COACHZ backlight: service conforms %d attached %d started %d, mode %u\n", conforms, attached, started, mode);
	OSSafeReleaseNULL(service);   // the registry keeps a started service
	return started ? 0 : EIO;
}
