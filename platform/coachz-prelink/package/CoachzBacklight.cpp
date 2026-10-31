// SPDX-License-Identifier: BSD-2-Clause-Patent
// The panel backlight for CoreBrightness: CoachzBacklight inherits the original AppleARMPlatform AppleARMBacklight class (its
// constructor, destructor and metaclass bound by build-coachz-backlight.py), so IOObjectConformsTo(service, "AppleARMBacklight")
// holds and CoreBrightness takes it for the built-in display. Class structure and published properties adapted from
// unixnu-sm-t733 NativeT733Backlight (NativeLegacyDisplay.cpp); the request hand-off follows UniXNU-sheng AppleARMBacklight.cpp.
//
// The backend is the Chrome EC's display PWM (EC_CMD_PWM_SET_DUTY, type DISPLAY_LIGHT, index 0; duty 0..65535) through
// coachz_ec_command() of coachz-prelink-battery.patch, the one owner of the EC bus. setProperties never touches the EC: it records
// the newest request and wakes the worker thread, which applies it at most every 20 ms (a newer request replaces one not yet taken),
// steps through FadePeriod, and reads the duty back (EC_CMD_PWM_GET_DUTY) once the value settles. The AP enable (TLMM GPIO12) and
// the EC's own gate stay as the boot probe left them: only the duty changes, and the dimmest level stays lit.
#include "OriginalAppleARMBacklightABI.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread.h>
extern "C" {
#include "coachz_ec_proto.h"
int coachz_ec_command(uint16_t command, uint8_t command_version, const void *args, uint32_t size, void *reply, uint32_t room,
    uint32_t *length);
IOService *coachz_backlight_allocate(void);
}

