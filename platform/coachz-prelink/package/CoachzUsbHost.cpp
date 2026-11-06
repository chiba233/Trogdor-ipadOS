// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Apple USB host stack on usb_1 (coachz-usb-host=1, eMMC root only; docs/plans/usb-host.md).
//
// The original AppleUSBXHCI and IOUSBHostFamily drive the DWC3's xHCI. Apple's T8011 USB complex, PHY and arbitrator drivers cannot
// run here, so this file supplies what they would: the controller's provider (CoachzUsbNub: the register window, SPI 133 and the
// IOMapper), the mapper itself on the apps SMMU (CoachzUsbMapper), and the controller object, an original AppleNubT8011USBXHCI
// instance whose dispatch table is a copy with the subclass's platform slots taken back from AppleUSBXHCI (OriginalAppleUSBABI.h).
//
// AppleUSBHostController::start takes its mapper from IOMapper::copyMapperForDevice(provider) and fails without one; with a mapper,
// IODMACommand and IOMemoryDescriptor::dmaMap want one contiguous device address range for a whole descriptor, so the mapper gives
// the DWC3's stream (sc7180.dtsi: iommus = <&apps_smmu 0x540 0>) a context bank of its own. Firmware leaves the apps SMMU with no
// valid stream match and CLIENTPD set; unmatched streams keep bypassing once it is enabled (sCR0.USFCFG clear), so every other master
// is unaffected. The DWC3 node has no dma-coherent: the mapper cleans and invalidates a buffer when it maps and when it unmaps it, and
// the controller allocates its rings write-combined (Normal non-cacheable) instead of cacheable.
#include <IOKit/IOLib.h>
#include <string.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IOInterrupts.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>
#include "ArmSmmu4KLive.h"
#include "OriginalAppleUSBABI.h"

extern "C" vm_offset_t ml_io_map(vm_offset_t physical, vm_size_t size);
extern "C" vm_offset_t phystokv(uint64_t pa);
extern "C" void dcache_incoherent_io_flush64(addr64_t pa, unsigned int count, unsigned int remaining, unsigned int *res);
extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);   // ArmCpu.cpp, IOReturn
extern "C" uint64_t armcpu_spi_count(uint32_t intid);
extern "C" unsigned int pmap_cache_attributes(ppnum_t pn);
extern "C" void pmap_set_cache_attributes(ppnum_t pn, unsigned int cacheattr);
extern "C" int coachz_usb_host_soc_init(void);   // drivers/RamBlock/coachz_usb.c: its DMA pool, then usbsoc (GDSC, clocks, QUSB2, QMP, DWC3 host)
// The base's input decoding, shared with the libpayload transport (drivers/RamBlock/coachz_zed_input.c).
extern "C" void coachz_zed_keyboard(const unsigned char *report, unsigned length);
extern "C" void coachz_zed_pad(const unsigned char *report, unsigned length);
extern "C" void coachz_zed_release(int keyboard, int pad);
extern "C" void coachz_zed_counts(unsigned *keyboard_reports, unsigned *actions, unsigned *pad_reports, unsigned *clicks);
extern "C" int coachz_usb_keyboard_requested(void);   // drivers/RamBlock/coachz_usb.c: coachz-usb-keyboard=1

namespace {

void log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log(const char *fmt, ...) {
  char line[200]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
  printf("COACHZ usb-host: %s\n", line);
}

// sc7180.dtsi: usb_1_dwc3 reg <0x0a600000 0xe000>, interrupts GIC_SPI 133 level high, iommus <&apps_smmu 0x540 0>;
// apps_smmu (qcom,sc7180-smmu-500) reg <0x15000000 0x100000>; intc (GICv3) distributor at 0x17a00000.
constexpr uint64_t kXhciBase = 0x0a600000, kXhciBytes = 0xe000;
constexpr uint32_t kXhciIntid = 32 + 133;
constexpr uint64_t kSmmuBase = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint16_t kStream = 0x540;
constexpr uint64_t kGicdBase = 0x17a00000;
// The device's addresses: a 1 GiB window below 4 GiB, 4 KiB pages.
constexpr uint64_t kIovaBase = 0x10000000, kIovaPages = 0x40000, kPage = 4096;
// Table pages for that window: a root, one level-2 table and at most 512 level-3 tables.
constexpr size_t kTablePages = 528;
constexpr unsigned kAsid = 1, kPoll = 1000000;

// SMMUv2 global register space 0 and the context bank registers this file touches (arm-smmu.h).
enum : size_t {
  kGr0Scr0 = 0x0, kGr0Sacr = 0x10, kGr0Idr0 = 0x20, kGr0Idr1 = 0x24, kGr0Idr2 = 0x28, kGr0Idr7 = 0x3c, kGr0Sgfsr = 0x48,
  kGr0Sgfsynr0 = 0x50, kGr0TlbiAllNsnh = 0x68, kGr0TlbiAllH = 0x6c, kGr0TlbGsync = 0x70, kGr0TlbGstatus = 0x74,
  kGr0Smr = 0x800, kGr0S2cr = 0xc00, kCbSctlr = 0x0, kCbActlr = 0x4, kCbFsr = 0x58, kCbFar = 0x60, kCbFsynr0 = 0x68,
};
enum : uint32_t {
  kScr0ClientPd = 1u << 0, kScr0Gfre = 1u << 1, kScr0Gcfgfre = 1u << 4, kScr0Usfcfg = 1u << 10, kScr0Vmidpne = 1u << 11,
  kScr0Ptm = 1u << 12, kScr0Fb = 1u << 13, kScr0Bsu = 3u << 14, kSmrValid = 1u << 31,
  kMmu500AcrCacheLock = 1u << 26, kMmu500AcrS2crbTlben = 1u << 10, kMmu500AcrSmtnmbTlben = 1u << 8, kMmu500ActlrCpre = 1u << 1,
};

// ---- the apps SMMU window, as the kit's I/O ----
struct Window { volatile uint8_t *base; size_t bytes; };
Window gSmmu;
int smmu_read32(void *c, size_t at, uint32_t *v) {
  const Window *w = static_cast<Window *>(c); if (at & 3 || at + 4 > w->bytes) return 1;
  *v = *reinterpret_cast<volatile uint32_t *>(w->base + at); return 0;
}
int smmu_read64(void *c, size_t at, uint64_t *v) {
  const Window *w = static_cast<Window *>(c); if (at & 7 || at + 8 > w->bytes) return 1;
  *v = *reinterpret_cast<volatile uint64_t *>(w->base + at); return 0;
}
int smmu_write32(void *c, size_t at, uint32_t v) {
  const Window *w = static_cast<Window *>(c); if (at & 3 || at + 4 > w->bytes) return 1;
  *reinterpret_cast<volatile uint32_t *>(w->base + at) = v; return 0;
}
int smmu_write64(void *c, size_t at, uint64_t v) {
  const Window *w = static_cast<Window *>(c); if (at & 7 || at + 8 > w->bytes) return 1;
  *reinterpret_cast<volatile uint64_t *>(w->base + at) = v; return 0;
}
int smmu_barrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return 0; }
const ArmSmmu4KIo gIo = {&gSmmu, kSmmuBytes, smmu_read32, smmu_read64, smmu_write32, smmu_write64, smmu_barrier};
uint32_t gr0(size_t at) { uint32_t v = 0; smmu_read32(&gSmmu, at, &v); return v; }
void gr0_write(size_t at, uint32_t v) { smmu_write32(&gSmmu, at, v); smmu_barrier(nullptr); }

void flush(uint64_t physical, uint64_t bytes) {
  // remaining 0: always by region. A larger value lets the routine clean the whole cache by set/way, which reaches this core only.
  while (bytes) {
    const unsigned chunk = bytes > 0x40000000 ? 0x40000000u : static_cast<unsigned>(bytes);
    unsigned int result = 0; dcache_incoherent_io_flush64(physical, chunk, 0, &result);
    physical += chunk; bytes -= chunk;
  }
}

// ---- table pages: one contiguous, cacheable pool; every changed page is cleaned before the walker may read it ----
struct Pool { IOBufferMemoryDescriptor *md; uint8_t *va; uint64_t pa; size_t used; };
Pool gPool;
int pool_allocate(void *, uint64_t *physical, volatile uint64_t **mapping) {
  if (gPool.used >= kTablePages) return 1;
  uint8_t *page = gPool.va + gPool.used * kPage;
  bzero(page, kPage);
  flush(gPool.pa + gPool.used * kPage, kPage);
  *physical = gPool.pa + gPool.used * kPage; *mapping = reinterpret_cast<volatile uint64_t *>(page); ++gPool.used;
  return 0;
}
volatile uint64_t *pool_find(void *, uint64_t physical) {
  if (physical < gPool.pa || physical >= gPool.pa + gPool.used * kPage || physical & (kPage - 1)) return nullptr;
  return reinterpret_cast<volatile uint64_t *>(gPool.va + (physical - gPool.pa));
}
int pool_overlaps(void *, uint64_t physical, uint64_t bytes) {
  return physical < gPool.pa + kTablePages * kPage && gPool.pa < physical + bytes;
}
int publish_table_page(void *, volatile uint64_t *page, size_t bytes) {
  const uint8_t *at = reinterpret_cast<const uint8_t *>(const_cast<uint64_t *>(page));
  if (at < gPool.va || at + bytes > gPool.va + kTablePages * kPage) return 1;
  flush(gPool.pa + static_cast<uint64_t>(at - gPool.va), bytes);
  __asm__ volatile("dsb sy" ::: "memory");
  return 0;
}
const ArmSmmu4KTablePages gPages = {nullptr, pool_allocate, pool_find, pool_overlaps};
const ArmSmmu4KLiveVisibility gVisibility = {nullptr, publish_table_page};

// ---- the context: one bank, one stream match, one arena of the IOVA window ----
ArmSmmu4KLiveArena gArena;
ArmSmmu4KLiveState gState;
ArmSmmu4KContextConfig gContext;
unsigned gBank, gSmr;
uint64_t gCbOffset;
bool gSmmuReady;

