// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts ShengHidInput's touch digitizer (an IOHIDDevice of the original IOHIDFamily) below IOHIDResource, once the
// touchscreen answers on I2C (bsd/dev/coachz_touch.c). Adapted from UniXNU-sheng package/HidInputProbe.cpp, whose hardware
// findings it keeps: backboardd only takes digitizer events from a service it classifies as the built-in touch screen.
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern "C" {
char coachz_hid_status[160];
// Refreshed each second: the event driver below the digitizer, its user clients (backboardd's connection), and the reports.
char coachz_hid_live[160];
extern volatile uint32_t native_touch_hid_reports;
volatile uint32_t native_hid_input_events[8];
volatile int native_hid_input_results[8]={-1,-1,-1,-1,-1,-1,-1,-1};
}

static IORegistryEntry *findEventDriver(IORegistryEntry *parent,unsigned depth) {
    if(!depth)return nullptr;
    OSIterator *it=parent->getChildIterator(gIOServicePlane);if(!it)return nullptr;
    IORegistryEntry *found=nullptr;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *entry=OSDynamicCast(IORegistryEntry,child);
        if(!entry)continue;
        if(entry->metaCast("IOHIDEventDriver")){found=entry;break;}
        if((found=findEventDriver(entry,depth-1)))break;
    }
    it->release();return found;
}

extern "C" {
// No keyboard or Home presses are synthesized here; the device is touch only.
bool native_hid_input_requested(void) {return false;}
// No button driver: a weak import still needs a definition in this statically linked kernel. The touch-only device never calls it.
void native_buttons_set_sink(void (*)(void *,uint32_t),void *) {}

void native_hid_input_observed(unsigned event,int result) {
    if(event<8){native_hid_input_events[event]=native_hid_input_events[event]+1;native_hid_input_results[event]=result;}
}

// Called by the device's one-second timer. Once IOHIDEventDriver has matched, publish on the event service and its interface what
// backboardd classifies a built-in touch screen by (BKHIDDirectTouchEventProcessor drops digitizer events from other services):
// Built-In, DisplayIntegrated and the Digitizer/Touch Screen primary usage. Set once: every change notifies its user clients.
static unsigned countUserClients(IORegistryEntry *entry) {
    unsigned count=0;OSIterator *it=entry->getChildIterator(gIOServicePlane);if(!it)return 0;
    while(OSObject *child=it->getNextObject()) {
        IORegistryEntry *next=OSDynamicCast(IORegistryEntry,child);
        if(next && next->metaCast("IOUserClient"))count++;
    }
    it->release();return count;
}

__attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
    static IORegistryEntry *publishedFor;
    IORegistryEntry *driver=findEventDriver(device,4);
    snprintf(coachz_hid_live,sizeof(coachz_hid_live),
        "K43 hid live: driver %s user clients %u built-in %d usage %d | reports %u handleReport n %u last 0x%x",
        driver?driver->getMetaClass()->getClassName():"none",driver?countUserClients(driver):0,
        driver?driver->getProperty("Built-In")!=nullptr:-1,driver?driver->getProperty("PrimaryUsagePage")!=nullptr:-1,
        native_touch_hid_reports,native_hid_input_events[6],native_hid_input_results[6]);
    if(!driver || publishedFor==driver || !device->getProperty("Built-In"))return;
    publishedFor=driver;
    driver->setProperty("Built-In",kOSBooleanTrue);driver->setProperty("DisplayIntegrated",kOSBooleanTrue);
    driver->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);driver->setProperty("PrimaryUsage",(unsigned long long)4,32);
    if(IORegistryEntry *interface=driver->getParentEntry(gIOServicePlane)) {
        interface->setProperty("Built-In",kOSBooleanTrue);interface->setProperty("DisplayIntegrated",kOSBooleanTrue);
        interface->setProperty("PrimaryUsagePage",(unsigned long long)0x0d,32);interface->setProperty("PrimaryUsage",(unsigned long long)4,32);
    }
}

