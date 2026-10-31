// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU drivers/RamBlock/NativeRamBlock.cpp, Copyright (c) 2026, UniXNU contributors.
// The IOKit side of UniXNU's RamBlock (same class, personality and exported names), with the
// USB mass storage device behind it instead of memory. Read-only: the kernel reports the medium
// as write protected and refuses writes, whatever a filesystem asks.
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <sys/param.h>
#include <pexpert/pexpert.h>

extern "C" {
int coachz_usb_open(uint64_t *blocks);
int coachz_usb_read(uint64_t block, uint32_t count, void **data);
uint32_t coachz_usb_max_blocks(void);
void coachz_usb_report(void);
int coachz_usb_root_device(dev_t device);
int coachz_usb_volume_device(const char *uuid, char *path, size_t size);
int coachz_data_publish(void);
int coachz_data_volume_device(char *path, size_t size);
int mdevgetrange(int devid, uint64_t *base, uint64_t *size);
// RamBlock's interface to the kernel patches; nothing here is a RAM disk or carries a seed.
volatile uint32_t native_ram_block_ready = 0;
volatile uint64_t native_ram_block_bytes = 0;
volatile uint64_t native_ram_block_reads = 0;
volatile uint64_t native_ram_block_writes = 0;
volatile uint64_t native_ram_block_failures = 0;
volatile uint64_t native_ram_block_resident_bytes = 0;
volatile uint32_t native_ram_block_sparse = 0;
uint8_t native_ram_block_seed_sha256[32];
int native_ram_block_volume_device(const char *, dev_t *, char *, size_t);
int native_ram_block_gpu_addon(const uint8_t **, uint64_t *);
int native_ram_block_volume_seed(const char *, dev_t *, char *, size_t, uint64_t *, uint64_t *, uint8_t *);
}

class NativeRamBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeRamBlock)
  IOLock *gate = nullptr;
  uint64_t capacity = 0;
  bool online = false;