// Every mapped range sorted by IOVA (the kit wants all of them for each update), and one record per mapping.
constexpr size_t kMaxRanges = 32768, kMaxRecords = 4096;
struct Record { uint64_t iova; uint32_t pages, first, count; };
ArmSmmu4KRange *gRanges;
size_t gRangeCount;
Record *gRecords;
size_t gRecordCount;
uint32_t *gShadow;   // IOVA page -> physical page number, read without the lock by mapToPhysicalAddress
uint8_t *gBitmap;    // one bit per IOVA page in use
IOLock *gMapLock;
uint64_t gMaps, gUnmaps, gMapFailures, gLivePages;   // under gMapLock
// The controller does not snoop (no dma-coherent in sc7180.dtsi) and the original driver, written for coherent T8011, writes its rings
// without cache maintenance. k179: the two pages allocated per device after power-on were cacheable (VM_WIMG 0x2) while every
// earlier structure was write-combined (0x6), and the first EP0 transfer never started. A driver buffer the controller can reach is
// therefore write-combined for as long as it is mapped, as dma_alloc_coherent gives a non-coherent master; client memory that is not an
// IOBufferMemoryDescriptor keeps the clean/invalidate at map and unmap. Counted per physical page, under gMapLock.
struct Uncached { uint32_t ppn, refs; unsigned original; };
constexpr size_t kMaxUncached = 1024;
Uncached gUncached[kMaxUncached];
size_t gUncachedCount;
uint8_t *gUncachedIova;   // IOVA page -> 1 when it holds one reference above
uint64_t gUncachedPages, gUncachedFull;
constexpr unsigned kWimgNotCacheable = 0x4, kWimgWriteCombined = 0x6;   // arm/memory_types.h: VM_MEM_NOT_CACHEABLE, VM_WIMG_WCOMB
bool uncached_get(uint32_t ppn) {
  for (size_t i = 0; i < gUncachedCount; ++i) if (gUncached[i].ppn == ppn) { ++gUncached[i].refs; return true; }
  const unsigned original = pmap_cache_attributes(ppn);
  if (original & kWimgNotCacheable) return false;
  if (gUncachedCount == kMaxUncached) { ++gUncachedFull; return false; }
  gUncached[gUncachedCount++] = {ppn, 1, original};
  pmap_set_cache_attributes(ppn, kWimgWriteCombined);
  ++gUncachedPages;
  return true;
}
void uncached_put(uint32_t ppn) {
  for (size_t i = 0; i < gUncachedCount; ++i) {
    if (gUncached[i].ppn != ppn) continue;
    if (--gUncached[i].refs) return;
    pmap_set_cache_attributes(ppn, gUncached[i].original);
    gUncached[i] = gUncached[--gUncachedCount];
    return;
  }
}

bool smmu_start() {
  const vm_offset_t base = ml_io_map(kSmmuBase, kSmmuBytes);
  if (!base) { log("no apps SMMU window"); return false; }
  gSmmu = {reinterpret_cast<volatile uint8_t *>(base), kSmmuBytes};
  const uint32_t scr0 = gr0(kGr0Scr0), idr0 = gr0(kGr0Idr0), idr1 = gr0(kGr0Idr1), idr2 = gr0(kGr0Idr2), idr7 = gr0(kGr0Idr7);
  const unsigned smrs = idr0 & 0xff, banks = idr1 & 0xff, pagesize = (idr1 >> 31) ? 0x10000 : 0x1000;
  const unsigned numpage = 1u << (((idr1 >> 28) & 7) + 1);
  log("apps SMMU sCR0 0x%x IDR0 0x%x IDR1 0x%x IDR2 0x%x IDR7 0x%x: %u SMRs, %u banks, page 0x%x", scr0, idr0, idr1, idr2, idr7, smrs,
      banks, pagesize);
  if (pagesize != 0x1000 || static_cast<uint64_t>(numpage) * pagesize * 2 > kSmmuBytes || !smrs || !banks || (idr0 >> 8) & 1) {
    log("unexpected SMMU layout; the USB host stack stays off"); return false;
  }
  // Firmware's state as found on hardware: client access disabled and no valid stream match. Anything else is left alone.
  if (!(scr0 & kScr0ClientPd)) { log("the apps SMMU is already enabled; not changing it"); return false; }
  for (unsigned i = 0; i < smrs; ++i) {
    if (gr0(kGr0Smr + 4 * i) & kSmrValid) { log("SMR %u is valid; not changing the apps SMMU", i); return false; }
  }
  gSmr = 0; gBank = 0;
  gCbOffset = static_cast<uint64_t>(numpage) * pagesize + static_cast<uint64_t>(gBank) * pagesize;
  uint32_t sctlr = 0; smmu_read32(&gSmmu, gCbOffset + kCbSctlr, &sctlr);
  if (sctlr & 1) { log("bank %u is enabled; not changing it", gBank); return false; }

  gPool.md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
      kTablePages * kPage, 0x0000000ffffff000ULL);
  if (!gPool.md || gPool.md->prepare() != kIOReturnSuccess) { log("no table pool"); return false; }
  gPool.va = static_cast<uint8_t *>(gPool.md->getBytesNoCopy()); gPool.pa = gPool.md->getPhysicalAddress(); gPool.used = 0;
  gRanges = static_cast<ArmSmmu4KRange *>(IOMallocZero(kMaxRanges * sizeof(ArmSmmu4KRange)));
  gRecords = static_cast<Record *>(IOMallocZero(kMaxRecords * sizeof(Record)));
  gShadow = static_cast<uint32_t *>(IOMallocZero(kIovaPages * sizeof(uint32_t)));
  gUncachedIova = static_cast<uint8_t *>(IOMallocZero(kIovaPages));
  gBitmap = static_cast<uint8_t *>(IOMallocZero(kIovaPages / 8));
  gMapLock = IOLockAlloc();
  if (!gPool.va || !gPool.pa || !gRanges || !gRecords || !gShadow || !gUncachedIova || !gBitmap || !gMapLock) { log("no memory for the mapper"); return false; }

  uint64_t root = 0; volatile uint64_t *rootMapping = nullptr;
  if (pool_allocate(nullptr, &root, &rootMapping)) return false;
  gArena.config = {32, 36, root, static_cast<size_t>(kIovaPages)};
  gArena.iova = kIovaBase; gArena.bytes = kIovaPages * kPage; gArena.upper_region = 0; gArena.pages = &gPages;
  ArmSmmu4KStatus status = ArmSmmu4KLivePrepare(&gArena, &gState);
  if (status != ArmSmmu4KSuccess) { log("arena not prepared: %d", status); return false; }
  publish_table_page(nullptr, rootMapping, kPage);
  gContext.lower = &gArena.config; gContext.lower_tables = &gState.tables; gContext.upper = nullptr; gContext.upper_tables = nullptr;
  gContext.id0 = idr0; gContext.id2 = idr2; gContext.asid = kAsid; gContext.coherent_walk = 0;
  gContext.stall_on_fault = 0; gContext.continue_after_fault = 0; gContext.ttbr0_switched = 0;

  // arm_mmu500_reset: on r2p0 and later CACHE_LOCK must be clear for ACTLR writes to stick; unmatched streams may allocate bypass
  // TLB entries; the next-page prefetcher (ACTLR.CPRE) is off for its errata. Only this bank's ACTLR is written.
  uint32_t sacr = gr0(kGr0Sacr);
  if (((idr7 >> 4) & 0xf) >= 2) sacr &= ~kMmu500AcrCacheLock;
  sacr |= kMmu500AcrSmtnmbTlben | kMmu500AcrS2crbTlben;
  gr0_write(kGr0Sacr, sacr);
  uint32_t actlr = 0; smmu_read32(&gSmmu, gCbOffset + kCbActlr, &actlr);
  smmu_write32(&gSmmu, gCbOffset + kCbActlr, actlr & ~kMmu500ActlrCpre); smmu_barrier(nullptr);
  smmu_read32(&gSmmu, gCbOffset + kCbActlr, &actlr);
  // On hardware CPRE stays set (ACTLR 0x3): Secure keeps its own CACHE_LOCK. Mainline on this machine logs the same
  // "Failed to disable prefetcher for errata workarounds, check SACR.CACHE_LOCK" and carries on; nothing more is possible here.

  ArmSmmu4KProgramResult programmed = {};
  status = ArmSmmu4KProgramDisabledContext(&gIo, gBank, &gContext, kPoll, &programmed);
  if (status != ArmSmmu4KSuccess) { log("context bank %u not programmed: %d (phase %u)", gBank, status, programmed.phase); return false; }
  if (programmed.context_offset != gCbOffset) { log("bank offset 0x%llx, expected 0x%llx", programmed.context_offset, gCbOffset); return false; }

  // arm_smmu_write_sme: S2CR (translate through the bank) before its SMR (exact stream 0x540, mask 0).
  gr0_write(kGr0S2cr + 4 * gSmr, gBank);
  gr0_write(kGr0Smr + 4 * gSmr, kSmrValid | kStream);
  // arm_smmu_device_reset: clear the global fault status, invalidate the TLB, sync, then enable client access with unmatched
  // streams bypassing; fault reporting on (no global fault handler here, so no interrupt), private VMIDs and TLB maintenance.
  gr0_write(kGr0Sgfsr, gr0(kGr0Sgfsr));
  gr0_write(kGr0TlbiAllH, 0); gr0_write(kGr0TlbiAllNsnh, 0); gr0_write(kGr0TlbGsync, 0);
  unsigned polls = 0;
  while ((gr0(kGr0TlbGstatus) & 1) && ++polls < kPoll) {}
  if (gr0(kGr0TlbGstatus) & 1) { log("global TLB sync did not complete"); return false; }
  const uint32_t enabled = ((scr0 | kScr0Gfre | kScr0Gcfgfre | kScr0Vmidpne | kScr0Ptm) & ~(kScr0ClientPd | kScr0Usfcfg | kScr0Fb | kScr0Bsu));
  gr0_write(kGr0Scr0, enabled);
  log("apps SMMU on: sCR0 0x%x -> 0x%x, sACR 0x%x, SMR %u 0x%x -> S2CR 0x%x, bank %u ACTLR 0x%x, %zu table pages", scr0, gr0(kGr0Scr0),
      gr0(kGr0Sacr), gSmr, gr0(kGr0Smr + 4 * gSmr), gr0(kGr0S2cr + 4 * gSmr), gBank, actlr, gPool.used);
  gSmmuReady = true;
  return true;
}

