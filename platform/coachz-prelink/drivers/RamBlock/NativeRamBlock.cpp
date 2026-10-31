// SPDX-License-Identifier: BSD-2-Clause-Patent
// Adapted from UniXNU drivers/RamBlock/NativeRamBlock.cpp, Copyright (c) 2026, UniXNU contributors.
// The IOKit side of UniXNU's RamBlock (same class, personality and exported names), with the
// USB mass storage device behind it instead of memory. Read-only: the kernel reports the medium
// as write protected unless the explicit, validated external Data partition is selected.
// In that mode only sectors wholly inside that partition can be written.
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <sys/param.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include "USBDataWindow.h"

extern "C" {
int coachz_usb_open(uint64_t *blocks);
int coachz_usb_read(uint64_t block, uint32_t count, void **data);
int coachz_usb_write(uint64_t block, uint32_t count, const void *data);
int coachz_usb_sync(void);
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
// For the periodic report: read ops, read bytes, write ops, write bytes, time holding the gate, time waiting for it
// (mach units), the slowest operation since the report last cleared it.
volatile uint64_t coachz_io_stats[8];
volatile uint64_t native_ram_block_resident_bytes = 0;
volatile uint32_t native_ram_block_sparse = 0;
uint8_t native_ram_block_seed_sha256[32];
int native_ram_block_volume_device(const char *, dev_t *, char *, size_t);
int native_ram_block_gpu_addon(const uint8_t **, uint64_t *);
int native_ram_block_volume_seed(const char *, dev_t *, char *, size_t, uint64_t *, uint64_t *, uint8_t *);
}

static CoachzUSBDataWindow data_window;
static char data_uuid[40];
static bool persistent_data;

/*
 * The kernel log on the external disk: once a second, everything the screen console printed since the last flush
 * (coachz_log_capture, filled by osfmk/arm64/coachz_early.c) is appended to the log ring after its header block, the header
 * is rewritten and the drive's cache is synchronized, under the same gate as block I/O. One boot per ring: it starts again
 * at each boot. Header: "COACHZLOG1\0", then little-endian u64 bytes stored, bytes dropped, flushes, seconds since boot.
 */
extern "C" {
extern char coachz_log_capture[];
extern volatile uint64_t coachz_log_head;
}
static const uint64_t kCaptureBytes = 1ull << 20;
static CoachzUSBDataWindow log_window;
static IOLock *log_gate;
static thread_call_t log_call;
static uint64_t log_taken, log_stored, log_dropped, log_flushes;
static uint8_t *log_buffer;  // 128 blocks: the header, then up to 127 blocks to write
static uint8_t log_tail[512];  // the bytes of the last partial block, rewritten with the next flush

static void log_put(uint64_t at, uint64_t value) {
  for (unsigned i = 0; i < 8; i++) log_buffer[at + i] = (uint8_t)(value >> (8 * i));
}

static bool log_write_header(void) {
  uint64_t ns;
  absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
  memset(log_buffer, 0, 512);
  memcpy(log_buffer, "COACHZLOG1", 11);
  log_put(16, log_stored);
  log_put(24, log_dropped);
  log_put(32, log_flushes);
  log_put(40, ns / 1000000000ull);
  return coachz_usb_write(log_window.first, 1, log_buffer);
}

// The caller holds log_gate.
static void log_store(void) {
  uint64_t head = coachz_log_head;
  if (head - log_taken > kCaptureBytes) {
    log_dropped += head - log_taken - kCaptureBytes;
    log_taken = head - kCaptureBytes;
  }
  const uint64_t capacity = (log_window.blocks - 1) * 512;
  bool ok = true;
  while (ok && log_taken < head && log_stored < capacity) {
    // Rewrite from the block holding the current end: its earlier bytes come from log_tail.
    uint64_t block = log_stored / 512, offset = log_stored % 512;
    uint64_t room = 127 * 512 - offset, take = head - log_taken;
    if (take > room) take = room;
    if (take > capacity - log_stored) take = capacity - log_stored;
    uint8_t *out = log_buffer + 512;
    memcpy(out, log_tail, offset);
    for (uint64_t i = 0; i < take; i++) out[offset + i] = (uint8_t)coachz_log_capture[(log_taken + i) & (kCaptureBytes - 1)];
    uint64_t end = offset + take, blocks = (end + 511) / 512;
    memset(out + end, 0, blocks * 512 - end);
    ok = coachz_usb_write(log_window.first + 1 + block, (uint32_t)blocks, out);
    if (ok) {
      log_taken += take;
      log_stored += take;
      memcpy(log_tail, out + (end / 512) * 512, end % 512);
    }
  }
  if (log_taken < head && log_stored >= capacity) {
    log_dropped += head - log_taken;
    log_taken = head;
  }
  log_flushes++;
  if (ok) ok = log_write_header() && coachz_usb_sync();
}