public:
  bool start(IOService *) override;
  void stop(IOService *) override;
  void free() override;
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Coachz"); }
  char *getProductString() override { return const_cast<char *>("USB mass storage"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("read-only bring-up"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors(NativeRamBlock, IOBlockStorageDevice)

bool NativeRamBlock::start(IOService *provider)
{
  if (!IOBlockStorageDevice::start(provider)) return false;
  if (!(gate = IOLockAlloc()) || !coachz_usb_open(&capacity) || !capacity) {
    kprintf("COACHZ USB: no storage, driver not started\n");
    return false;
  }
  native_ram_block_bytes = capacity * 512;
  online = true;
  native_ram_block_ready = 1;
  registerService();
  return true;
}

IOReturn NativeRamBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= capacity ||
      count > capacity - block || buffer->getLength() < count * 512) return kIOReturnBadArgument;
  if (buffer->getDirection() != kIODirectionIn) {
    __atomic_fetch_add(&native_ram_block_writes, 1, __ATOMIC_RELAXED);
    return kIOReturnNotWritable;
  }
  if (!gate) return kIOReturnNotReady;
  IOLockLock(gate);
  if (!online) { IOLockUnlock(gate); return kIOReturnNotReady; }
  const uint32_t most = coachz_usb_max_blocks();
  uint64_t done = 0;
  IOReturn status = kIOReturnSuccess;
  while (done < count) {
    uint32_t n = count - done > most ? most : static_cast<uint32_t>(count - done);
    void *data = nullptr;
    if (!coachz_usb_read(block + done, n, &data)) { status = kIOReturnIOError; break; }
    if (buffer->writeBytes(done * 512, data, n * 512ull) != n * 512ull) { status = kIOReturnIOError; break; }
    done += n;
  }
  __atomic_fetch_add(&native_ram_block_reads, 1, __ATOMIC_RELAXED);
  if (status) __atomic_fetch_add(&native_ram_block_failures, 1, __ATOMIC_RELAXED);
  IOLockUnlock(gate);
  completion->action(completion->target, completion->parameter, status, done * 512);
  return kIOReturnSuccess;
}

void NativeRamBlock::stop(IOService *provider)
{
  if (gate) { IOLockLock(gate); online = false; IOLockUnlock(gate); }
  IOBlockStorageDevice::stop(provider);
}

void NativeRamBlock::free()
{
  if (gate) { IOLockFree(gate); gate = nullptr; }
  IOBlockStorageDevice::free();
}

// The writable Data seed: the memory of the md0 RAM disk as an IOKit block device, because APFS
// mounts volumes through IOMedia and md0 has none. Read-write; changes are lost at reboot.
class CoachzDataBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(CoachzDataBlock)
  uint8_t *memory = nullptr;
  uint64_t capacity = 0;
public:
  bool setMemory(uint8_t *base, uint64_t bytes) { memory = base; capacity = bytes / 512; return memory && capacity; }
  bool start(IOService *provider) override { return IOBlockStorageDevice::start(provider) && (registerService(), true); }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Coachz"); }
  char *getProductString() override { return const_cast<char *>("Data seed"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("memory, lost at reboot"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = true; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
      IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!buffer || !completion || !completion->action || !count || block >= capacity ||
        count > capacity - block || buffer->getLength() < count * 512) return kIOReturnBadArgument;
    uint8_t *at = memory + block * 512;
    IOByteCount bytes = count * 512, moved = buffer->getDirection() == kIODirectionIn ?
        buffer->writeBytes(0, at, bytes) : buffer->readBytes(0, at, bytes);
    completion->action(completion->target, completion->parameter, moved == bytes ? kIOReturnSuccess : kIOReturnIOError, moved);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(CoachzDataBlock, IOBlockStorageDevice)

extern "C" int coachz_data_publish(void)
{
  uint64_t base = 0, bytes = 0;
  if (mdevgetrange(0, &base, &bytes) || !base || !bytes || (bytes & 511)) return 0;
  IOService *resources = IOService::getResourceService();
  CoachzDataBlock *device = OSTypeAlloc(CoachzDataBlock);
  bool started = device && device->init(nullptr) && device->setMemory(reinterpret_cast<uint8_t *>(base), bytes) &&
                 resources && device->attach(resources);
  if (started && !device->start(resources)) { device->detach(resources); started = false; }
  OSSafeReleaseNULL(device);
  return started;
}

// The /dev path of the APFS volume above the Data seed, once IOKit publishes it (up to 10 s).
extern "C" int coachz_data_volume_device(char *path, size_t size)
{
  if (!path || size < 6) return 0;
  path[0] = 0;
  for (unsigned attempt = 0; attempt < 100 && !path[0]; attempt++) {
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSString *content = OSString::withCString("41504653-0000-11AA-AA11-00306543ECAC");
    OSIterator *iterator = matching && content && matching->setObject("Content", content) ?
        IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(content); OSSafeReleaseNULL(matching);
    while (iterator && !path[0]) {
      IOService *media = OSDynamicCast(IOService, iterator->getNextObject());
      if (!media) break;
      OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
      unsigned depth = 0;
      for (IOService *provider = media; name && provider && depth < 64; provider = provider->getProvider(), depth++) {
        if (OSDynamicCast(CoachzDataBlock, provider)) { snprintf(path, size, "/dev/%s", name->getCStringNoCopy()); break; }
      }
    }
    OSSafeReleaseNULL(iterator);
    if (!path[0]) IOSleep(100);
  }
  return path[0] != 0;
}

// Shows on the screen console whether the storage stack above us came up.
extern "C" void coachz_usb_report(void)
{
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) { kprintf("COACHZ media: none\n"); return; }
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSString *content = OSDynamicCast(OSString, media->getProperty("Content"));
    OSNumber *size = OSDynamicCast(OSNumber, media->getProperty("Size"));
    kprintf("COACHZ media: %s size %llu content %s\n", name ? name->getCStringNoCopy() : "-",
        size ? size->unsigned64BitValue() : 0ull, content ? content->getCStringNoCopy() : "-");
  }
  iterator->release();
}

// Whether a BSD device is media stacked on this driver, as the unsealed root patch asks. One media
// must carry that major and minor, and its provider chain must reach us. This is not a verification.
extern "C" int coachz_usb_root_device(dev_t device)
{
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(2);
  OSNumber *mjr = OSNumber::withNumber(static_cast<unsigned>(major(device)), 32);
  OSNumber *mnr = OSNumber::withNumber(static_cast<unsigned>(minor(device)), 32);
  bool ready = matching && properties && mjr && mnr && properties->setObject(kIOBSDMajorKey, mjr) &&
               properties->setObject(kIOBSDMinorKey, mnr) && matching->setObject("IOPropertyMatch", properties);
  OSSafeReleaseNULL(mnr); OSSafeReleaseNULL(mjr); OSSafeReleaseNULL(properties);
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) return 0;
  unsigned matches = 0;
  bool ours = false;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media || ++matches != 1) { ours = false; continue; }
    unsigned depth = 0;
    for (IOService *provider = media; provider && depth < 64; provider = provider->getProvider(), depth++) {
      if (OSDynamicCast(NativeRamBlock, provider)) { ours = true; break; }
    }
  }
  iterator->release();
  return matches == 1 && ours;
}

// The /dev path of the one media with this volume UUID on this driver, for the SystemOS mount.
extern "C" int coachz_usb_volume_device(const char *uuid, char *path, size_t size)
{
  if (!uuid || !path || size < 6) return 0;
  path[0] = 0;
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  bool ready = matching && properties && wanted && properties->setObject("UUID", wanted) &&
               matching->setObject("IOPropertyMatch", properties);
  OSSafeReleaseNULL(wanted); OSSafeReleaseNULL(properties);
  // The root lookup waited only for the root volume: give another container up to 10 s to publish.
  IOService *first = ready ? IOService::waitForMatchingService(matching, 10ull * NSEC_PER_SEC) : nullptr;
  OSSafeReleaseNULL(first);
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) return 0;
  unsigned matches = 0;
  bool ours = false;
  OSString *name = nullptr;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media || ++matches != 1) { ours = false; continue; }
    name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    unsigned depth = 0;
    for (IOService *provider = media; provider && depth < 64; provider = provider->getProvider(), depth++) {
      if (OSDynamicCast(NativeRamBlock, provider)) { ours = true; break; }
    }
  }
  bool found = matches == 1 && ours && name;
  if (found) snprintf(path, size, "/dev/%s", name->getCStringNoCopy());
  iterator->release();
  return found;
}

extern "C" int native_ram_block_volume_device(const char *, dev_t *, char *, size_t) { return 0; }
extern "C" int native_ram_block_gpu_addon(const uint8_t **, uint64_t *) { return 0; }
extern "C" int native_ram_block_volume_seed(const char *, dev_t *, char *, size_t, uint64_t *, uint64_t *, uint8_t *) { return 0; }