// ---- IOVA allocation ----
bool bit(uint64_t page) { return gBitmap[page >> 3] & (1u << (page & 7)); }
void set_bits(uint64_t first, uint64_t count, bool value) {
  for (uint64_t p = first; p < first + count; ++p) {
    if (value) gBitmap[p >> 3] |= static_cast<uint8_t>(1u << (p & 7)); else gBitmap[p >> 3] &= static_cast<uint8_t>(~(1u << (p & 7)));
  }
}
uint64_t gHint;
bool allocate_iova(uint64_t pages, uint64_t alignPages, uint64_t *first) {
  if (!alignPages) alignPages = 1;
  for (unsigned pass = 0; pass < 2; ++pass) {
    uint64_t start = pass ? 0 : gHint;
    start = (start + alignPages - 1) / alignPages * alignPages;
    while (start + pages <= kIovaPages) {
      uint64_t run = 0;
      while (run < pages && !bit(start + run)) ++run;
      if (run == pages) { set_bits(start, pages, true); gHint = start + pages; *first = start; return true; }
      start = (start + run + 1 + alignPages - 1) / alignPages * alignPages;
    }
  }
  return false;
}

}  // namespace

// ---- the mapper ----
class CoachzUsbMapper : public IOMapper {
  OSDeclareDefaultStructors(CoachzUsbMapper);
 protected:
  bool initHardware(IOService *) APPLE_KEXT_OVERRIDE { return gSmmuReady; }
 public:
  uint64_t getPageSize(void) const APPLE_KEXT_OVERRIDE { return kPage; }
  IOReturn iovmMapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, uint32_t options,
      const IODMAMapSpecification *spec, IODMACommand *command, const IODMAMapPageList *pageList, uint64_t *mapAddress,
      uint64_t *mapLength) APPLE_KEXT_OVERRIDE;
  IOReturn iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *command, uint64_t mapAddress, uint64_t mapLength) APPLE_KEXT_OVERRIDE;
  IOReturn iovmInsert(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
  uint64_t mapToPhysicalAddress(uint64_t mappedAddress) APPLE_KEXT_OVERRIDE {
    if (mappedAddress < kIovaBase || mappedAddress >= kIovaBase + kIovaPages * kPage) return 0;
    const uint32_t ppn = __atomic_load_n(&gShadow[(mappedAddress - kIovaBase) / kPage], __ATOMIC_ACQUIRE);
    return ppn ? (static_cast<uint64_t>(ppn) * kPage) | (mappedAddress & (kPage - 1)) : 0;
  }
};
OSDefineMetaClassAndStructors(CoachzUsbMapper, IOMapper);

namespace {

ArmSmmu4KLiveRegion region() { return {&gArena, &gState, gRanges, gRangeCount}; }

// The physical pages of [offset, offset + length): from the descriptor's page list, or by walking its physical segments. Inside the
// range every segment must start and end on a page boundary, or one contiguous device range cannot describe it.
IOReturn collect(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, const IODMAMapPageList *list, uint32_t *pages,
    uint64_t capacity, uint64_t *count, uint32_t *pageOffset) {
  if (list) {
    if (list->pageListCount > capacity) return kIOReturnNoResources;
    for (uint32_t i = 0; i < list->pageListCount; ++i) pages[i] = list->pageList[i].phys_addr;
    *count = list->pageListCount; *pageOffset = list->pageOffset & (kPage - 1);
    return kIOReturnSuccess;
  }
  if (!memory) return kIOReturnBadArgument;
  uint64_t at = offset, end = offset + length, n = 0;
  while (at < end) {
    IOByteCount segment = 0;
    const addr64_t physical = memory->getPhysicalSegment(at, &segment, kIOMemoryMapperNone);
    if (!physical || !segment) return kIOReturnVMError;
    if (segment > end - at) segment = static_cast<IOByteCount>(end - at);
    if (at == offset) *pageOffset = static_cast<uint32_t>(physical & (kPage - 1));
    else if (physical & (kPage - 1)) return kIOReturnNotAligned;
    if (at + segment < end && ((physical + segment) & (kPage - 1))) return kIOReturnNotAligned;
    for (uint64_t p = physical & ~(kPage - 1); p < physical + segment; p += kPage) {
      if (n >= capacity) return kIOReturnNoResources;
      pages[n++] = static_cast<uint32_t>(p / kPage);
    }
    at += segment;
  }
  *count = n;
  return kIOReturnSuccess;
}

}  // namespace

IOReturn CoachzUsbMapper::iovmMapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, uint32_t options,
    const IODMAMapSpecification *spec, IODMACommand *, const IODMAMapPageList *pageList, uint64_t *mapAddress, uint64_t *mapLength) {
  if (!gSmmuReady || !length || !mapAddress || !mapLength) return kIOReturnNotReady;
  const uint64_t capacity = (length + 2 * kPage) / kPage + (pageList ? pageList->pageListCount : 0);
  uint32_t *pages = static_cast<uint32_t *>(IOMalloc(capacity * sizeof(uint32_t)));
  if (!pages) return kIOReturnNoMemory;
  uint64_t count = 0; uint32_t pageOffset = 0;
  IOReturn result = collect(memory, offset, length, pageList, pages, capacity, &count, &pageOffset);
  // One range per physically contiguous run.
  uint64_t runs = 0;
  for (uint64_t i = 0; result == kIOReturnSuccess && i < count; ++i) runs += !i || pages[i] != pages[i - 1] + 1;
  IOLockLock(gMapLock);
  uint64_t first = 0;
  const uint64_t alignPages = spec && spec->alignment > kPage ? spec->alignment / kPage : 1;
  if (result == kIOReturnSuccess && (!count || gRangeCount + runs > kMaxRanges || gRecordCount >= kMaxRecords)) result = kIOReturnNoResources;
  if (result == kIOReturnSuccess && !allocate_iova(count, alignPages, &first)) result = kIOReturnNoSpace;
  ArmSmmu4KRange *changes = nullptr;
  if (result == kIOReturnSuccess && !(changes = static_cast<ArmSmmu4KRange *>(IOMalloc(runs * sizeof(ArmSmmu4KRange))))) {
    set_bits(first, count, false); result = kIOReturnNoMemory;
  }
  if (result == kIOReturnSuccess) {
    // Read and write for every mapping, whatever the direction (options): one physical page in two mappings must carry the same
    // attributes. Normal non-cacheable, outer shareable: io-pgtable-arm's attributes for a master that does not snoop (no IOMMU_CACHE).
    (void)options;
    uint64_t r = 0;
    for (uint64_t i = 0; i < count; ++i) {
      if (i && pages[i] == pages[i - 1] + 1) { changes[r - 1].bytes += kPage; continue; }
      changes[r++] = {kIovaBase + (first + i) * kPage, static_cast<uint64_t>(pages[i]) * kPage, kPage, ArmSmmu4KRead | ArmSmmu4KWrite, 0, 2};
    }
    // Write-combined before the flush, so no line the CPU held cacheable survives into the mapping.
    if (OSDynamicCast(IOBufferMemoryDescriptor, memory))
      for (uint64_t i = 0; i < count; ++i) gUncachedIova[first + i] = uncached_get(pages[i]);
    for (uint64_t i = 0; i < runs; ++i) flush(changes[i].physical, changes[i].bytes);
    for (uint64_t i = 0; i < count; ++i) __atomic_store_n(&gShadow[first + i], pages[i], __ATOMIC_RELEASE);
    const ArmSmmu4KLiveRegion lower = region();
    ArmSmmu4KLiveResult done = {};
    const ArmSmmu4KStatus status = ArmSmmu4KLiveUpdateContextBatch(&gIo, gBank, &gContext, &lower, nullptr, 0, &gVisibility, changes, runs, 1,
        kPoll, &done);
    if (status != ArmSmmu4KSuccess) {
      for (uint64_t i = 0; i < count; ++i) {
        __atomic_store_n(&gShadow[first + i], 0u, __ATOMIC_RELEASE);
        if (gUncachedIova[first + i]) { gUncachedIova[first + i] = 0; uncached_put(pages[i]); }
      }
      set_bits(first, count, false);
      static unsigned said; if (said++ < 8) log("map of %llu pages failed: %d (complete %u)", count, status, done.complete);
      result = kIOReturnVMError;
    } else {
      // Insert the run block at its place in the sorted list and keep the records sorted the same way.
      size_t at = 0;
      while (at < gRangeCount && gRanges[at].iova < changes[0].iova) ++at;
      memmove(&gRanges[at + runs], &gRanges[at], (gRangeCount - at) * sizeof(ArmSmmu4KRange));
      memcpy(&gRanges[at], changes, runs * sizeof(ArmSmmu4KRange));
      gRangeCount += runs;
      size_t rec = 0;
      while (rec < gRecordCount && gRecords[rec].iova < changes[0].iova) ++rec;
      memmove(&gRecords[rec + 1], &gRecords[rec], (gRecordCount - rec) * sizeof(Record));
      gRecords[rec] = {changes[0].iova, static_cast<uint32_t>(count), static_cast<uint32_t>(at), static_cast<uint32_t>(runs)};
      ++gRecordCount;
      for (size_t i = rec + 1; i < gRecordCount; ++i) gRecords[i].first += static_cast<uint32_t>(runs);
      *mapAddress = changes[0].iova + pageOffset; *mapLength = count * kPage;
      ++gMaps; gLivePages += count;
    }
  }
  if (result != kIOReturnSuccess) ++gMapFailures;
  IOLockUnlock(gMapLock);
  if (changes) IOFree(changes, runs * sizeof(ArmSmmu4KRange));
  IOFree(pages, capacity * sizeof(uint32_t));
  return result;
}

