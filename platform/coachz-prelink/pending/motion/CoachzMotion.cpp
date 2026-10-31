// SPDX-License-Identifier: BSD-2-Clause
// Chrome EC sensor service. Pending only: no active build integration.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include "coachz_motion.h"

extern "C" {
int coachz_ec_command(uint16_t, uint8_t, const void *, uint32_t, void *, uint32_t, uint32_t *);
void coachz_motion_start(void);
}

namespace {
constexpr unsigned kPollMs = 200; // Host polling, not a promise of sensor ODR or fresh hardware samples.
int command(void *, uint16_t cmd, uint8_t version, const void *args, uint32_t size,
    void *reply, uint32_t room, uint32_t *length) {
    return coachz_ec_command(cmd, version, args, size, reply, room, length);
}
const cm_bus bus = {nullptr, command};

bool number(OSDictionary *dictionary, const char *key, uint64_t value) {
    OSNumber *n = OSNumber::withNumber(value, 64);
    bool ok = n && dictionary->setObject(key, n);
    OSSafeReleaseNULL(n);
    return ok;
}
bool text(OSDictionary *dictionary, const char *key, const char *value) {
    OSString *s = OSString::withCString(value);
    bool ok = s && dictionary->setObject(key, s);
    OSSafeReleaseNULL(s);
    return ok;
}
bool axes(OSDictionary *dictionary, const char *key, const int64_t *values) {
    OSArray *array = OSArray::withCapacity(3);
    bool ok = array != nullptr;
    for (unsigned i = 0; i < 3 && ok; ++i) {
        OSNumber *n = OSNumber::withNumber((uint64_t)values[i], 64);
        ok = n && array->setObject(n);
        OSSafeReleaseNULL(n);
    }
    ok = ok && dictionary->setObject(key, array);
    OSSafeReleaseNULL(array);
    return ok;
}
}

class CoachzMotion : public IOService {
    OSDeclareDefaultStructors(CoachzMotion)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *timer = nullptr;
    IOService *children[CM_MAX_SENSORS] = {};
    bool childPublished[CM_MAX_SENSORS] = {};
    cm_topology topology = {};
    uint64_t sequence = 0;
    unsigned attempts = 0;
    bool discovered = false, stopping = false;

    bool discover() {
        int error = cm_discover(&bus, &topology);
        if (error) {
            setProperty("EnumerationStatus", "query-error");
            setProperty("LastEnumerationError", (uint64_t)(int64_t)error, 64);
            printf("COACHZ motion: enumeration error %d attempt %u\n", error, attempts);
            return false;
        }
        // INFO can fail while the EC initializes sensors: retry before publishing children.
        for (unsigned i = 0; i < topology.scanned; ++i) if (topology.sensors[i].info_error) {
            setProperty("EnumerationStatus", "info-query-error");
            setProperty("LastEnumerationError", (uint64_t)(int64_t)topology.sensors[i].info_error, 64);
            printf("COACHZ motion: INFO sensor %u error %d attempt %u\n",
                i, topology.sensors[i].info_error, attempts);
            return false;
        }
        setProperty("CommandVersions", (uint64_t)topology.versions, 32);
        setProperty("ReportedSensorCount", (uint64_t)topology.reported, 32);
        setProperty("ScannedSensorCount", (uint64_t)topology.scanned, 32);
        setProperty("EnumerationStatus", topology.reported > topology.scanned ? "scan-limited" : "complete");
        for (unsigned i = 0; i < topology.scanned; ++i) {
            const cm_sensor &sensor = topology.sensors[i];
            IOService *child = new IOService;
            if (!child || !child->init() || !child->attach(this)) {
                OSSafeReleaseNULL(child);
                continue;
            }
            children[i] = child; // Retain the allocation until stop(), in addition to attachment.
            child->setName(sensor.type == CM_ACCEL ? "CoachzECAccelerometer" :
                sensor.type == CM_GYRO ? "CoachzECGyroscope" : "CoachzECUnclaimedSensor");
            child->setProperty("ECSensorID", (uint64_t)sensor.id, 32);
            child->setProperty("ECSensorType", (uint64_t)sensor.type, 32);
            child->setProperty("ECSensorLocation", (uint64_t)sensor.location, 32);
            child->setProperty("ECSensorChip", (uint64_t)sensor.chip, 32);
            child->setProperty("Coordinates", "EC-standard-frame-unmodified");
            child->setProperty("TimestampMeaning", "host-command-completion-not-sensor-time");
            child->setProperty("SupportedByThisDriver", cm_supported(&sensor) != 0);
            printf("COACHZ motion: sensor %u type %u location %u chip %u supported %d\n",
                sensor.id, sensor.type, sensor.location, sensor.chip, cm_supported(&sensor));
        }
        return true;
    }

