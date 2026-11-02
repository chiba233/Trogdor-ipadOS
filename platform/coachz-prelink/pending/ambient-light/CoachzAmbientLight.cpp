// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inactive EC LIGHT service and optional local brightness controller.
// Allocation alone starts nothing. No active source includes or calls this.
// It publishes diagnostic lux, NOT an Apple ALS HID/CoreBrightness provider.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMessage.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include "coachz_als.h"
#include "coachz_auto_brightness.h"

extern "C" int coachz_ec_command(uint16_t,uint8_t,const void *,uint32_t,void *,uint32_t,uint32_t *);

namespace {
int ec(void *,uint16_t command,uint8_t version,const void *request,uint32_t bytes,
    void *reply,uint32_t room,uint32_t *length) {
    return coachz_ec_command(command,version,request,bytes,reply,room,length);
}
uint64_t uptimeMs() {
    uint64_t ns=0;absolutetime_to_nanoseconds(mach_absolute_time(),&ns);return ns/1000000u;
}
bool number(OSDictionary *d,const char *key,uint64_t value) {
    OSNumber *n=OSNumber::withNumber(value,64);
    const bool ok=n && d->setObject(key,n);OSSafeReleaseNULL(n);return ok;
}
bool text(OSDictionary *d,const char *key,const char *value) {
    OSString *s=OSString::withCString(value);
    const bool ok=s && d->setObject(key,s);OSSafeReleaseNULL(s);return ok;
}
}

class CoachzAmbientLight : public IOService {
    OSDeclareDefaultStructors(CoachzAmbientLight)
    IOWorkLoop *loop=nullptr;
    IOCommandGate *gate=nullptr;
    IOTimerEventSource *timer=nullptr;
    IOService *backlight=nullptr;
    ca_topology topology={}; ca_sensor sensor={};
    ca_bus bus={nullptr,ec}; cab_policy policy={};
    bool selected=false,stopping=false,controlGranted=false;
    bool gateAdded=false,timerAdded=false;
    int enumerationError=CA_ABSENT;
    uint32_t setError=0,accepted=0;
    uint64_t generation=0;

