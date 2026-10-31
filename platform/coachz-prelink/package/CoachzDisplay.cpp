// SPDX-License-Identifier: BSD-2-Clause-Patent
// Coachz factory framebuffer -> original iPadOS MobileFramebuffer Legacy lifecycle.
// Lifecycle and scale policy adapted from UniXNU-sheng LegacyDisplayProbe.cpp.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSData.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>
#include <kern/task.h>
#include <sys/proc.h>
#include <sys/errno.h>
extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void coachz_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);
static IOService *activeDisplay;
static IOPlatformDevice *activeNub;
static unsigned stage,registrations,clients;
static uint64_t frames,retirements,transfers,vsyncs,rejections;
static IOReturn lastTransfer=kIOReturnNotReady;
extern "C" {
bool native_legacy_prepare_scanout(void) { return activeDisplay != nullptr; }
void native_legacy_swap_stage(unsigned) {}
void native_legacy_pm_observed(unsigned event,IOReturn status,unsigned long ordinal) {
    kprintf("COACHZ display: PM event %u status 0x%x state %lu\n",event,status,ordinal);
}
void native_legacy_client_enter(task_t task,uint32_t type) {
    clients++;
    if(clients<=12) kprintf("COACHZ display: client %u pid %d type %u\n",clients,task?proc_pid((proc_t)get_bsdtask_info(task)):0,type);
}
void native_legacy_client_observed(IOReturn status,IOUserClient *) {
    if(clients<=12) kprintf("COACHZ display: client open 0x%x\n",status);
}
void native_legacy_registration_observed(IOService *) {
    registrations++;kprintf("COACHZ display: original framebuffer registered %u\n",registrations);
}
void native_legacy_vsync_observed(unsigned event,unsigned hz) {
    if(event==1) vsyncs++;
    else if(event==4 || event==5) kprintf("COACHZ display: refresh event %u timer %u Hz\n",event,hz);
}
bool native_legacy_gop_mapping_observed(IOService *,IOMemoryMap *map,uint32_t width,uint32_t height) {
    PE_Video video={};
    bool valid=!PE_current_console(&video) && map && map->getPhysicalAddress()==video.v_baseAddr &&
        map->getLength()==video.v_rowBytes*video.v_height && width==video.v_width && height==video.v_height;
    kprintf("COACHZ display: real scanout map %d mode %ux%u stride %lu\n",valid,width,height,video.v_rowBytes);
    return valid;
}
void native_legacy_frame_observed(unsigned kind,uint64_t count,uint64_t,uint64_t,uint64_t,uint64_t) {
    frames++;
    if(frames<=8) kprintf("COACHZ display: frame %llu kind %u nonzero %llu\n",frames,kind,count);
}
void native_legacy_frame_colors_observed(const uint32_t *,const uint64_t *,uint64_t) {}
void native_legacy_swap_validation_observed(bool accepted,unsigned reason,const void *,const void *) {
    if(!accepted && ++rejections<=8) kprintf("COACHZ display: swap rejected reason %u\n",reason);
}
void native_legacy_surface_rejected(uint64_t width,uint64_t height,uint32_t format,uint32_t planes,uint64_t stride,uint64_t allocation,uint64_t bytes) {
    if(rejections<=8) kprintf("COACHZ display: rejected surface %llux%llu format %x planes %u stride %llu size %llu/%llu\n",width,height,format,planes,stride,allocation,bytes);
}
static uint64_t counter_read() { uint64_t value;__asm__ volatile("isb\n\tmrs %0, cntpct_el0":"=r"(value));return value; }
static uint64_t counter_hz() { uint64_t value;__asm__ volatile("mrs %0, cntfrq_el0":"=r"(value));return value?value:1; }
// Copy cost against the interval between copies: a slow copy and a slow producer look alike on the screen.
// The periodic report reads frames copied, the last copy in us and the gap before it in ms; for its window, the slowest
// and fastest copy, the longest gap and a mask of the cores that copied (it resets those and keeps [8], the count it last saw).
extern "C" { volatile uint64_t coachz_frame_stats[9]; }
void native_legacy_copy_timing_observed(uint64_t ticks,uint64_t bytes) {
    static uint64_t copies,previous;
    uint64_t now=counter_read(),hz=counter_hz();
    copies++;
    if(copies<=8 || !(copies%32)) kprintf("COACHZ display: copy %llu took %llu us for %llu bytes, %llu ms since previous\n",
        copies,ticks*1000000/hz,bytes,previous?(now-previous)*1000/hz:0);
    uint64_t us=ticks*1000000/hz,gap=previous?(now-previous)*1000/hz:0,mpidr;
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    const uint64_t core=(mpidr>>8)&0xff;   // SC7180 numbers its cores in Affinity1
    coachz_frame_stats[0]=copies;coachz_frame_stats[1]=us;coachz_frame_stats[2]=gap;coachz_frame_stats[6]=core;
    if(us>coachz_frame_stats[3])coachz_frame_stats[3]=us;
    if(us<coachz_frame_stats[4])coachz_frame_stats[4]=us;
    if(gap>coachz_frame_stats[5])coachz_frame_stats[5]=gap;
    if(core<64)coachz_frame_stats[7]|=1ull<<core;
    previous=now;
}
void native_legacy_transfer_observed(IOReturn status,const void *,uint64_t bytes) {
    lastTransfer=status;transfers++;
    if(transfers<=8 || status) kprintf("COACHZ display: CPU transfer %llu status 0x%x bytes %llu\n",transfers,status,bytes);
}
void native_legacy_retirement_observed(IOService *) { retirements++; }
bool native_legacy_gpu_offer(void *,IOMemoryDescriptor *,uint64_t,uint32_t,uint32_t) { return false; }
void native_legacy_gpu_fallback_observed(int status) { kprintf("COACHZ display: GPU offer unsupported %d\n",status); }
__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot,void *,void *caller) {
    panic("Coachz display: unqualified slot %u caller %p stage %u registrations %u clients %u frames %llu transfers %llu status %x retire %llu vsync %llu",
        slot,caller,stage,registrations,clients,frames,transfers,lastTransfer,retirements,vsyncs);
}
static bool word(IORegistryEntry *entry,const char *key,uint32_t value) {
    OSObject *old=entry->getProperty(key);
    if(old) {
        OSData *data=OSDynamicCast(OSData,old);uint32_t actual=0;
        if(!data || data->getLength()!=4)return false;
        memcpy(&actual,data->getBytesNoCopy(),4);return actual==value;
    }
    OSData *data=OSData::withBytes(&value,4);
    bool ok=data && entry->setProperty(key,data);OSSafeReleaseNULL(data);return ok;
}
// Which core runs the system and roughly how fast: a fixed count loop timed by the generic counter.
static void cpu_probe() {
    uint64_t midr,mpidr,count=20000000,start,end;
    __asm__ volatile("mrs %0, midr_el1":"=r"(midr));
    __asm__ volatile("mrs %0, mpidr_el1":"=r"(mpidr));
    start=counter_read();
    __asm__ volatile("1: subs %0, %0, #1\n\tb.ne 1b":"+r"(count)::"cc");
    end=counter_read();
    uint64_t us=(end-start)*1000000/counter_hz();
    kprintf("COACHZ cpu: midr 0x%llx mpidr 0x%llx counter %llu Hz, 20M-iteration loop %llu us (%llu per us)\n",
        midr,mpidr,counter_hz(),us,us?20000000/us:0);
}
int coachz_display_start(void) {
    int enabled=0;
    if(!PE_parse_boot_argn("coachz-display",&enabled,sizeof(enabled)) || enabled!=1)return 0;
    cpu_probe();
    if(stage)return stage==10?0:EIO;
    stage=1;
    kern_return_t surface=OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
    kern_return_t family=OSKextLoadKextWithIdentifier("com.apple.iokit.IOMobileGraphicsFamily");
    kprintf("COACHZ display: original IOSurface 0x%x MobileGraphics 0x%x\n",surface,family);
    if(surface || family)return EIO;
    OSDictionary *match=IOService::nameMatching("IOCoreSurfaceRoot");
    IOService *root=match?IOService::waitForMatchingService(match,5000000000ULL):nullptr;
    bool ready=root!=nullptr;OSSafeReleaseNULL(root);OSSafeReleaseNULL(match);
    if(!ready)return ENODEV;
    stage=2;
    void *handle=OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if(!handle)return EIO;
    coachz_display_static_init();
    OSReturn meta=OSMetaClass::postModLoad(handle);
    kprintf("COACHZ display: late class registration 0x%x\n",meta);
    if(meta)return EIO;
    PE_Video video={};
    if(PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height || video.v_depth!=32 ||
       video.v_rotate || video.v_width>16384 || video.v_height>16384 || video.v_rowBytes<video.v_width*4 ||
       video.v_rowBytes>UINT64_MAX/video.v_height)return EINVAL;
    IORegistryEntry *chosen=IORegistryEntry::fromPath("/chosen",gIODTPlane);
    bool scale=chosen && word(chosen,"display-scale",2) && word(chosen,"display-rotation",video.v_rotate);
    OSSafeReleaseNULL(chosen);if(!scale)return EINVAL;
    IOService *display=native_legacy_display_allocate();
    if(!display || !display->metaCast("IOMobileFramebufferLegacy") || !display->metaCast("IOMobileFramebuffer"))return EIO;
    stage=3;kprintf("COACHZ display: calling original init\n");
    if(!display->init())return EIO;
    IOPlatformDevice *nub=new IOPlatformDevice;
    IOService *parent=IOService::getResourceService();
    IODeviceMemory *memory=IODeviceMemory::withRange(video.v_baseAddr,video.v_rowBytes*video.v_height);
    OSArray *ranges=OSArray::withCapacity(1);
    if(!nub || !parent || !memory || !ranges || !nub->init())return ENOMEM;
    ranges->setObject(memory);nub->setDeviceMemory(ranges);nub->setName("CoachzFirmwareFramebuffer");
    OSSafeReleaseNULL(ranges);OSSafeReleaseNULL(memory);
    if(!nub->attach(parent) || !nub->start(parent))return EIO;
    stage=4;
    IORegistryEntry *armio=IORegistryEntry::fromPath("/arm-io",gIODTPlane);
    const char type[]="display-subsystem";
    OSData *data=OSData::withBytes(type,sizeof(type));
    bool attached=armio && data && nub->setProperty("device_type",data);
    nub->setName("coachz-display",gIODTPlane);
    attached=attached && nub->attachToParent(armio,gIODTPlane);
    OSSafeReleaseNULL(data);OSSafeReleaseNULL(armio);
    if(!attached)return ENODEV;
    SInt32 score=0;kprintf("COACHZ display: calling original probe\n");
    if(display->probe(nub,&score)!=display || !display->attach(nub))return EIO;
    stage=5;activeDisplay=display;activeNub=nub;
    kprintf("COACHZ display: calling original start\n");
    if(!display->start(nub))return EIO;
    stage=10;
    kprintf("COACHZ display: original Legacy active, mode %lux%lu scale 2 registered %u\n",video.v_width,video.v_height,registrations);
    return registrations?0:EIO;
}
}