namespace {
const uint16_t kPwmSetDuty = 0x0025, kPwmGetDuty = 0x0026;
const uint8_t kPwmTypeDisplayLight = 2;
const uint32_t kStepMs = 20;

// Duty and nominal luminance (host-tested by tests/test_backlight.py).
// The lowest duty is the first step of the Linux Trogdor pwm-backlight curve (196/4096: "the panels don't seem to like anything
// below ~5%"); 0 is never sent, so the dimmest setting stays lit. Luminance is linear in duty. The nits range is nominal, not a
// measurement of this panel: CoreBrightness needs some range, and only the duty reaches the hardware.
const uint32_t kMinDuty = 3136, kMaxDuty = 65535;
const uint32_t kMaxLevel = 255, kMaxFadeMs = 5000;
const uint32_t kMinMilliNits = 20000, kMidMilliNits = 100000, kMaxMilliNits = 400000;

uint32_t dutyForMilliNits(uint32_t milliNits) {
	if (milliNits <= kMinMilliNits) return kMinDuty;
	if (milliNits >= kMaxMilliNits) return kMaxDuty;
	const uint64_t span = kMaxMilliNits - kMinMilliNits;
	return kMinDuty + (uint32_t)(((uint64_t)(milliNits - kMinMilliNits) * (kMaxDuty - kMinDuty) + span / 2) / span);
}

uint32_t milliNitsForDuty(uint32_t duty) {
	if (duty <= kMinDuty) return kMinMilliNits;
	if (duty >= kMaxDuty) return kMaxMilliNits;
	const uint64_t span = kMaxDuty - kMinDuty;
	return kMinMilliNits + (uint32_t)(((uint64_t)(duty - kMinDuty) * (kMaxMilliNits - kMinMilliNits) + span / 2) / span);
}

// The 0..255 "brightness" parameter spans the same nominal range.
uint32_t milliNitsForLevel(uint32_t level) {
	if (level >= kMaxLevel) return kMaxMilliNits;
	return kMinMilliNits + (uint32_t)(((uint64_t)level * (kMaxMilliNits - kMinMilliNits) + kMaxLevel / 2) / kMaxLevel);
}

uint32_t levelForMilliNits(uint32_t milliNits) {
	if (milliNits <= kMinMilliNits) return 0;
	if (milliNits >= kMaxMilliNits) return kMaxLevel;
	const uint64_t span = kMaxMilliNits - kMinMilliNits;
	return (uint32_t)(((uint64_t)(milliNits - kMinMilliNits) * kMaxLevel + span / 2) / span);
}

// brightness-nits is signed 16.16 nits.
uint32_t milliNitsForFixed(int32_t fixed) {
	if (fixed <= 0) return 0;
	const uint64_t milli = ((uint64_t)fixed * 1000 + 32768) >> 16;
	return milli > kMaxMilliNits ? kMaxMilliNits : (uint32_t)milli;
}

// FadePeriod is 16.16 seconds.
uint32_t fadeMsForFixed(uint32_t fixed) {
	const uint64_t ms = ((uint64_t)fixed * 1000) >> 16;
	return ms > kMaxFadeMs ? kMaxFadeMs : (uint32_t)ms;
}

// The duty a fade from `from` to `to` over `period` ms has reached after `elapsed` ms.
uint32_t fadeDuty(uint32_t from, uint32_t to, uint32_t elapsed, uint32_t period) {
	if (!period || elapsed >= period) return to;
	const int64_t delta = (int64_t)to - (int64_t)from;
	return (uint32_t)((int64_t)from + delta * (int64_t)elapsed / (int64_t)period);
}
// End of the host-tested functions.

int pwmSet(uint32_t duty) {
	const uint8_t args[4] = {(uint8_t)duty, (uint8_t)(duty >> 8), kPwmTypeDisplayLight, 0};
	uint32_t length = 0;
	return coachz_ec_command(kPwmSetDuty, 0, args, sizeof(args), nullptr, 0, &length);
}

int pwmGet(uint32_t *duty) {
	const uint8_t args[2] = {kPwmTypeDisplayLight, 0};
	uint8_t reply[EC_MAX_REPLY];
	uint32_t length = 0;
	int error = coachz_ec_command(kPwmGetDuty, 0, args, sizeof(args), reply, sizeof(reply), &length);
	if (!error && length < 2) {
		error = EC_ERR_SHORT;
	}
	if (!error) {
		*duty = coachz_ec_le16(reply);
	}
	return error;
}

uint32_t elapsedMs(uint64_t since) {
	uint64_t now = mach_absolute_time(), ns = 0;
	absolutetime_to_nanoseconds(now - since, &ns);
	return ns / 1000000 > 0xffffffffull ? 0xffffffffu : (uint32_t)(ns / 1000000);
}

bool setNumber(OSDictionary *dictionary, const char *key, uint32_t value) {
	OSNumber *number = OSNumber::withNumber(value, 32);
	bool set = dictionary && number && dictionary->setObject(key, number);
	OSSafeReleaseNULL(number);
	return set;
}
}  // namespace

class CoachzBacklight : public AppleARMBacklight {
	OSDeclareDefaultStructors(CoachzBacklight)
	IOLock *lock = nullptr;
	uint64_t request = 0;            // 1 << 63 | duty << 32 | fade ms, taken by the worker
	uint32_t mode = 0;               // coachz-backlight: 1 CoreBrightness service, 2 EC sweep without backlight-control
	uint32_t applied = 0, readback = 0, sets = 0, errors = 0;
	int lastError = 0;

	bool publish(uint32_t milliNits) {
		OSDictionary *parameters = OSDictionary::withCapacity(2), *brightness = OSDictionary::withCapacity(3);
		OSDictionary *nits = OSDictionary::withCapacity(3);
		bool published = parameters && brightness && nits &&
		    setNumber(brightness, "min", 0) && setNumber(brightness, "max", kMaxLevel) &&
		    setNumber(brightness, "value", levelForMilliNits(milliNits)) &&
		    setNumber(nits, "min", kMinMilliNits) && setNumber(nits, "max", kMaxMilliNits) && setNumber(nits, "value", milliNits) &&
		    parameters->setObject("brightness", brightness) && parameters->setObject("BrightnessMilliNits", nits) &&
		    setProperty("IODisplayParameters", parameters);
		OSSafeReleaseNULL(nits); OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
		return published;
	}