IOReturn CoachzUsbMapper::iovmUnmapMemory(IOMemoryDescriptor *, IODMACommand *, uint64_t mapAddress, uint64_t) {
  if (!gSmmuReady) return kIOReturnNotReady;
  const uint64_t base = mapAddress & ~(kPage - 1);
  IOLockLock(gMapLock);
  size_t rec = 0;
  while (rec < gRecordCount && gRecords[rec].iova != base) ++rec;
  if (rec == gRecordCount) { IOLockUnlock(gMapLock); log("unmap of an unknown address 0x%llx", mapAddress); return kIOReturnBadArgument; }
  const Record record = gRecords[rec];
  const ArmSmmu4KLiveRegion lower = region();
  ArmSmmu4KLiveResult done = {};
  const ArmSmmu4KStatus status = ArmSmmu4KLiveUpdateContextBatch(&gIo, gBank, &gContext, &lower, nullptr, 0, &gVisibility,
      &gRanges[record.first], record.count, 0, kPoll, &done);
  if (status != ArmSmmu4KSuccess) {
    // The translation may still exist: keep the pages and the addresses; nothing is reused.
    IOLockUnlock(gMapLock);
    static unsigned said; if (said++ < 8) log("unmap of %u pages at 0x%llx failed: %d", record.pages, base, status);
    return kIOReturnVMError;
  }
  // The device can no longer reach the pages: drop whatever the CPU cached while it wrote them.
  for (uint32_t i = 0; i < record.count; ++i) flush(gRanges[record.first + i].physical, gRanges[record.first + i].bytes);
  memmove(&gRanges[record.first], &gRanges[record.first + record.count], (gRangeCount - record.first - record.count) * sizeof(ArmSmmu4KRange));
  gRangeCount -= record.count;
  memmove(&gRecords[rec], &gRecords[rec + 1], (gRecordCount - rec - 1) * sizeof(Record));
  --gRecordCount;
  for (size_t i = rec; i < gRecordCount; ++i) gRecords[i].first -= record.count;
  const uint64_t first = (base - kIovaBase) / kPage;
  // The device can no longer reach them: the pages made write-combined for this mapping take their attribute back.
  for (uint64_t i = 0; i < record.pages; ++i) {
    if (gUncachedIova[first + i]) { gUncachedIova[first + i] = 0; uncached_put(gShadow[first + i]); }
    __atomic_store_n(&gShadow[first + i], 0u, __ATOMIC_RELEASE);
  }
  set_bits(first, record.pages, false);
  ++gUnmaps; gLivePages -= record.pages;
  IOLockUnlock(gMapLock);
  return kIOReturnSuccess;
}

// ---- the controller's provider ----
// AppleUSBXHCI::start requires three interrupters ("1 interrupters supported, 3 are required", k182) and the driver routes transfer
// events to interrupter 1 (k181: every EP0 TRB had Interrupter Target 1). This DWC3 has one interrupt line and no MSI, and without MSI
// only interrupter 0 raises it (xHCI 1.1 4.17), as Linux's secondary interrupters on Qualcomm are read without an interrupt of their own.
// So the nub offers sources 0 to 2 on the one SPI: each interrupt also looks at interrupters 1 and 2, a 1 ms poll does the same between
// interrupts, and a pending interrupter is handed to the handler registered for it, or to source 0's when the driver registered one
// handler for all. One spinlock keeps the handlers from running twice at once.
class CoachzUsbNub : public IOService {
  OSDeclareDefaultStructors(CoachzUsbNub);
 public:
  static constexpr int kSources = 3;
  OSObject *irqTarget[kSources] = {};
  IOInterruptAction irqHandler[kSources] = {};
  void *irqRef[kSources] = {};
  volatile bool irqEnabled[kSources] = {};
  bool irqAttached = false;
  IOReturn registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon) APPLE_KEXT_OVERRIDE;
  IOReturn unregisterInterrupt(int source) APPLE_KEXT_OVERRIDE;
  IOReturn getInterruptType(int source, int *interruptType) APPLE_KEXT_OVERRIDE {
    if (source < 0 || source >= kSources || !interruptType) return kIOReturnNoInterrupt;
    *interruptType = kIOInterruptTypeLevel; return kIOReturnSuccess;
  }
  IOReturn enableInterrupt(int source) APPLE_KEXT_OVERRIDE;
  IOReturn disableInterrupt(int source) APPLE_KEXT_OVERRIDE;
  IOReturn causeInterrupt(int) APPLE_KEXT_OVERRIDE { return kIOReturnUnsupported; }
};
OSDefineMetaClassAndStructors(CoachzUsbNub, IOService);

namespace {

// ArmCpu enables an SPI when it is attached and has no way to mask it: the distributor's set/clear-enable registers do that here.
volatile uint32_t *gGicd;
void gic_enable(bool on) {
  if (!gGicd) return;
  gGicd[((on ? 0x100 : 0x180) + (kXhciIntid / 32) * 4) / 4] = 1u << (kXhciIntid % 32);
  __asm__ volatile("dsb sy" ::: "memory");
}

volatile uint32_t *gIrqRegs;   // the capability page and the runtime registers after it
uint32_t gIrqRuntime;
IOSimpleLock *gIrqLock;
uint64_t gIrqLine, gIrqSecondary[CoachzUsbNub::kSources], gIrqShared;
bool interrupter_pending(int i) { return gIrqRegs && (gIrqRegs[(gIrqRuntime + 0x20u + 0x20u * static_cast<uint32_t>(i)) / 4] & 1u); }   // IMAN.IP
// Called with gIrqLock held and interrupts off.
void dispatch(CoachzUsbNub *nub, int source) {
  const int use = nub->irqHandler[source] ? source : 0;
  if (!nub->irqEnabled[use] || !nub->irqHandler[use]) return;
  if (use != source) ++gIrqShared;
  nub->irqHandler[use](nub->irqTarget[use], nub->irqRef[use], nub, use);
}
void dispatch_secondary(CoachzUsbNub *nub) {
  for (int i = 1; i < CoachzUsbNub::kSources; ++i)
    if (interrupter_pending(i)) { ++gIrqSecondary[i]; dispatch(nub, i); }
}
void xhci_interrupt(void *context) {
  CoachzUsbNub *nub = static_cast<CoachzUsbNub *>(context);
  if (!nub->irqEnabled[0] || !nub->irqHandler[0]) { gic_enable(false); return; }
  IOSimpleLockLock(gIrqLock);
  ++gIrqLine;
  dispatch(nub, 0);
  dispatch_secondary(nub);
  IOSimpleLockUnlock(gIrqLock);
}
void trace_state_sample();
void interrupter_poll(void *context, wait_result_t) {
  CoachzUsbNub *nub = static_cast<CoachzUsbNub *>(context);
  for (;;) {
    IOSleep(1);
    trace_state_sample();
    bool any = false;
    for (int i = 1; i < CoachzUsbNub::kSources; ++i) any |= interrupter_pending(i);
    if (!any) continue;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gIrqLock);
    dispatch_secondary(nub);
    IOSimpleLockUnlockEnableInterrupt(gIrqLock, state);
  }
}

}  // namespace

IOReturn CoachzUsbNub::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refCon) {
  if (source < 0 || source >= kSources) return kIOReturnNoInterrupt;
  if (irqHandler[source]) return kIOReturnNoResources;
  if (!irqAttached) {
    gIrqLock = IOSimpleLockAlloc();
    gIrqRegs = reinterpret_cast<volatile uint32_t *>(ml_io_map(kXhciBase, 0x1000));
    if (!gIrqLock || !gIrqRegs) return kIOReturnNoResources;
    gIrqRuntime = gIrqRegs[0x18 / 4] & ~0x1fu;
    uint32_t readback = 0; uint64_t router = 0;
    const int status = armcpu_spi_attach(kXhciIntid, false, xhci_interrupt, this, &readback, &router);
    gic_enable(false);
    log("SPI %u attached: 0x%x, readback 0x%x, router 0x%llx; runtime registers at +0x%x", kXhciIntid - 32, status, readback, router,
        gIrqRuntime);
    if (status) return kIOReturnNoResources;
    irqAttached = true;
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(interrupter_poll, this, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else log("no interrupter poll thread");
  }
  const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gIrqLock);
  irqEnabled[source] = false; irqTarget[source] = target; irqRef[source] = refCon; irqHandler[source] = handler;
  IOSimpleLockUnlockEnableInterrupt(gIrqLock, state);
  log("interrupt source %d registered", source);
  return kIOReturnSuccess;
}
IOReturn CoachzUsbNub::unregisterInterrupt(int source) {
  if (source < 0 || source >= kSources || !gIrqLock) return kIOReturnNoInterrupt;
  if (source == 0) gic_enable(false);
  const IOInterruptState state = IOSimpleLockLockDisableInterrupt(gIrqLock);
  irqEnabled[source] = false; irqHandler[source] = nullptr; irqTarget[source] = nullptr; irqRef[source] = nullptr;
  IOSimpleLockUnlockEnableInterrupt(gIrqLock, state);
  return kIOReturnSuccess;
}
IOReturn CoachzUsbNub::enableInterrupt(int source) {
  if (source < 0 || source >= kSources || !irqHandler[source]) return kIOReturnNoInterrupt;
  irqEnabled[source] = true;
  if (source == 0) gic_enable(true);   // the line is interrupter 0's
  return kIOReturnSuccess;
}
IOReturn CoachzUsbNub::disableInterrupt(int source) {
  if (source < 0 || source >= kSources) return kIOReturnNoInterrupt;
  if (source == 0) gic_enable(false);
  irqEnabled[source] = false;
  return kIOReturnSuccess;
}