// IOHIDFamily's event driver for this digitizer, matched as soon as the device is published. Its stock digitizer personality has
// IOMatchDefer, so it waits for the IOKit daemon; driverkitd then adds the IOProbeScore 1 personality of
// com.apple.AppleUserHIDDrivers.dext, whose AppleUserHIDEventService takes the device and waits up to 120 s
// (kIOUserServerCheckInTimeoutSecs) for a dext process. On hardware that process never started, so backboardd had nothing to open.
// With the kernel driver started first the default match category is taken and the dext personality is skipped.
static bool addTouchPersonality(void) {
    OSDictionary *personality=OSDictionary::withCapacity(7),*pair=OSDictionary::withCapacity(2);
    OSArray *pairs=OSArray::withCapacity(1),*drivers=OSArray::withCapacity(1);
    OSNumber *page=OSNumber::withNumber(0x0d,32),*usage=OSNumber::withNumber(4,32),*score=OSNumber::withNumber(1000,32);
    OSString *bundle=OSString::withCString("com.apple.iokit.IOHIDFamily"),*driver=OSString::withCString("IOHIDEventDriver");
    OSString *provider=OSString::withCString("IOHIDInterface"),*transport=OSString::withCString("SPI"),*behavior=OSString::withCString("");
    bool added=false;
    if(personality && pair && pairs && drivers && page && usage && score && bundle && driver && provider && transport && behavior) {
        pair->setObject("DeviceUsagePage",page);pair->setObject("DeviceUsage",usage);pairs->setObject(pair);
        personality->setObject("CFBundleIdentifier",bundle);personality->setObject("IOClass",driver);
        personality->setObject("IOProviderClass",provider);personality->setObject("IOProbeScore",score);
        personality->setObject("Transport",transport);personality->setObject("HIDDefaultBehavior",behavior);
        personality->setObject("DeviceUsagePairs",pairs);drivers->setObject(personality);
        added=gIOCatalogue->addDrivers(drivers,false);
    }
    OSSafeReleaseNULL(personality);OSSafeReleaseNULL(pair);OSSafeReleaseNULL(pairs);OSSafeReleaseNULL(drivers);
    OSSafeReleaseNULL(page);OSSafeReleaseNULL(usage);OSSafeReleaseNULL(score);
    OSSafeReleaseNULL(bundle);OSSafeReleaseNULL(driver);OSSafeReleaseNULL(provider);OSSafeReleaseNULL(transport);OSSafeReleaseNULL(behavior);
    return added;
}

// Runs once on the touch thread after the controller answered. Each step leaves its result in coachz_hid_status.
void coachz_hid_touch_start(void) {
    static bool started;
    if(started)return;
    started=true;
    int family=OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
    int driver=family?-1:OSKext::loadKextWithIdentifier("com.apple.iokit.IOHIDEventDriver",true,false,kOSKextExcludeNone,kOSKextExcludeNone,nullptr);
    const OSSymbol *name=OSSymbol::withCString("IOHIDDevice");
    const OSMetaClass *meta=name?OSMetaClass::getMetaClassWithName(name):nullptr;
    unsigned size=meta?meta->getClassSize():0;
    OSSafeReleaseNULL(name);
    OSDictionary *match=IOService::serviceMatching("IOHIDResource");
    IOService *resource=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
    OSSafeReleaseNULL(match);
    int registered=-1;bool attached=false,running=false;
    const bool personality=addTouchPersonality();
    // The digitizer's metaclass is registered now rather than at boot: IOHIDFamily's own metaclasses must exist first.
    if(resource && size==208) {
        void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if(handle){native_hid_static_init();registered=OSMetaClass::postModLoad(handle);}
    }
    if(!registered) {
        IOService *touch=native_hid_touch_allocate();
        attached=touch && touch->metaCast("IOHIDDevice") && touch->init() && touch->attach(resource);
        running=attached && touch->start(resource);
        if(!running) {
            if(attached)touch->detach(resource);
            OSSafeReleaseNULL(touch);
        }
    }
    snprintf(coachz_hid_status,sizeof(coachz_hid_status),"K42 hid: family %d driver %d IOHIDDevice size %u resource %d meta %d personality %d attached %d started %d",
        family,driver,size,resource!=nullptr,registered,personality,attached,running);
    OSSafeReleaseNULL(resource);
}
}
