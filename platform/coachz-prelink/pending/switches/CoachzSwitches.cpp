// SPDX-License-Identifier: BSD-2-Clause
// Lid/tablet/base state provider. No power-policy or keyboard-injection side effects.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include "coachz_switches.h"

extern "C" {
int coachz_ec_command(uint16_t, uint8_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
void coachz_switches_start(void);
}

namespace {
int command(void *, uint16_t cmd, uint8_t version, const void *args, uint32_t size,
    void *reply, uint32_t room, uint32_t *length) {
    return coachz_ec_command(cmd, version, args, size, reply, room, length);
}
const cs_bus bus = {nullptr, command};
bool number(OSDictionary *dictionary, const char *key, uint64_t value) {
    OSNumber *n = OSNumber::withNumber(value, 64);
    bool ok = n && dictionary->setObject(key, n);
    OSSafeReleaseNULL(n);
    return ok;
}
}

class CoachzSwitches : public IOService {
    OSDeclareDefaultStructors(CoachzSwitches)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    cs_tracker tracker = {};
    uint64_t sequence = 0;
    bool stopping = false;

    bool refresh() {
        cs_sample sample;
        cs_collect(&bus, &sample);
        enum cs_notice notice = cs_track(&tracker, &sample);
        ++sequence;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        OSDictionary *state = OSDictionary::withCapacity(15);
        if (!state) return sample.status == CS_READY;
        OSString *status = OSString::withCString(cs_status_name(sample.status));
        bool ok = status && state->setObject("Status", status) &&
            number(state, "Error", (uint64_t)(int64_t)sample.error) &&
            number(state, "HostSequence", sequence) && number(state, "HostCompletionNS", ns) &&
            number(state, "CommandVersions", sample.versions) && number(state, "SupportedMask", sample.supported) &&
            number(state, "TransitionCount", tracker.transitions) &&
            state->setObject("Baseline", sample.baseline ? kOSBooleanTrue : kOSBooleanFalse);
        OSSafeReleaseNULL(status);
        if (sample.status == CS_READY) {
            ok = ok && number(state, "RawState", sample.raw) && number(state, "KnownState", sample.known) &&
                number(state, "ChangedMask", sample.changed);
            const struct { const char *key; uint32_t bit; } fields[] = {
                {"LidOpen", CS_LID_OPEN}, {"TabletMode", CS_TABLET_MODE},
                {"BaseAttached", CS_BASE_ATTACHED}, {"FrontProximity", CS_FRONT_PROXIMITY}};
            for (const auto &field : fields) if (sample.supported & field.bit)
                ok = ok && state->setObject(field.key, sample.known & field.bit ? kOSBooleanTrue : kOSBooleanFalse);
        }
        // A single immutable property prevents stale booleans being paired with a new valid timestamp.
        bool replaced = ok && setProperty("SwitchState", state);
        state->release();
        if (replaced && notice != CS_QUIET)
            messageClients(kIOMessageServicePropertyChange); // Interested clients re-read SwitchState.
        if (notice != CS_QUIET)
            printf("COACHZ switches: %s error %d supported 0x%x raw 0x%x changed 0x%x baseline %d transitions %llu\n",
                cs_status_name(sample.status), sample.error, sample.supported, sample.raw,
                sample.changed, sample.baseline, (unsigned long long)tracker.transitions);
        return sample.status == CS_READY;
    }

    static void tick(OSObject *owner, IOTimerEventSource *sender) {
        CoachzSwitches *self = OSDynamicCast(CoachzSwitches, owner);
        if (!self || self->stopping) return;
        bool ready = self->refresh();
        sender->setTimeoutMS(ready ? 200 : 5000); // Back off on unsupported or failing EC commands.
    }
    static IOReturn quiesce(OSObject *owner, void *, void *, void *, void *) {
        CoachzSwitches *self = OSDynamicCast(CoachzSwitches, owner);
        if (!self) return kIOReturnBadArgument;
        self->stopping = true;
        if (self->timer) { self->timer->cancelTimeout(); self->timer->disable(); }
        return kIOReturnSuccess;
    }

public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, &CoachzSwitches::tick);
        if (!loop || !timer || loop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer); OSSafeReleaseNULL(loop);
            IOService::stop(provider); return false;
        }
        setName("CoachzECPhysicalSwitches");
        setProperty("Policy", "state-provider-only");
        setProperty("PollIntervalMS", (uint64_t)200, 32);
        setProperty("TimestampMeaning", "host-command-completion");
        timer->setTimeoutMS(1);
        return true;
    }
    void stop(IOService *provider) override {
        if (loop) loop->runAction(&CoachzSwitches::quiesce, this);
        if (loop && timer) loop->removeEventSource(timer);
        OSSafeReleaseNULL(timer); OSSafeReleaseNULL(loop);
        IOService::stop(provider);
    }
};
OSDefineMetaClassAndStructors(CoachzSwitches, IOService)

extern "C" void coachz_switches_start(void) {
    static bool started;
    unsigned mode = 0;
    if (started || !PE_parse_boot_argn("coachz-switches", &mode, sizeof(mode)) || mode != 1) return;
    started = true;
    IOService *platform = IOService::getPlatform();
    CoachzSwitches *service = OSTypeAlloc(CoachzSwitches);
    if (!platform || !service || !service->init()) { OSSafeReleaseNULL(service); return; }
    if (!service->attach(platform)) { service->release(); return; }
    if (!service->start(platform)) { service->detach(platform); service->release(); return; }
    service->registerService();
    service->release(); // Platform attachment retains it.
}