static void log_flush(thread_call_param_t, thread_call_param_t) {
  IOLockLock(log_gate);
  log_store();
  IOLockUnlock(log_gate);
  uint64_t deadline;
  clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
  thread_call_enter_delayed(log_call, deadline);
}

// A panic stops the flusher, so the panic path stores what it printed. The gate is never released: nothing else may use the
// drive from here on, and an unlock could wake waiters on CPUs that are already stopped. If a transfer holds the gate for
// half a second, its controller state is unknown and the log stays as it is.
extern "C" void coachz_log_panic_flush(void) {
  static volatile uint32_t running;
  static bool held;
  if (!log_gate || !log_buffer || !__sync_bool_compare_and_swap(&running, 0, 1)) return;
  if (!held) {
    uint64_t start = mach_absolute_time(), ns = 0;
    do {
      held = IOLockTryLock(log_gate);
      absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    } while (!held && ns < 500000000ull);
  }
  if (held) log_store();
  running = 0;
}

// The Zed keyboard (coachz_usb_hid.c): its interrupt queue every 8 ms, under the same gate as block I/O.
extern "C" {
void coachz_usb_hid_poll(void);
int coachz_usb_keyboard_enabled(void);
}
static thread_call_t keyboard_call;

static void keyboard_poll(thread_call_param_t, thread_call_param_t) {
  IOLockLock(log_gate);
  coachz_usb_hid_poll();
  IOLockUnlock(log_gate);
  uint64_t deadline;
  clock_interval_to_deadline(8, NSEC_PER_MSEC, &deadline);
  thread_call_enter_delayed(keyboard_call, deadline);
}

static void log_start(IOLock *gate, uint64_t capacity) {
  if (!coachz_log_window(capacity, data_window, log_window)) {
    kprintf("COACHZ log: no room for the log ring past the partitions\n");
    return;
  }
  log_buffer = static_cast<uint8_t *>(IOMalloc(128 * 512));
  log_call = thread_call_allocate(log_flush, nullptr);
  if (!log_buffer || !log_call) return;
  memset(log_buffer, 0, 128 * 512);
  log_gate = gate;
  kprintf("COACHZ log: kernel log ring at block %llu, %llu MiB\n", log_window.first, log_window.blocks / 2048);
  thread_call_enter(log_call);
  if (coachz_usb_keyboard_enabled() && (keyboard_call = thread_call_allocate(keyboard_poll, nullptr))) {
    thread_call_enter(keyboard_call);
  }
}

class NativeRamBlock : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(NativeRamBlock)
  IOLock *gate = nullptr;
  uint8_t *write_buffer = nullptr;
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
  IOReturn reportWriteProtection(bool *value) override { *value = !persistent_data; return kIOReturnSuccess; }
  IOReturn doSynchronizeCache() override;
  IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
  IOReturn getWriteCacheState(bool *value) override { *value = persistent_data; return kIOReturnSuccess; }
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
  if (PE_parse_boot_argn("coachz-data-uuid", data_uuid, sizeof(data_uuid))) {
    uint8_t *work = static_cast<uint8_t *>(IOMallocZero(33792));
    auto read = [](uint64_t block, uint32_t count, uint8_t *out) {
      void *source = nullptr;
      if (!coachz_usb_read(block, count, &source)) return false;
      memcpy(out, source, count * 512u);
      return true;
    };
    bool valid = strlen(data_uuid) == 36 && work && coachz_data_window(capacity, read, data_window, work, 33792);
    if (work) IOFree(work, 33792);
    if (!valid) panic("Coachz external Data GPT validation failed");
    write_buffer = static_cast<uint8_t *>(IOMalloc(128 * 512));
    if (!write_buffer || !coachz_usb_sync()) panic("Coachz external Data cache synchronization unavailable");
    persistent_data = true;
    kprintf("COACHZ Data: external partition validated, %llu MiB, protected outside window\n", data_window.blocks / 2048);
    log_start(gate, capacity);
  }
  native_ram_block_bytes = capacity * 512;
  online = true;
  native_ram_block_ready = 1;
  registerService();
  return true;
}

