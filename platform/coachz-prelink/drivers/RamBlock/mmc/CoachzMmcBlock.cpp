// SPDX-License-Identifier: BSD-2-Clause
// The IOKit side of the stage-1 SDHCI driver: one write-protected IOBlockStorageDevice per slot (eMMC, microSD),
// attached under IOResources like CoachzDataBlock in drivers/RamBlock/NativeRamBlock.cpp, so the original
// IOStorageFamily publishes IOMedia and the GPT partitions above it. Not in the kernel build.
//
// coachz_mmc_start() is the only entry point. It returns at once; each enabled slot (coachz-emmc=1, coachz-sd=1)
// is brought up on its own kernel thread, because card initialisation sleeps for tens of milliseconds and CMD1/ACMD41
// may poll for up to two seconds. The class subclasses IOBlockStorageDevice of the prelinked IOStorageFamily, so it is
// linked with the same storage-original-bindings.S as NativeRamBlock.
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>

extern "C" {
int coachz_sdcc_prepare(void);
int coachz_sdcc_open(int slot, uint64_t *blocks);
int coachz_sdcc_read(int slot, uint64_t block, uint32_t count, void **data);
uint32_t coachz_sdcc_max_blocks(void);
int coachz_sdcc_removable(int slot);
int coachz_sdcc_enabled(int slot);
void coachz_mmc_start(void);
kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
void thread_deallocate(thread_t);
}

class CoachzMmcBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(CoachzMmcBlock)
  IOLock *gate = nullptr;
  uint64_t capacity = 0;
  int slot = -1;
  bool online = false;
public:
  bool setSlot(int which, uint64_t blocks) {
    slot = which; capacity = blocks; gate = IOLockAlloc(); online = gate && capacity;
    return online;
  }
  bool start(IOService *provider) override { return IOBlockStorageDevice::start(provider) && (registerService(), true); }
  void free() override { if (gate) { IOLockFree(gate); gate = nullptr; } IOBlockStorageDevice::free(); }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("Coachz"); }
  char *getProductString() override { return const_cast<char *>(slot == 0 ? "eMMC" : "microSD"); }
  char *getRevisionString() override { return const_cast<char *>("1.0"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("sdhci-msm, read-only stage 1"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = 512; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = capacity - 1; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn reportRemovability(bool *value) override { *value = coachz_sdcc_removable(slot) != 0; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override { return kIOReturnSuccess; }
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return kIOReturnSuccess; }
  IOReturn getWriteCacheState(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                            IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!buffer || !completion || !completion->action || !count || block >= capacity || count > capacity - block ||
        buffer->getLength() < count * 512) return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    IOLockLock(gate);
    const uint32_t most = coachz_sdcc_max_blocks();
    uint64_t done = 0;
    IOReturn status = kIOReturnSuccess;
    while (done < count) {
      const uint32_t n = count - done > most ? most : static_cast<uint32_t>(count - done);
      void *data = nullptr;
      if (!coachz_sdcc_read(slot, block + done, n, &data) ||
          buffer->writeBytes(done * 512, data, n * 512ull) != n * 512ull) { status = kIOReturnIOError; break; }
      done += n;
    }
    IOLockUnlock(gate);
    completion->action(completion->target, completion->parameter, status, done * 512);
    return kIOReturnSuccess;
  }
};
OSDefineMetaClassAndStructors(CoachzMmcBlock, IOBlockStorageDevice)

static void coachz_mmc_slot_thread(void *parameter, wait_result_t)
{
  const int slot = static_cast<int>(reinterpret_cast<uintptr_t>(parameter));
  uint64_t blocks = 0;
  if (!coachz_sdcc_open(slot, &blocks)) return;
  IOService *resources = IOService::getResourceService();
  CoachzMmcBlock *device = OSTypeAlloc(CoachzMmcBlock);
  bool started = device && device->init(nullptr) && device->setSlot(slot, blocks) && resources && device->attach(resources);
  if (started && !device->start(resources)) { device->detach(resources); started = false; }
  OSSafeReleaseNULL(device);
  IOLog("COACHZ mmc slot %d: block device %s, %llu blocks\n", slot, started ? "published" : "not published", blocks);
}

extern "C" void coachz_mmc_start(void)
{
  if (!coachz_sdcc_enabled(0) && !coachz_sdcc_enabled(1)) return;
  if (!coachz_sdcc_prepare()) return;
  for (int slot = 0; slot < 2; slot++) {
    if (!coachz_sdcc_enabled(slot)) continue;
    thread_t thread;
    if (kernel_thread_start(coachz_mmc_slot_thread, reinterpret_cast<void *>(static_cast<uintptr_t>(slot)), &thread) == KERN_SUCCESS)
      thread_deallocate(thread);
  }
}