    bool currentLevel(uint32_t &value) {
        OSObject *object=backlight->copyProperty("IODisplayParameters");
        OSDictionary *parameters=OSDynamicCast(OSDictionary,object);
        OSDictionary *entry=parameters?OSDynamicCast(OSDictionary,parameters->getObject("BrightnessMilliNits")):nullptr;
        OSNumber *n=entry?OSDynamicCast(OSNumber,entry->getObject("value")):nullptr;
        const bool ok=n && n->unsigned64BitValue()>=CAB_MIN && n->unsigned64BitValue()<=CAB_MAX;
        if(ok)value=n->unsigned32BitValue();
        OSSafeReleaseNULL(object);return ok;
    }
    IOReturn requestLevel(const cab_action &action) {
        OSDictionary *d=OSDictionary::withCapacity(2);
        if(!d)return kIOReturnNoMemory;
        const uint64_t nits=((uint64_t)action.millinits*65536u+500u)/1000u;
        const uint64_t fade=(uint64_t)action.fade_ms*65536u/1000u;
        const bool ok=number(d,"brightness-nits",nits) && number(d,"FadePeriod",fade);
        const IOReturn result=ok?backlight->setProperties(d):kIOReturnNoMemory;
        d->release();return result;
    }
    void publish(const ca_sample &sample,uint64_t now) {
        OSDictionary *d=OSDictionary::withCapacity(18);
        if(!d)return;
        bool ok=text(d,"Status",selected?ca_status_name(sample.status):"no-selected-light-sensor") &&
            number(d,"EnumerationError",(uint64_t)(int64_t)enumerationError) &&
            number(d,"Error",(uint64_t)(int64_t)sample.error) &&
            number(d,"HostSequence",++generation) && number(d,"HostCompletionMs",now) &&
            number(d,"ReportedSensors",topology.reported) && number(d,"LightSensors",topology.light_count) &&
            number(d,"ODRMilliHz",sample.odr_millihz) && number(d,"BacklightRequestError",setError) &&
            number(d,"AcceptedBacklightRequests",accepted) && number(d,"ManualHoldUntilMs",policy.hold_until_ms) &&
            d->setObject("LocalAutoBrightnessEnabled",policy.enabled?kOSBooleanTrue:kOSBooleanFalse) &&
            d->setObject("LocalControlGranted",controlGranted?kOSBooleanTrue:kOSBooleanFalse) &&
            d->setObject("DisplayAwake",policy.awake?kOSBooleanTrue:kOSBooleanFalse);
        if(selected) ok=ok && number(d,"ECSensorID",sensor.id) && number(d,"ECSensorChip",sensor.chip);
        if(sample.status==CA_READY) ok=ok && number(d,"IlluminanceLux",sample.lux) &&
            number(d,"FilteredMilliLux",policy.filtered_millilux);
        if(ok && setProperty("AmbientLightState",d))messageClients(kIOMessageServicePropertyChange);
        d->release();
    }
    void refresh() {
        ca_sample sample={};
        if(!policy.awake) {
            sample.status=CA_PAUSED;
            const uint64_t now=uptimeMs();
            (void)cab_update(&policy,now,0,0,0);
            publish(sample,now);timer->setTimeoutMS(5000);return;
        }
        if(!selected) {
            enumerationError=ca_discover(&bus,&topology);
            if(!enumerationError) enumerationError=ca_select(&topology,&sensor);
            selected=enumerationError==0;
        }
        if(selected)ca_collect(&bus,&sensor,&sample);
        else sample.error=enumerationError;
        const uint64_t now=uptimeMs(); // completion of the EC queries, not their start
        uint32_t current=0;
        // Local policy needs at least 1 Hz. A slower stream may still be
        // reported diagnostically, but never drives the panel from old lux.
        const bool usable=selected && sample.status==CA_READY && sample.odr_millihz>=1000u &&
            controlGranted && currentLevel(current);
        const cab_action action=cab_update(&policy,now,usable,sample.lux,current);
        if(action.apply) {
            const IOReturn result=requestLevel(action);
            setError=(uint32_t)result;
            if(result==kIOReturnSuccess) {cab_commit(&policy,&action);++accepted;}
            else {
                // Backlight accepts asynchronously. Never flood retries or
                // call it "applied" on the basis of this method's return.
                cab_enable(&policy,0,policy.awake);
            }
        }
        publish(sample,now);
        timer->setTimeoutMS(selected?500:5000);
    }
    static void tick(OSObject *owner,IOTimerEventSource *) {
        CoachzAmbientLight *self=OSDynamicCast(CoachzAmbientLight,owner);
        if(self && !self->stopping)self->refresh();
    }
    static IOReturn configure(OSObject *owner,void *argument,void *,void *,void *) {
        CoachzAmbientLight *self=OSDynamicCast(CoachzAmbientLight,owner);
        OSDictionary *d=OSDynamicCast(OSDictionary,(OSObject *)argument);
        if(!self || !d || self->stopping)return kIOReturnNotReady;
        const char *keys[]={"LocalAutoBrightnessEnabled","LocalControlGranted","DisplayAwake","ManualBrightnessChanged"};
        bool values[]={self->policy.enabled!=0,self->controlGranted,self->policy.awake!=0,false};
        bool present[4]={};
        for(unsigned i=0;i<4;++i) {
            OSObject *o=d->getObject(keys[i]);if(!o)continue;
            OSBoolean *b=OSDynamicCast(OSBoolean,o);if(!b)return kIOReturnBadArgument;
            present[i]=true;values[i]=b->isTrue();
        }
        if(!present[0] && !present[1] && !present[2] && !present[3])return kIOReturnUnsupported;
        // The integration owner grants the local controller sole authority;
        // it cannot run alongside a native CoreBrightness ALS controller.
        if(!values[1]) {
            if(present[0] && values[0])return kIOReturnNotPermitted;
            values[0]=false; // revoking the lease also stops local control
        }
        const bool changed=values[0]!=(self->policy.enabled!=0) || values[2]!=(self->policy.awake!=0) || values[1]!=self->controlGranted;
        self->controlGranted=values[1];
        if(changed)cab_enable(&self->policy,values[0],values[2]);
        if(present[3] && values[3])cab_manual(&self->policy,uptimeMs());
        self->timer->setTimeoutMS(1);
        return kIOReturnSuccess;
    }
    static IOReturn quiesce(OSObject *owner,void *,void *,void *,void *) {
        CoachzAmbientLight *self=OSDynamicCast(CoachzAmbientLight,owner);
        if(!self)return kIOReturnBadArgument;
        self->stopping=true;cab_enable(&self->policy,0,0);
        if(self->timer) {self->timer->cancelTimeout();self->timer->disable();}
        return kIOReturnSuccess;
    }
    void releaseSources() {
        if(timer){timer->cancelTimeout();timer->disable();}
        if(timerAdded){loop->removeEventSource(timer);timerAdded=false;}
        if(gateAdded){loop->removeEventSource(gate);gateAdded=false;}
        OSSafeReleaseNULL(timer);OSSafeReleaseNULL(gate);OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(backlight);
    }
public:
    bool start(IOService *provider) override {
        if(!provider || !provider->metaCast("AppleARMBacklight") || !IOService::start(provider))return false;
        backlight=provider;backlight->retain();
        loop=IOWorkLoop::workLoop();
        gate=IOCommandGate::commandGate(this);
        timer=IOTimerEventSource::timerEventSource(this,tick);
        if(loop && gate && loop->addEventSource(gate)==kIOReturnSuccess)gateAdded=true;
        if(gateAdded && timer && loop->addEventSource(timer)==kIOReturnSuccess)timerAdded=true;
        if(!timerAdded){releaseSources();IOService::stop(provider);return false;}
        cab_enable(&policy,0,1); // local automatic brightness starts disabled
        setName("CoachzECAmbientLight");
        setProperty("IlluminanceUnits","lux");
        setProperty("TimestampMeaning","host-read-completion-not-sensor-capture-time");
        setProperty("ControlBackend","local-opt-in-policy-not-native-CoreBrightness-ALS");
        registerService();timer->setTimeoutMS(1);return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *d=OSDynamicCast(OSDictionary,properties);
        if(!d || !gate)return kIOReturnBadArgument;
        return gate->runAction(configure,d);
    }
    void stop(IOService *provider) override {
        if(gate && gateAdded)gate->runAction(quiesce);
        releaseSources();IOService::stop(provider);
    }
    void free() override {releaseSources();IOService::free();}
};
OSDefineMetaClassAndStructors(CoachzAmbientLight,IOService)

// Future owner must register this translation unit's late metaclass first,
// then init/attach/start the object under the existing backlight service.
// No startup hook, boot argument, patch-list entry or constructor call added.
extern "C" IOService *coachz_ambient_light_allocate(void) {
    return new CoachzAmbientLight;
}