IOReturn NativeRamBlock::doSynchronizeCache()
{
  if (!gate) return kIOReturnNotReady;
  IOLockLock(gate);
  IOReturn status = !online ? kIOReturnNotReady :
      persistent_data && !coachz_usb_sync() ? kIOReturnIOError : kIOReturnSuccess;
  IOLockUnlock(gate);
  return status;
}

IOReturn NativeRamBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *, IOStorageCompletion *completion)
{
  if (!buffer || !completion || !completion->action || !count || block >= capacity ||
      count > capacity - block || buffer->getLength() < count * 512 ||
      (buffer->getDirection() != kIODirectionIn && buffer->getDirection() != kIODirectionOut)) return kIOReturnBadArgument;
  bool write = buffer->getDirection() == kIODirectionOut;
  if (write && (!persistent_data || !data_window.contains(block, count))) return kIOReturnNotWritable;
  if (!gate) return kIOReturnNotReady;
  const uint64_t asked = mach_absolute_time();
  IOLockLock(gate);
  if (!online) { IOLockUnlock(gate); return kIOReturnNotReady; }
  const uint64_t started = mach_absolute_time();
  const uint32_t most = coachz_usb_max_blocks();
  uint64_t done = 0;
  IOReturn status = kIOReturnSuccess;
  while (done < count) {
    uint32_t n = count - done > most ? most : static_cast<uint32_t>(count - done);
    if (write) {
      if (buffer->readBytes(done * 512, write_buffer, n * 512ull) != n * 512ull ||
          !coachz_usb_write(block + done, n, write_buffer)) { status = kIOReturnIOError; break; }
    } else {
      void *data = nullptr;
      if (!coachz_usb_read(block + done, n, &data) ||
          buffer->writeBytes(done * 512, data, n * 512ull) != n * 512ull) { status = kIOReturnIOError; break; }
    }
    done += n;
  }
  __atomic_fetch_add(write ? &native_ram_block_writes : &native_ram_block_reads, 1, __ATOMIC_RELAXED);
  if (status) __atomic_fetch_add(&native_ram_block_failures, 1, __ATOMIC_RELAXED);
  const uint64_t ended = mach_absolute_time();
  __atomic_fetch_add(&coachz_io_stats[write ? 2 : 0], 1, __ATOMIC_RELAXED);
  __atomic_fetch_add(&coachz_io_stats[write ? 3 : 1], done * 512, __ATOMIC_RELAXED);
  __atomic_fetch_add(&coachz_io_stats[4], ended - started, __ATOMIC_RELAXED);
  __atomic_fetch_add(&coachz_io_stats[5], started - asked, __ATOMIC_RELAXED);
  if (ended - started > coachz_io_stats[6]) __atomic_store_n(&coachz_io_stats[6], ended - started, __ATOMIC_RELAXED);
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
  if (write_buffer) { IOFree(write_buffer, 128 * 512); write_buffer = nullptr; }
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
  if (persistent_data) return 1;
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
  if (persistent_data) {
    if (!coachz_usb_volume_device(data_uuid, path, size)) return 0;
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSIterator *iterator = matching ? IOService::getMatchingServices(matching) : nullptr;
    OSSafeReleaseNULL(matching);
    bool found = false;
    while (iterator) {
      IOService *media = OSDynamicCast(IOService, iterator->getNextObject());
      if (!media) break;
      OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
      if (!name || strcmp(name->getCStringNoCopy(), path + 5)) continue;
      bool partition = false;
      for (unsigned depth = 0; media && depth < 64; media = media->getProvider(), depth++) {
        // IOMedia belongs to the original prelinked storage kext, not the source kernel.
        IOMedia *extent = static_cast<IOMedia *>(media->metaCast("IOMedia"));
        if (extent && extent->getBase() == data_window.first * 512 &&
            extent->getSize() == data_window.blocks * 512) partition = true;
        if (OSDynamicCast(NativeRamBlock, media)) { found = partition; break; }
      }
      break;
    }
    OSSafeReleaseNULL(iterator);
    if (!found) path[0] = 0;
    return found;
  }
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