	// CoreBrightness dereferences every calibration number; the currents stay zero because nothing here measures them.
	bool publishCalibration() {
		OSDictionary *calibration = OSDictionary::withCapacity(9);
		const struct { const char *name; uint32_t fixed; } entries[] = {
			{"current-for-max-backlight", 0}, {"current-for-mid-backlight", 0}, {"hardware-max-current-limit", 0},
			{"lMinProduct", (kMinMilliNits / 1000) << 16}, {"lMidProduct", (kMidMilliNits / 1000) << 16},
			{"lMaxProduct", (kMaxMilliNits / 1000) << 16}, {"lMinPanel", (kMinMilliNits / 1000) << 16},
			{"lMidPanel", (kMidMilliNits / 1000) << 16}, {"lMaxPanel", (kMaxMilliNits / 1000) << 16}};
		bool published = calibration != nullptr;
		for (const auto &entry : entries) {
			published = published && setNumber(calibration, entry.name, entry.fixed);
		}
		published = published && setProperty("backlight-calibration-parameters", calibration);
		OSSafeReleaseNULL(calibration);
		if (!published) return false;
		// Equally spaced slider samples in signed 16.16 nits.
		const int32_t marketing[] = {(int32_t)((kMinMilliNits / 1000) << 16), (int32_t)((kMidMilliNits / 1000) << 16),
		    (int32_t)((kMaxMilliNits / 1000) << 16)};
		OSData *table = OSData::withBytes(marketing, sizeof(marketing));
		published = table && setProperty("backlight-marketing-table", table) && setProperty("new-backlight-architecture", true);
		OSSafeReleaseNULL(table);
		return published;
	}

	void post(uint32_t duty, uint32_t fadeMs) {
		IOLockLock(lock);
		__atomic_store_n(&request, 1ull << 63 | (uint64_t)duty << 32 | fadeMs, __ATOMIC_RELEASE);
		IOLockWakeup(lock, &request, true);
		IOLockUnlock(lock);
	}

	void apply(uint32_t duty) {
		const int error = pwmSet(duty);
		sets++;
		if (error) {
			if (errors++ < 8 || error != lastError) {
				kprintf("COACHZ backlight: set duty %u failed: error 0x%x (%u errors)\n", duty, (unsigned)error, errors);
			}
			lastError = error;
			return;
		}
		applied = duty;
	}

	void settle(uint32_t target) {
		uint32_t duty = 0;
		const int error = pwmGet(&duty);
		if (!error) {
			readback = duty;
		}
		kprintf("COACHZ backlight: target %u applied %u readback %u (error 0x%x), %u sets %u errors\n", target, applied,
		    error ? 0 : duty, (unsigned)error, sets, errors);
	}

	// coachz-backlight=2: no CoreBrightness, a visible check of the EC path alone: a quarter, then back.
	void sweep() {
		const uint32_t initial = applied;
		apply(kMaxDuty / 4);
		IOSleep(1000);
		settle(kMaxDuty / 4);
		apply(initial);
		IOSleep(200);
		settle(initial);
	}

	void work() {
		uint32_t from = applied, to = applied, period = 0;
		uint64_t start = 0;
		bool moving = false;
		if (mode == 2) {
			sweep();
			return;
		}
		for (;;) {
			const uint64_t taken = __atomic_exchange_n(&request, 0, __ATOMIC_ACQ_REL);
			if (taken) {
				from = applied;
				to = (uint32_t)(taken >> 32) & 0xffff;
				period = (uint32_t)taken & 0xffff;
				start = mach_absolute_time();
				moving = true;
			}
			if (!moving) {
				IOLockLock(lock);
				while (!__atomic_load_n(&request, __ATOMIC_ACQUIRE)) {
					IOLockSleep(lock, &request, THREAD_UNINT);
				}
				IOLockUnlock(lock);
				continue;
			}
			const uint32_t duty = fadeDuty(from, to, elapsedMs(start), period);
			if (duty != applied) {
				apply(duty);
			}
			// A failed step waits for the next request instead of repeating; a finished one is read back once.
			if (duty == to || applied != duty) {
				moving = false;
				if (!__atomic_load_n(&request, __ATOMIC_ACQUIRE)) {
					settle(to);
				}
			}
			IOSleep(kStepMs);
		}
	}