// ---- the base's keyboard and touchpad: the same route as with the libpayload stack ----
// IOUSBHostHIDDevice drives the Zed's USB side (enumeration, pipes, resets, power) as it would any HID device. Above it, this takes
// the keyboard's and the touchpad's original IOHIDInterface in place of IOHIDEventDriver (a higher probe score, the base's vendor and
// product), opens it with a report action, and hands the raw reports to the decoding the libpayload transport uses: iPadOS gets reports
// 1, 4 and 5 of the one keyboard device either way, and no second copy of the base's events.
class CoachzZedHidBridge : public IOService {
  OSDeclareDefaultStructors(CoachzZedHidBridge);
 public:
  enum Role { kNone, kKeyboard, kTouchpad };
  Role role = kNone;
  IOService *interface = nullptr;
  IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE;
  bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
  void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(CoachzZedHidBridge, IOService);

namespace {

constexpr uint32_t kZedVendor = 0x18d1, kZedProduct = 0x504c;
constexpr unsigned kZedKeyboardInterface = 0, kZedTouchpadInterface = 2;

// The USB interface number of the IOUSBHostInterface below an IOHIDInterface (through its IOUSBHostHIDDevice).
int usb_interface_number(IOService *hid) {
  OSObject *value = hid->copyProperty("bInterfaceNumber", gIOServicePlane, kIORegistryIterateRecursively | kIORegistryIterateParents);
  OSNumber *number = OSDynamicCast(OSNumber, value);
  const int result = number ? static_cast<int>(number->unsigned32BitValue()) : -1;
  OSSafeReleaseNULL(value);
  return result;
}

// IOHIDInterface::InterruptReportAction. handleReport passes the interface or the client as the target: only refCon is used.
void zed_report(OSObject *, uint64_t, IOMemoryDescriptor *report, uint32_t type, uint32_t, void *refCon) {
  CoachzZedHidBridge *bridge = static_cast<CoachzZedHidBridge *>(refCon);
  if (!bridge || !report || type != 0 /* kIOHIDReportTypeInput */) return;
  unsigned char bytes[64];
  const IOByteCount length = report->readBytes(0, bytes, report->getLength() < sizeof bytes ? report->getLength() : sizeof bytes);
  if (bridge->role == CoachzZedHidBridge::kKeyboard) coachz_zed_keyboard(bytes, static_cast<unsigned>(length));
  else if (bridge->role == CoachzZedHidBridge::kTouchpad) coachz_zed_pad(bytes, static_cast<unsigned>(length));
}

// One personality for the base's interfaces; probe keeps the keyboard and the touchpad and leaves any other to IOHIDEventDriver.
bool add_bridge_personality() {
  OSDictionary *personality = OSDictionary::withCapacity(5), *match = OSDictionary::withCapacity(2);
  OSArray *drivers = OSArray::withCapacity(1);
  OSString *cls = OSString::withCString("CoachzZedHidBridge"), *provider = OSString::withCString("IOHIDInterface");
  OSNumber *vendor = OSNumber::withNumber(kZedVendor, 32), *product = OSNumber::withNumber(kZedProduct, 32);
  OSNumber *score = OSNumber::withNumber(100000, 32);
  bool added = false;
  if (personality && match && drivers && cls && provider && vendor && product && score) {
    match->setObject("VendorID", vendor); match->setObject("ProductID", product);
    // No CFBundleIdentifier: an in-kernel driver, allocated by its class name.
    personality->setObject(gIOClassKey, cls); personality->setObject(gIOProviderClassKey, provider);
    personality->setObject(gIOPropertyMatchKey, match); personality->setObject(gIOProbeScoreKey, score);
    drivers->setObject(personality);
    // k197: interface 0 (boot keyboard) matched IOUSBHostHIDDevice's only normal-boot personality, "Generic Keyboard" (class 3,
    // subclass 1, protocol 1), and the bridge took its IOHIDInterface; the touchpad's interface 2 (class 3, subclass 0) got no driver
    // at all, so its IOHIDInterface never existed. Apple's own vendor form (IOUSBHostHIDDeviceSafeBoot "SaitekP2500": idVendor,
    // idProduct, bConfigurationValue, bInterfaceNumber) gives that one interface to the in-kernel IOUSBHostHIDDevice.
    if (OSDictionary *pad = OSDictionary::withCapacity(9)) {
      OSString *bundle = OSString::withCString("com.apple.driver.usb.IOUSBHostHIDDevice");
      OSString *padClass = OSString::withCString("IOUSBHostHIDDevice"), *padProvider = OSString::withCString("IOUSBHostInterface");
      OSString *behavior = OSString::withCString("");
      OSNumber *padInterface = OSNumber::withNumber(kZedTouchpadInterface, 32), *configuration = OSNumber::withNumber(1ULL, 32);
      if (bundle && padClass && padProvider && behavior && padInterface && configuration) {
        pad->setObject("CFBundleIdentifier", bundle); pad->setObject(gIOClassKey, padClass); pad->setObject(gIOProviderClassKey, padProvider);
        pad->setObject("HIDDefaultBehavior", behavior); pad->setObject("idVendor", vendor); pad->setObject("idProduct", product);
        pad->setObject("bConfigurationValue", configuration); pad->setObject("bInterfaceNumber", padInterface);
        drivers->setObject(pad);
      }
      OSSafeReleaseNULL(bundle); OSSafeReleaseNULL(padClass); OSSafeReleaseNULL(padProvider); OSSafeReleaseNULL(behavior);
      OSSafeReleaseNULL(padInterface); OSSafeReleaseNULL(configuration); pad->release();
    }
    added = gIOCatalogue->addDrivers(drivers, true);
  }
  OSSafeReleaseNULL(personality); OSSafeReleaseNULL(match); OSSafeReleaseNULL(drivers); OSSafeReleaseNULL(cls);
  OSSafeReleaseNULL(provider); OSSafeReleaseNULL(vendor); OSSafeReleaseNULL(product); OSSafeReleaseNULL(score);
  return added;
}

}  // namespace

IOService *CoachzZedHidBridge::probe(IOService *provider, SInt32 *score) {
  if (!provider->metaCast("IOHIDInterface")) return nullptr;
  const int number = usb_interface_number(provider);
  role = number == kZedKeyboardInterface ? kKeyboard : number == kZedTouchpadInterface ? kTouchpad : kNone;
  log("base interface %d: %s", number, role == kKeyboard ? "keyboard" : role == kTouchpad ? "touchpad" : "left to IOHIDEventDriver");
  return role == kNone ? nullptr : IOService::probe(provider, score);
}

bool CoachzZedHidBridge::start(IOService *provider) {
  if (!IOService::start(provider)) return false;
  using Open = bool (*)(IOService *, IOService *, IOOptionBits, void (*)(OSObject *, uint64_t, IOMemoryDescriptor *, uint32_t, uint32_t, void *), void *);
  Open open = reinterpret_cast<Open>((*reinterpret_cast<void ***>(provider))[kIOHIDInterfaceOpenSlot]);
  if (!open(provider, this, 0, zed_report, this)) {
    log("base %s: the HID interface did not open", role == kKeyboard ? "keyboard" : "touchpad");
    IOService::stop(provider);
    return false;
  }
  interface = provider;
  log("base %s bridged into the keyboard device", role == kKeyboard ? "keyboard" : "touchpad");
  return true;
}

void CoachzZedHidBridge::stop(IOService *provider) {
  if (interface) {
    interface->close(this);
    interface = nullptr;
    coachz_zed_release(role == kKeyboard, role == kTouchpad);
    unsigned reports, actions, touches, clicks;
    coachz_zed_counts(&reports, &actions, &touches, &clicks);
    log("base %s detached: keyboard %u reports, %u action key changes; touchpad %u reports, %u clicks",
        role == kKeyboard ? "keyboard" : "touchpad", reports, actions, touches, clicks);
  }
  IOService::stop(provider);
}

namespace {

// ---- the controller: an original AppleNubT8011USBXHCI with AppleUSBXHCI's platform slots ----
bool (*gXhciStart)(IOService *, IOService *);
bool controller_start(IOService *self, IOService *provider) {
  // T8011's start clears the cache mode of the buffer options (its controller snoops); this one does not, so the rings and contexts
  // are write-combined: Normal non-cacheable. kIOMapRealTimeCache would be Device memory in this kernel.
  volatile uint32_t *options = reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(self) + kAppleUSBHostControllerCacheOptions);
  const uint32_t before = *options;
  *options = (before & ~static_cast<uint32_t>(kIOMapCacheMask)) | kIOMapWriteCombineCache;
  log("controller start: buffer options 0x%x -> 0x%x", before, *options);
  const bool started = gXhciStart(self, provider);
  log("controller start returned %d", started);
  // No changePowerStateTo floor: IOUSBHostFamily turns powerOverrideOnPriv on, so only its own changePowerStateToPriv counts (k192
  // set 3 and the controller still went to 0). The real cause was the flat power tree (ArmDTPlatform.patch).
  return started;
}

// Read only, k188: every register access the driver makes goes through AppleNubT8011USBXHCI's slots 290 (read32: this, window, offset)
// and 292 (write32: this, window, offset, value), 103 and 26 call sites in AppleUSBXHCI. k185 and k187 both had the two root ports in
// U3 (PORTSC PLS 3) while GET_DESCRIPTOR waited, after AppleUSBXHCIPort::powerOn had read PORTSC as 0 for its 10 ms. This logs each
// write and each read whose value differs from the last read of that register, by offset from the controller base and with the time.
constexpr unsigned kXhciRead32Slot = 290, kXhciRead64Slot = 291, kXhciWrite32Slot = 292, kXhciWrite64Slot = 293, kTraceLines = 4000;
extern "C" uint64_t kvtophys(vm_offset_t va);
extern "C" uint64_t mach_absolute_time(void);
uint32_t (*gXhciRead32)(IOService *, volatile void *, uint32_t);
void (*gXhciWrite32)(IOService *, volatile void *, uint32_t, uint32_t);
uint64_t (*gXhciRead64)(IOService *, volatile void *, uint32_t);
void (*gXhciWrite64)(IOService *, volatile void *, uint32_t, uint64_t);
uint32_t gTraceLast[kXhciBytes / 4];
uint8_t gTraceSeen[kXhciBytes / 4];
volatile uint32_t gTraceCount;
int64_t trace_offset(volatile void *window, uint32_t offset) {
  const uint64_t pa = kvtophys(reinterpret_cast<vm_offset_t>(window) + offset);
  return pa >= kXhciBase && pa < kXhciBase + kXhciBytes ? static_cast<int64_t>(pa - kXhciBase) : -1;
}
// k188 printed only with interrupts on and so saw no PORTSC access at all: the port code runs with them off. Entries now go to a
// buffer, any context, and the monitor prints them (trace_flush).
struct TraceEntry { uint64_t ticks; int32_t at; uint32_t offset, value; char kind; volatile bool ready; };
TraceEntry gTraceRing[kTraceLines];
uint32_t gTracePrinted;
void trace(char kind, int64_t at, uint32_t offset, uint32_t value) {
  const uint32_t i = __atomic_fetch_add(&gTraceCount, 1, __ATOMIC_RELAXED);
  if (i >= kTraceLines) return;
  TraceEntry &e = gTraceRing[i];
  e.ticks = mach_absolute_time(); e.at = static_cast<int32_t>(at); e.offset = offset; e.value = value; e.kind = kind;
  __atomic_store_n(&e.ready, true, __ATOMIC_RELEASE);
}
// The driver's register helpers (AppleUSBXHCI, e.g. at 0xfffffff005ec0ac8) read nothing and return 0 unless the controller's word at
// +0x250 is 1, which is how powerOn could report PORTSC 0: its changes go into the same trace, sampled every millisecond.
constexpr size_t kDriverState = 0x250;
IOService *volatile gTraceController;
void trace_state_sample() {
  static uint32_t last = ~0u;
  IOService *controller = gTraceController;
  if (!controller) return;
  const uint32_t v = *reinterpret_cast<volatile uint32_t *>(reinterpret_cast<uint8_t *>(controller) + kDriverState);
  if (v != last) { last = v; trace('S', -2, kDriverState, v); }
}
void trace_flush() {
  const uint32_t end = __atomic_load_n(&gTraceCount, __ATOMIC_RELAXED);
  for (; gTracePrinted < end && gTracePrinted < kTraceLines; ++gTracePrinted) {
    const TraceEntry &e = gTraceRing[gTracePrinted];
    if (!__atomic_load_n(&e.ready, __ATOMIC_ACQUIRE)) break;
    const uint64_t us = e.ticks * 10 / 192;   // the SC7180 generic timer runs at 19.2 MHz
    if (e.kind == 'S') log("rw %llu.%03llu ms driver state +0x%x = %u", us / 1000, us % 1000, e.offset, e.value);
    else if (e.kind == 'P' || e.kind == 'p') log("rw %llu.%03llu ms %c port %u PORTSC 0x%08x", us / 1000, us % 1000, e.kind, e.offset, e.value);
    else if (e.at >= 0) log("rw %llu.%03llu ms %c +0x%x 0x%08x", us / 1000, us % 1000, e.kind, static_cast<unsigned>(e.at), e.value);
    else log("rw %llu.%03llu ms %c window offset 0x%x 0x%08x (outside the controller)", us / 1000, us % 1000, e.kind, e.offset, e.value);
  }
  if (end > kTraceLines && gTracePrinted == kTraceLines) { log("rw trace full: %u more accesses not kept", end - kTraceLines); ++gTracePrinted; }
}
uint32_t controller_read32(IOService *self, volatile void *window, uint32_t offset) {
  const uint32_t value = gXhciRead32(self, window, offset);
  const int64_t at = trace_offset(window, offset);
  if (at < 0 || (at & 3)) { trace('R', at, offset, value); return value; }
  const size_t i = static_cast<size_t>(at) / 4;
  if (!gTraceSeen[i] || gTraceLast[i] != value) { gTraceSeen[i] = 1; gTraceLast[i] = value; trace('r', at, offset, value); }
  return value;
}
void controller_write32(IOService *self, volatile void *window, uint32_t offset, uint32_t value) {
  trace('w', trace_offset(window, offset), offset, value);
  gXhciWrite32(self, window, offset, value);
}
// 64-bit registers (CRCR, DCBAAP, ERSTBA, ERDP): the low word as 'Q'/'q', the high word as 'H' at offset + 4. Reads are kept only when
// either half changed, as for 32-bit reads.
uint64_t controller_read64(IOService *self, volatile void *window, uint32_t offset) {
  const uint64_t value = gXhciRead64(self, window, offset);
  const int64_t at = trace_offset(window, offset);
  const size_t i = at >= 0 && !(at & 7) ? static_cast<size_t>(at) / 4 : 0;
  if (!i || !gTraceSeen[i] || gTraceLast[i] != static_cast<uint32_t>(value) || gTraceLast[i + 1] != static_cast<uint32_t>(value >> 32)) {
    if (i) { gTraceSeen[i] = 1; gTraceLast[i] = static_cast<uint32_t>(value); gTraceLast[i + 1] = static_cast<uint32_t>(value >> 32); }
    trace('q', at, offset, static_cast<uint32_t>(value)); trace('H', at >= 0 ? at + 4 : at, offset + 4, static_cast<uint32_t>(value >> 32));
  }
  return value;
}
void controller_write64(IOService *self, volatile void *window, uint32_t offset, uint64_t value) {
  const int64_t at = trace_offset(window, offset);
  trace('Q', at, offset, static_cast<uint32_t>(value)); trace('H', at >= 0 ? at + 4 : at, offset + 4, static_cast<uint32_t>(value >> 32));
  gXhciWrite64(self, window, offset, value);
}

// Read only, k191: AppleUSBXHCIPort reads and writes its PORTSC itself, through a pointer at +0x3a8 and only while the controller's
// +0x598 check passes (port vtable +0x770 read at 0xfffffff005ee0c80, +0x780 write at 0xfffffff005ee0cf0), so k189's slot trace saw no
// PORTSC access at all while both root ports went to U3 before GET_DESCRIPTOR. Each port gets a copy of its vtable with these two
// wrapped: writes always, reads when the value changes, in the same buffer as the controller's accesses ('P' port write, 'p' read).
constexpr unsigned kPortRead = 0x770 / 8, kPortWrite = 0x780 / 8, kPortTableSlots = 512, kPortsTraced = 4;
struct TracedPort { void *port; uint32_t (*read)(void *); void (*write)(void *, uint32_t); uint32_t last; bool seen; };
TracedPort gPorts[kPortsTraced];
int traced_port(void *port) { for (int i = 0; i < static_cast<int>(kPortsTraced); ++i) if (gPorts[i].port == port) return i; return -1; }
uint32_t port_read(void *port) {
  const int i = traced_port(port);
  if (i < 0) return 0;
  const uint32_t value = gPorts[i].read(port);
  if (!gPorts[i].seen || gPorts[i].last != value) { gPorts[i].seen = true; gPorts[i].last = value; trace('p', -3, static_cast<uint32_t>(i), value); }
  return value;
}
void port_write(void *port, uint32_t value) {
  const int i = traced_port(port);
  if (i < 0) return;
  trace('P', -3, static_cast<uint32_t>(i), value);
  gPorts[i].write(port, value);
}
void trace_ports(IOService *controller) {
  OSIterator *children = controller->getChildIterator(gIOServicePlane);
  if (!children) return;
  int count = 0;
  while (OSObject *child = children->getNextObject()) {
    IOService *port = OSDynamicCast(IOService, child);
    if (!port || !strnstr(port->getMetaClass()->getClassName(), "XHCI", 64) || !strnstr(port->getMetaClass()->getClassName(), "Port", 64) || count >= static_cast<int>(kPortsTraced)) continue;
    void **own = *reinterpret_cast<void ***>(port);
    void **table = static_cast<void **>(IOMalloc(kPortTableSlots * sizeof(void *)));
    if (!table) break;
    memcpy(table, own, kPortTableSlots * sizeof(void *));
    gPorts[count] = {port, reinterpret_cast<uint32_t (*)(void *)>(own[kPortRead]), reinterpret_cast<void (*)(void *, uint32_t)>(own[kPortWrite]), 0, false};
    table[kPortRead] = reinterpret_cast<void *>(port_read);
    table[kPortWrite] = reinterpret_cast<void *>(port_write);
    __atomic_store_n(reinterpret_cast<void ***>(port), table, __ATOMIC_RELEASE);
    log("port %d traced: %s %s", count, port->getMetaClass()->getClassName(), port->getName());
    ++count;
  }
  children->release();
}

// k192/k195 (PORTSC traced): about 2 ms after each port reset AppleUSBXHCIPort::suspend wrote PORTSC | 0x10060 (LWS, PLS U3) on the root
// ports with the controller running; the hub's resume (PLS 15) was never driven to U0, and every later transfer hung. The ports sat at
// power state 2 under the root domain: ArmDTPlatform's PMRegisterDevice (patches/drivers/ArmDTPlatform.patch) now builds the tree as
// AppleARMPE does, and UsbForcePower (tried in k195, before the tree was right) is no longer set.
// Every service under the controller with its power-plane parent and state: with ArmDTPlatform's AppleARMPE-style PMRegisterDevice the
// ports should sit under the controller and each IOUSBHostDevice under its port (before: all under the root domain).
void log_power_tree(IOService *controller) {
  IORegistryIterator *it = IORegistryIterator::iterateOver(controller, gIOServicePlane, kIORegistryIterateRecursively);
  if (!it) return;
  unsigned shown = 0;
  while (IORegistryEntry *entry = it->getNextObject()) {
    IOService *service = OSDynamicCast(IOService, entry);
    if (!service || !service->inPlane(gIOPowerPlane) || shown++ >= 40) continue;
    const char *parent = "none";
    if (IORegistryEntry *connection = service->getParentEntry(gIOPowerPlane))
      if (IORegistryEntry *above = connection->getParentEntry(gIOPowerPlane)) parent = above->getName();
    log("power tree: %s (%s) under %s, power %ld", service->getName(), service->getMetaClass()->getClassName(), parent,
        static_cast<long>(service->getPowerState()));
  }
  it->release();
}
// k196: both hubs and the base (18d1:504c, AppleUSBHostCompositeDevice configuration 1) enumerated, yet the bridge was never probed in
// 26 s and IOUSBHostHIDDevice left nothing in the unified log. Every service under the controller in the service plane, depth by
// indentation, with its class, name, inactive state, busy count and (in the power plane) power state: where the interfaces and their
// drivers stop.
void log_service_tree(IOService *controller, unsigned tick) {
  IORegistryIterator *it = IORegistryIterator::iterateOver(controller, gIOServicePlane, kIORegistryIterateRecursively);
  if (!it) return;
  unsigned shown = 0;
  const unsigned base = controller->getDepth(gIOServicePlane);
  while (IORegistryEntry *entry = it->getNextObject()) {
    if (shown++ >= 80) { log("tree %u: more entries not shown", tick); break; }
    IOService *service = OSDynamicCast(IOService, entry);
    const unsigned depth = entry->getDepth(gIOServicePlane) - base;
    log("tree %u: %.*s%s (%s)%s busy %u power %ld", tick, static_cast<int>(depth > 12 ? 12 : depth) * 2, "                        ",
        entry->getName(), entry->getMetaClass()->getClassName(), service && service->isInactive() ? " inactive" : "",
        service ? static_cast<unsigned>(service->getBusyState()) : 0u,
        service && service->inPlane(gIOPowerPlane) ? static_cast<long>(service->getPowerState()) : -1L);
  }
  it->release();
}
void log_port_power(const char *when) {
  for (int i = 0; i < static_cast<int>(kPortsTraced); ++i)
    if (IOService *port = static_cast<IOService *>(gPorts[i].port)) log("%s: port %d power %ld PORTSC 0x%08x", when, i,
                                                                         static_cast<long>(port->getPowerState()), gPorts[i].last);
}

IOService *make_controller(CoachzUsbNub *nub) {
  OSObject *object = OSMetaClass::allocClassWithName("AppleNubT8011USBXHCI");
  IOService *controller = OSDynamicCast(IOService, object);
  if (!controller) { OSSafeReleaseNULL(object); log("no AppleNubT8011USBXHCI instance"); return nullptr; }
  void **own = *reinterpret_cast<void ***>(controller);
  void **table = static_cast<void **>(IOMalloc(kAppleNubT8011USBXHCISlots * sizeof(void *)));
  if (!table) { controller->release(); return nullptr; }
  memcpy(table, own, kAppleNubT8011USBXHCISlots * sizeof(void *));
  for (unsigned slot : kAppleNubT8011USBXHCIReplaced) table[slot] = coachz_original_xhci_table[slot];
  gXhciStart = reinterpret_cast<bool (*)(IOService *, IOService *)>(coachz_original_xhci_table[kAppleUSBXHCIStartSlot]);
  table[kAppleUSBXHCIStartSlot] = reinterpret_cast<void *>(controller_start);
  gXhciRead32 = reinterpret_cast<uint32_t (*)(IOService *, volatile void *, uint32_t)>(table[kXhciRead32Slot]);
  gXhciWrite32 = reinterpret_cast<void (*)(IOService *, volatile void *, uint32_t, uint32_t)>(table[kXhciWrite32Slot]);
  table[kXhciRead32Slot] = reinterpret_cast<void *>(controller_read32);
  table[kXhciWrite32Slot] = reinterpret_cast<void *>(controller_write32);
  gXhciRead64 = reinterpret_cast<uint64_t (*)(IOService *, volatile void *, uint32_t)>(table[kXhciRead64Slot]);
  gXhciWrite64 = reinterpret_cast<void (*)(IOService *, volatile void *, uint32_t, uint64_t)>(table[kXhciWrite64Slot]);
  table[kXhciRead64Slot] = reinterpret_cast<void *>(controller_read64);
  gTraceController = controller;
  table[kXhciWrite64Slot] = reinterpret_cast<void *>(controller_write64);
  *reinterpret_cast<void ***>(controller) = table;

  // The controller's properties are the original personality's (stream, LPM and request policies among them).
  OSDictionary *personality = nullptr;
  if (OSDictionary *match = OSDictionary::withCapacity(1)) {
    if (OSString *name = OSString::withCString("AppleNubT8011USBXHCI")) {
      match->setObject(gIOClassKey, name);
      SInt32 generation = 0;
      if (OSOrderedSet *found = gIOCatalogue->findDrivers(match, &generation)) {
        if (OSDictionary *first = OSDynamicCast(OSDictionary, found->getFirstObject())) personality = OSDictionary::withDictionary(first);
        found->release();
      }
      name->release();
    }
    match->release();
  }
  log("controller instance made, personality %d", personality != nullptr);
  const bool initialized = controller->init(personality);
  OSSafeReleaseNULL(personality);
  if (!initialized) { log("controller init failed"); controller->release(); return nullptr; }
  if (!controller->attach(nub)) { log("controller attach failed"); controller->release(); return nullptr; }
  if (!controller->start(nub)) { log("controller did not start"); controller->detach(nub); controller->release(); return nullptr; }
  return controller;
}

// Read only: the controller's run state and its ports (xHCI 1.1 sections 5.3 to 5.5), beside its power state.
volatile uint32_t *gXhci;
uint32_t xhci32(uint32_t offset) { return gXhci[offset / 4]; }

void log_controller(IOService *controller) {
  if (!gXhci) gXhci = reinterpret_cast<volatile uint32_t *>(ml_io_map(kXhciBase, kXhciBytes));
  if (!gXhci) return;
  const uint32_t op = xhci32(0) & 0xff, rt = xhci32(0x18) & ~0x1fu, ports = xhci32(4) >> 24;
  // k177: address device worked, the first EP0 transfer timed out and the controller then raised HCE. The rings' addresses as the
  // controller holds them, and DWC3's own bus error record (dwc3/core.h: GSTS bit 4 BUSERRADDRVLD, GBUSERRADDR), against the
  // mapper's table below.
  const uint32_t usbsts = xhci32(op + 4), gsts = xhci32(0xc118);
  log("xhci HCSPARAMS2 0x%x CRCR 0x%x DCBAAP 0x%x%08x ERSTSZ %u ERSTBA 0x%x%08x ERDP 0x%x%08x",
      xhci32(8), xhci32(op + 0x18), xhci32(op + 0x34), xhci32(op + 0x30), xhci32(rt + 0x28), xhci32(rt + 0x34), xhci32(rt + 0x30),
      xhci32(rt + 0x3c), xhci32(rt + 0x38));
  log("dwc3 GSTS 0x%x GBUSERRADDR 0x%x%08x GSBUSCFG0 0x%x GSBUSCFG1 0x%x GCTL 0x%x", gsts, xhci32(0xc134), xhci32(0xc130),
      xhci32(0xc100), xhci32(0xc104), xhci32(0xc110));
  static bool dumped;
  if (!dumped && ((usbsts & ((1u << 2) | (1u << 12))) || (gsts & (1u << 4)))) {
    dumped = true;
    // k178: no DWC3 bus error and no SMMU fault, ERDP stuck after 17 events, the first EP0 transfer timed out. The controller does
    // not snoop (no dma-coherent in sc7180.dtsi) and the original driver, written for coherent T8011, does no cache maintenance: a
    // shared page the CPU maps cacheable would explain both directions. Each range's CPU attribute (VM_WIMG: 0x2 cacheable, 0x6
    // write-combined), then the event ring at ERDP read through the kernel's cacheable view and through an uncached one.
    IOLockLock(gMapLock);
    for (size_t i = 0; i < gRangeCount && i < 64; ++i)
      log("mapped %zu: iova 0x%llx pa 0x%llx bytes 0x%llx wimg 0x%x", i, gRanges[i].iova, gRanges[i].physical, gRanges[i].bytes,
          pmap_cache_attributes(static_cast<ppnum_t>(gRanges[i].physical >> 12)));
    IOLockUnlock(gMapLock);
  }
  char text[160]; size_t used = 0;
  for (uint32_t i = 0; i < ports && i < 4 && used < sizeof text; ++i)
    used += static_cast<size_t>(snprintf(text + used, sizeof text - used, " 0x%x", xhci32(op + 0x400 + 0x10 * i)));
  if (!used) text[0] = 0;
  log("controller power %ld in plane %d; USBCMD 0x%x USBSTS 0x%x CONFIG 0x%x IMAN 0x%x 0x%x 0x%x ERSTSZ %u %u %u; %u ports PORTSC%s",
      controller ? static_cast<long>(controller->getPowerState()) : -1L, controller ? controller->inPlane(gIOPowerPlane) : 0,
      xhci32(op), xhci32(op + 4), xhci32(op + 0x38), xhci32(rt + 0x20), xhci32(rt + 0x40), xhci32(rt + 0x60), xhci32(rt + 0x28),
      xhci32(rt + 0x48), xhci32(rt + 0x68), ports, text);
  // Registers only: where each interrupter's handler has consumed its event ring to (ERDP, EHB in bit 3) and its moderation.
  log("interrupters ERDP 0x%x 0x%x 0x%x IMOD 0x%x 0x%x 0x%x", xhci32(rt + 0x38), xhci32(rt + 0x58), xhci32(rt + 0x78), xhci32(rt + 0x24),
      xhci32(rt + 0x44), xhci32(rt + 0x64));
}

// Read only, k186: what the controller and the driver hold in memory around the first EP0 transfer. k185 enumerated both root ports
// (Enable Slot and Address Device completed, 17 events on interrupter 0), then GET_DESCRIPTOR timed out on both devices, the next
// Stop Endpoint got no completion and the ports went to U3, with no SMMU fault, no HSE/HCE and every interrupter's IP clear.
// Interrupters (xHCI 5.5.2), their first event ring segment table entry and the event TRBs at ERDP, then for slots 1 and 2 the
// output slot and EP0 contexts (6.2) and the EP0 transfer ring at its dequeue pointer. Memory is read through the physical aperture:
// the driver's buffer pages are already write-combined there (pmap_set_cache_attributes changes it too), and a second Device
// mapping of RAM (ml_io_map) is a mismatched alias, which hung eMMC CMD25 in k181.
uint64_t iova_to_pa(uint64_t iova) {
  if (iova < kIovaBase || iova >= kIovaBase + kIovaPages * kPage) return 0;
  const uint32_t ppn = __atomic_load_n(&gShadow[(iova - kIovaBase) / kPage], __ATOMIC_ACQUIRE);
  return ppn ? (static_cast<uint64_t>(ppn) * kPage) | (iova & (kPage - 1)) : 0;
}
// One 32-bit word at a device address, or false when it is not mapped.
bool dma32(uint64_t iova, uint32_t *value) {
  const uint64_t pa = iova_to_pa(iova);
  if (!pa || (iova & 3)) return false;
  *value = *reinterpret_cast<volatile uint32_t *>(phystokv(pa));
  return true;
}
uint64_t dma64(uint64_t iova) {
  uint32_t lo = 0, hi = 0;
  if (!dma32(iova, &lo) || !dma32(iova + 4, &hi)) return ~0ull;
  return (static_cast<uint64_t>(hi) << 32) | lo;
}
void dump_trbs(const char *what, uint64_t iova, int before, int after) {
  for (int i = -before; i <= after; ++i) {
    const uint64_t at = (iova & ~0xfull) + static_cast<int64_t>(i) * 16;
    uint32_t w[4] = {};
    bool ok = true;
    for (int j = 0; j < 4 && ok; ++j) ok = dma32(at + 4u * static_cast<unsigned>(j), &w[j]);
    if (!ok) { log("%s %+d at 0x%llx: not mapped", what, i, at); continue; }
    // TRB type in dword 3 bits 15:10, cycle bit 0; for transfer TRBs the interrupter target is dword 2 bits 31:22.
    log("%s %+d at 0x%llx: %08x %08x %08x %08x type %u c%u", what, i, at, w[0], w[1], w[2], w[3], (w[3] >> 10) & 0x3f, w[3] & 1);
  }
}
void dump_rings(const char *when) {
  if (!gXhci) return;
  const uint32_t op = xhci32(0) & 0xff, rt = xhci32(0x18) & ~0x1fu, hcc1 = xhci32(0x10);
  const unsigned ctx = (hcc1 & 4) ? 64 : 32;
  log("dump %s: USBSTS 0x%x CRCR 0x%x HCCPARAMS1 0x%x context %u bytes, PORTSC 0x%x 0x%x", when, xhci32(op + 4), xhci32(op + 0x18),
      hcc1, ctx, xhci32(op + 0x400), xhci32(op + 0x410));
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t ir = rt + 0x20 + 0x20 * i;
    const uint64_t erstba = (static_cast<uint64_t>(xhci32(ir + 0x14)) << 32) | xhci32(ir + 0x10);
    const uint64_t erdp = (static_cast<uint64_t>(xhci32(ir + 0x1c)) << 32) | xhci32(ir + 0x18);
    const uint64_t segment = dma64(erstba);
    uint32_t size = 0; dma32(erstba + 8, &size);
    log("dump %s: interrupter %u IMAN 0x%x IMOD 0x%x ERSTSZ %u ERSTBA 0x%llx ERDP 0x%llx; segment 0x%llx size %u", when, i,
        xhci32(ir), xhci32(ir + 4), xhci32(ir + 8), erstba, erdp, segment, size & 0xffff);
    char label[32]; snprintf(label, sizeof label, "dump %s ev%u", when, i);
    if (segment != ~0ull) dump_trbs(label, erdp & ~0xfull, 2, 4);
  }
  const uint64_t dcbaap = (static_cast<uint64_t>(xhci32(op + 0x34)) << 32) | xhci32(op + 0x30);
  for (uint32_t slot = 1; slot <= 2; ++slot) {
    const uint64_t out = dma64(dcbaap + 8 * slot);
    if (out == ~0ull || !out) { log("dump %s: slot %u no output context (0x%llx)", when, slot, out); continue; }
    uint32_t s[4] = {}, e[5] = {};
    for (int j = 0; j < 4; ++j) dma32(out + 4u * static_cast<unsigned>(j), &s[j]);
    for (int j = 0; j < 5; ++j) dma32(out + ctx + 4u * static_cast<unsigned>(j), &e[j]);
    const uint64_t deq = ((static_cast<uint64_t>(e[3]) << 32) | e[2]) & ~0xfull;
    // Slot state dword 3 bits 31:27 (0 disabled, 1 default, 2 addressed, 3 configured); EP state dword 0 bits 2:0 (1 running,
    // 2 halted, 3 stopped, 4 error); dequeue cycle state dword 2 bit 0.
    log("dump %s: slot %u context 0x%llx slot %08x %08x %08x %08x state %u; ep0 %08x %08x %08x %08x %08x state %u dcs %u", when, slot,
        out, s[0], s[1], s[2], s[3], s[3] >> 27, e[0], e[1], e[2], e[3], e[4], e[0] & 7, e[2] & 1);
    char label[32]; snprintf(label, sizeof label, "dump %s ep0.%u", when, slot);
    dump_trbs(label, deq, 4, 3);
  }
}