    void refresh() {
        ++sequence;
        for (unsigned i = 0; i < topology.scanned; ++i) {
            IOService *child = children[i];
            if (!child) continue;
            cm_sample sample;
            cm_collect(&bus, &topology.sensors[i], &sample);
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
            OSDictionary *snapshot = OSDictionary::withCapacity(9);
            if (!snapshot) continue;
            bool ok = text(snapshot, "Status", cm_status_name(sample.status)) &&
                number(snapshot, "Error", (uint64_t)(int64_t)sample.error) &&
                number(snapshot, "HostSequence", sequence) && number(snapshot, "HostCompletionNS", ns) &&
                number(snapshot, "Range", (uint64_t)(int64_t)sample.range) &&
                number(snapshot, "ODRMilliHz", (uint64_t)(int64_t)sample.odr_millihz);
            if (sample.status == CM_AVAILABLE) {
                int64_t raw[3] = {sample.raw[0], sample.raw[1], sample.raw[2]};
                ok = ok && axes(snapshot, "RawXYZ", raw) && axes(snapshot, "ScaledXYZ", sample.scaled) &&
                    text(snapshot, "Units", topology.sensors[i].type == CM_ACCEL ? "micro-g" : "micro-degrees-per-second");
            }
            // One immutable dictionary replaces the entire prior snapshot. Failed reads
            // publish an error without old axes presented under a new timestamp.
            bool replaced = ok && child->setProperty("LatestSample", snapshot);
            snapshot->release();
            if (replaced && !childPublished[i]) {
                childPublished[i] = true;
                child->registerService(); // Initial state is visible before matching.
            }
            if (sequence == 1 || sequence % 50 == 0)
                printf("COACHZ motion: sensor %u %s error %d odr %d range %d raw %d/%d/%d host-seq %llu\n",
                    i, cm_status_name(sample.status), sample.error, sample.odr_millihz, sample.range,
                    sample.raw[0], sample.raw[1], sample.raw[2], (unsigned long long)sequence);
        }
    }

    static void tick(OSObject *owner, IOTimerEventSource *sender) {
        CoachzMotion *self = OSDynamicCast(CoachzMotion, owner);
        if (!self || self->stopping) return;
        if (!self->discovered) {
            ++self->attempts;
            self->discovered = self->discover();
            if (!self->discovered) {
                if (self->attempts < 5) sender->setTimeoutMS(2000);
                return;
            }
        }
        self->refresh();
        sender->setTimeoutMS(kPollMs);
    }

    static IOReturn quiesce(OSObject *owner, void *, void *, void *, void *) {
        CoachzMotion *self = OSDynamicCast(CoachzMotion, owner);
        if (!self) return kIOReturnBadArgument;
        self->stopping = true;
        if (self->timer) { self->timer->cancelTimeout(); self->timer->disable(); }
        return kIOReturnSuccess;
    }

public:
    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        loop = IOWorkLoop::workLoop();
        timer = IOTimerEventSource::timerEventSource(this, &CoachzMotion::tick);
        if (!loop || !timer || loop->addEventSource(timer) != kIOReturnSuccess) {
            OSSafeReleaseNULL(timer);
            OSSafeReleaseNULL(loop);
            IOService::stop(provider);
            return false;
        }
        setName("CoachzECMotion");
        setProperty("EnumerationStatus", "pending");
        setProperty("PollIntervalMS", (uint64_t)kPollMs, 32);
        setProperty("CoreMotionABI", "not-connected");
        timer->setTimeoutMS(1);
        return true;
    }

    void stop(IOService *provider) override {
        // runAction is public and waits for an active callback; closeGate is protected.
        if (loop) loop->runAction(&CoachzMotion::quiesce, this);
        if (loop && timer) loop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
        OSSafeReleaseNULL(loop);
        for (unsigned i = 0; i < CM_MAX_SENSORS; ++i) if (children[i]) {
            children[i]->terminate(kIOServiceRequired | kIOServiceSynchronous);
            OSSafeReleaseNULL(children[i]);
        }
        IOService::stop(provider);
    }
};
OSDefineMetaClassAndStructors(CoachzMotion, IOService)

extern "C" void coachz_motion_start(void) {
    static bool started;
    unsigned mode = 0;
    if (started || !PE_parse_boot_argn("coachz-motion", &mode, sizeof(mode)) || mode != 1) return;
    started = true;
    IOService *platform = IOService::getPlatform();
    CoachzMotion *motion = OSTypeAlloc(CoachzMotion);
    if (!platform || !motion || !motion->init()) { OSSafeReleaseNULL(motion); return; }
    if (!motion->attach(platform)) { motion->release(); return; }
    if (!motion->start(platform)) { motion->detach(platform); motion->release(); return; }
    motion->registerService();
    motion->release(); // The platform attachment keeps the running service.
}