	static void thread(void *argument, wait_result_t) {
		CoachzBacklight *self = static_cast<CoachzBacklight *>(argument);
		self->work();
		self->release();
	}

public:
	// The original AppleARMBacklight methods drive Apple's own backlight hardware: none of them run here.
	bool init(OSDictionary *properties = nullptr) override {
		return IOService::init(properties);
	}

	bool start(IOService *provider) override {
		if (!PE_parse_boot_argn("coachz-backlight", &mode, sizeof(mode)) || (mode != 1 && mode != 2) ||
		    !IOService::start(provider)) {
			return false;
		}
		lock = IOLockAlloc();
		uint32_t duty = 0;
		const int error = pwmGet(&duty);
		// The boot probe leaves 80% (52428); a duty below the floor is raised to it by the first request only.
		applied = readback = error ? 52428 : duty;
		kprintf("COACHZ backlight: initial duty %u (error 0x%x), mode %u\n", applied, (unsigned)error, mode);
		const bool control = mode == 1;
		bool ready = lock && publish(milliNitsForDuty(applied)) &&
		    (!control || (publishCalibration() && setProperty("backlight-control", true)));
		thread_t worker = nullptr;
		if (ready) {
			retain();   // the worker's reference
			ready = kernel_thread_start(thread, this, &worker) == KERN_SUCCESS;
			if (!ready) {
				release();
			}
		}
		if (!ready) {
			IOService::stop(provider);
			return false;
		}
		thread_deallocate(worker);
		setName("CoachzPanelBacklight");
		return true;
	}

	IOReturn setProperties(OSObject *properties) override {
		OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
		if (!dictionary) {
			return kIOReturnBadArgument;
		}
		OSObject *object = dictionary->getObject("brightness-nits");
		const bool fixedNits = object != nullptr;
		if (!object) {
			object = dictionary->getObject("brightness");
		}
		if (!object) {
			return IOService::setProperties(properties);
		}
		OSNumber *number = OSDynamicCast(OSNumber, object);
		if (!number || (!fixedNits && number->unsigned32BitValue() > kMaxLevel)) {
			return kIOReturnBadArgument;
		}
		uint32_t milliNits = fixedNits ? milliNitsForFixed((int32_t)number->unsigned32BitValue()) :
		    milliNitsForLevel(number->unsigned32BitValue());
		if (milliNits < kMinMilliNits) {
			milliNits = kMinMilliNits;
		}
		OSNumber *fade = OSDynamicCast(OSNumber, dictionary->getObject("FadePeriod"));
		post(dutyForMilliNits(milliNits), fade ? fadeMsForFixed(fade->unsigned32BitValue()) : 0);
		// The requested value, not a readback: the applied and read-back duty are in the log.
		return publish(milliNits) ? kIOReturnSuccess : kIOReturnNoMemory;
	}

	void free() override {
		if (lock) {
			IOLockFree(lock);
			lock = nullptr;
		}
		IOService::free();
	}

	IOWorkLoop *getWorkLoop() const override {
		return IOService::getWorkLoop();
	}

	IOReturn configureReport(IOReportChannelList *channels, IOReportConfigureAction action, void *result, void *destination) override {
		return IOService::configureReport(channels, action, result, destination);
	}

	IOReturn updateReport(IOReportChannelList *channels, IOReportUpdateAction action, void *result, void *destination) override {
		return IOService::updateReport(channels, action, result, destination);
	}

	IOReturn callPlatformFunction(const OSSymbol *name, bool wait, void *param1, void *param2, void *param3, void *param4) override {
		return IOService::callPlatformFunction(name, wait, param1, param2, param3, param4);
	}

	IOReturn message(UInt32 type, IOService *provider, void *argument) override {
		return IOService::message(type, provider, argument);
	}

	IOReturn setPowerState(unsigned long ordinal, IOService *device) override {
		return IOService::setPowerState(ordinal, device);
	}
};
OSDefineMetaClassAndStructors(CoachzBacklight, AppleARMBacklight)

extern "C" IOService *
coachz_backlight_allocate(void)
{
	return new CoachzBacklight;
}