// The PM state as ioreg shows it ("IOPowerManagement": current, desired, the driver's, the device's and the children's requests,
// the machine state) and the power-plane parent: k176 had the nub in the plane and on, the controller at state 0 with its registers
// initialized and run/stop clear, and the hub connected on both ports.
void log_pm(IOService *service, const char *label) {
  if (!service) return;
  const char *parent = "none";
  if (IORegistryEntry *connection = service->getParentEntry(gIOPowerPlane))
    if (IORegistryEntry *above = connection->getParentEntry(gIOPowerPlane)) parent = above->getName();
  OSSerialize *text = OSSerialize::withCapacity(512);
  OSObject *pm = service->copyProperty("IOPowerManagement");
  if (text && pm) pm->serialize(text);
  const char *body = text && pm ? text->text() : "(no IOPowerManagement)";
  const size_t length = strlen(body);
  for (size_t at = 0; at < length && at < 600; at += 120) log("%s pm (parent %s) %.120s", label, parent, body + at);
  OSSafeReleaseNULL(pm); OSSafeReleaseNULL(text);
}

void monitor(IOService *controller) {
  for (unsigned tick = 0;; ++tick) {
    IOSleep(tick < 30 ? 2000 : 10000);
    trace_flush();
    // k185 timing: monitor starts about a second after makeUsable, GET_DESCRIPTOR timed out about 4.3 s after it and the Stop
    // Endpoint 2 s later: ticks 0 and 1 see the transfer pending, tick 3 the state after both timeouts.
    if (tick == 0) dump_rings("pending");
    if (tick == 1) dump_rings("late");
    if (tick == 3) dump_rings("after");
    uint32_t fsr = 0, fsynr = 0; uint64_t far = 0;
    smmu_read32(&gSmmu, gCbOffset + kCbFsr, &fsr);
    // FSR[10:9] is FORMAT (0b10, AArch64), not a fault and not write-1-to-clear: only TF..UUT, SS and MULTI are faults.
    fsr &= 0xc00001feu;
    if (fsr) {
      smmu_read64(&gSmmu, gCbOffset + kCbFar, &far); smmu_read32(&gSmmu, gCbOffset + kCbFsynr0, &fsynr);
      smmu_write32(&gSmmu, gCbOffset + kCbFsr, fsr);
    }
    const uint32_t gfsr = gr0(kGr0Sgfsr), gfsynr = gfsr ? gr0(kGr0Sgfsynr0) : 0;
    if (gfsr) gr0_write(kGr0Sgfsr, gfsr);
    if (fsr || gfsr || tick < 30 || tick % 6 == 0) {
      log("maps %llu unmaps %llu failed %llu live pages %llu ranges %zu, write-combined %zu now %llu made %llu full, irqs %llu line %llu, "
          "interrupter 1 %llu 2 %llu (to source 0 %llu), bank FSR 0x%x FAR 0x%llx FSYNR0 0x%x, sGFSR 0x%x/0x%x", gMaps, gUnmaps, gMapFailures,
          gLivePages, gRangeCount, gUncachedCount, gUncachedPages, gUncachedFull, armcpu_spi_count(kXhciIntid), gIrqLine, gIrqSecondary[1],
          gIrqSecondary[2], gIrqShared, fsr, far, fsynr, gfsr, gfsynr);
      if (tick < 5 || tick % 6 == 0) log_controller(controller);
      log_port_power("monitor");
      if (controller && (tick == 3 || tick == 10 || tick == 20 || tick == 29 || tick % 6 == 0)) log_service_tree(controller, tick);
      if (tick == 1 || tick == 10) {
        log_pm(controller, "controller"); log_pm(controller ? controller->getProvider() : nullptr, "nub");
        for (const TracedPort &t : gPorts) if (t.port) log_pm(static_cast<IOService *>(t.port), "port");
        if (controller) log_power_tree(controller);
      }
    }
  }
}

