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

extern "C" int native_ram_block_volume_device(const char *, dev_t *, char *, size_t) { return 0; }
extern "C" int native_ram_block_gpu_addon(const uint8_t **, uint64_t *) { return 0; }
extern "C" int native_ram_block_volume_seed(const char *, dev_t *, char *, size_t, uint64_t *, uint64_t *, uint8_t *) { return 0; }
