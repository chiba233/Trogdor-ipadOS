// SPDX-License-Identifier: BSD-2-Clause-Patent
// Starts UniXNU's AppleKeyStoreState service (coachz-aks=1): an AppleKeyStore that tells the unchanged libaks and MobileKeyBag there
// is no keybag, no passcode and nothing locked. It supplies no keys and protects nothing. Adapted from UniXNU-sheng
// package/KeyStoreProbe.cpp. Without it keybagd fails to load the system bag and PreBoard asks for a passcode that never existed.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
extern kmod_info_t g_kernel_kmod_info;
extern "C" void native_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);
extern "C" void proc_selfname(char *buf,int size);

static int aks_enabled=-1;
static volatile int32_t aks_last[128];
static volatile uint32_t aks_seen[128];

extern "C" {
void native_keyevents_open_refused(void) {kprintf("COACHZ AKS: open refused\n");}

bool native_aks_state_enabled(void) {
    if(aks_enabled<0) {
        unsigned value=0;
        aks_enabled=PE_parse_boot_argn("coachz-aks",&value,sizeof(value)) && value==1;
    }
    return aks_enabled==1;
}

// coachz-aks-sls overrides the reported lock state (0 unlocked, 3 MobileKeyBag's "no passcode") without rebuilding the kernel.
uint32_t native_aks_lock_state(void) {
    static int cached=-1;
    if(cached<0) {
        unsigned value=0;
        if(!PE_parse_boot_argn("coachz-aks-sls",&value,sizeof(value)) || value>7)value=0;
        cached=(int)value;
    }
    return (uint32_t)cached;
}

void native_aks_opened(void) {}

// One log line per selector and result, the first time it is seen: the persistent log then shows what the system still asks for.
void native_aks_call(uint32_t selector,int32_t result) {
    if(selector>=128 || (aks_seen[selector] && aks_last[selector]==result))return;
    aks_seen[selector]=1;aks_last[selector]=result;
    char name[32]={};proc_selfname(name,sizeof(name));
    kprintf("COACHZ AKS: %s selector %u -> 0x%x\n",name,selector,(uint32_t)result);
}

// Before launchd: keybagd is one of its first boot tasks.
void coachz_keystore_start(void) {
    static bool started;
    if(started || !native_aks_state_enabled())return;
    started=true;
    const OSSymbol *name=OSSymbol::withCString("AppleKeyStore");
    const bool absent=name && !OSMetaClass::getMetaClassWithName(name);
    OSSafeReleaseNULL(name);
    IOService *provider=IOService::getResourceService();
    void *handle=absent && provider?OSMetaClass::preModLoad(g_kernel_kmod_info.name):nullptr;
    if(!handle){kprintf("COACHZ AKS: not started, class absent %d\n",absent);return;}
    native_keyevents_static_init();
    const OSReturn meta=OSMetaClass::postModLoad(handle);
    IOService *service=meta?nullptr:native_keyevents_allocate();
    const bool attached=service && service->init() && service->attach(provider);
    const bool running=attached && service->start(provider);
    if(!running) {
        if(attached)service->detach(provider);
        OSSafeReleaseNULL(service);
    }
    kprintf("COACHZ AKS: AppleKeyStore state service meta 0x%x started %d, no passcode, nothing locked\n",meta,running);
}
}