void host_thread(void *, wait_result_t) {
  if (OSDictionary *resource = IOService::resourceMatching("IOKit")) {
    IOService *found = IOService::waitForMatchingService(resource, 60ULL * NSEC_PER_SEC);
    resource->release();
    OSSafeReleaseNULL(found);
  }
  const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.usb.AppleUSBHostT8011");
  log("AppleUSBHostT8011 load 0x%x", static_cast<unsigned>(loaded));
  if (loaded != kOSReturnSuccess) return;
  if (!smmu_start()) return;
  if (!coachz_usb_host_soc_init()) { log("SoC bring-up failed, the controller stays off"); return; }
  // Before the controller: the base enumerates right after it, and its interfaces must find the bridge's personality.
  if (coachz_usb_keyboard_requested()) log("base HID bridge personality %d", add_bridge_personality());
  else log("coachz-usb-keyboard is off: the base stays with IOHIDEventDriver");
  gGicd = reinterpret_cast<volatile uint32_t *>(ml_io_map(kGicdBase, 0x1000));
  IOService *platform = IOService::getPlatform();
  CoachzUsbMapper *mapper = OSTypeAlloc(CoachzUsbMapper);
  if (!platform || !gGicd || !mapper || !mapper->init() || !mapper->attach(platform)) { log("mapper not made"); return; }
  mapper->setName("coachz-usb-mapper");
  if (!mapper->start(platform)) { log("mapper did not start"); return; }
  CoachzUsbNub *nub = OSTypeAlloc(CoachzUsbNub);
  if (!nub || !nub->init()) { log("nub not made"); return; }
  nub->setName("coachz-usb-host");
  nub->setProperty("iommu-parent", mapper);
  if (OSArray *memory = OSArray::withCapacity(1)) {
    if (IODeviceMemory *range = IODeviceMemory::withRange(kXhciBase, kXhciBytes)) { memory->setObject(range); range->release(); }
    nub->setDeviceMemory(memory);
    memory->release();
  }
  if (!nub->attach(platform) || !nub->start(platform)) { log("nub not attached"); return; }
  nub->registerService();
  // A power node under the root domain, as an Apple ARM nub is under its bus. changePowerStateTo only runs on a node in the power
  // plane whose parents know their state (IOServicePM.cpp): without this the controller's power-on request was dropped, start built
  // the rings (10 maps, 2 interrupts) and the controller never ran, so no port ever reported.
  static IOPMPowerState nubStates[2] = {
    {kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
    {kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0},
  };
  nub->PMinit();
  platform->joinPMtree(nub);
  const IOReturn registered = nub->registerPowerDriver(nub, nubStates, 2);
  const IOReturn powered = nub->changePowerStateTo(1);
  log("nub power: register 0x%x, on 0x%x, in plane %d", registered, powered, nub->inPlane(gIOPowerPlane));
  IOService *controller = make_controller(nub);
  log("controller %s", controller ? "started" : "not started");
  if (controller) {
    log_pm(controller, "controller at start");
    IOSleep(5000);
    trace_ports(controller);
    if (controller->getPowerState() == 0) {
      // Still off with the nub on (k176): ask for the controller's usable state as a client would, once, and show what PM did.
      log_pm(controller, "controller still off");
      const IOReturn usable = controller->makeUsable();
      IOSleep(1000);
      log("makeUsable 0x%x: controller power %ld", usable, static_cast<long>(controller->getPowerState()));
      log_controller(controller);
    }
  }
  monitor(controller);
}

}  // namespace

extern "C" int coachz_usb_host_enabled(void) {
  static int enabled = -1;
  if (enabled < 0) { unsigned value = 0; enabled = PE_parse_boot_argn("coachz-usb-host", &value, sizeof value) && value == 1; }
  return enabled;
}

// From NativeRamBlock::start on the eMMC root, in place of the libpayload keyboard stack: the two cannot share the controller.
extern "C" void coachz_usb_host_start(void) {
  thread_t thread = THREAD_NULL;
  if (kernel_thread_start(host_thread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
  else log("no thread");
}
