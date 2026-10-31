// SPDX-License-Identifier: BSD-2-Clause-Patent
// The audio service iOS's audio server plug-in opens (CoachzAudioPlugIn.c, interface in CoachzAudioAbi.h). The user client and the host
// clock fix are adapted from UniXNU-sheng package/ShengAudioService.cpp, revision 08cde20 (BSD-2-Clause-Patent, Copyright (c) 2026,
// UniXNU contributors). There is no audio DSP in this path: LPASS read DMA loops over the output ring itself, so the DMA position is the
// play position. All kernel memory lies above 4 GiB and this DMA takes 32 bits, so the ring is low DRAM outside the kernel's window.
// CoachzSpeaker.cpp has powered LPASS and started its clocks; a timer clears each frame once it has played.
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <mach/kmod.h>
#include "CoachzAudioAbi.h"

extern kmod_info_t g_kernel_kmod_info;
extern "C" {
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
vm_offset_t phystokv(uint64_t pa);
uint64_t pmap_find_pa(void *pmap, uint64_t va);
void coachz_audio_static_init(void) __asm__("__GLOBAL__sub_I_CoachzAudio.cpp");
void coachz_audio_start(void);
}

namespace {
const uint64_t kLpass = 0x62000000;
const uint32_t kBitBranch = 0x0d11018, kModeMux = 0x0d9e000, kI2sCtl = 0x0f02000, kPcmI2sSel = 0x0f02200;
const uint32_t kIrqEn = 0x0f0a000, kIrqClear = 0x0f0a00c;
const uint32_t kDmaCtl = 0x0f0d000, kDmaBase = 0x0f0d004, kDmaBufLen = 0x0f0d008, kDmaCurr = 0x0f0d00c, kDmaPerLen = 0x0f0d010, kDmaBaseExt = 0x0f0d040;
const uint32_t kReset = 1u << 31, kSpkrEn = 1u << 16, kSpkrMode = 1u << 11, kLongRate = 0xfu << 18;
const uint32_t kDmaFlags = (1u << 16) | (7u << 1) | (2u << 12) | (1u << 20) | (1u << 21);
const uint32_t kIrqAll = 7u << 3;
const uint64_t kAmpCfg = 0x03d00000 + 23 * 0x1000;   // TLMM GPIO23, the amplifier's SD_MODE, on the SOUTH tile
const uint64_t kRingPa = 0x81200000;                 // just past the USB DMA pool; no reserved-memory node covers it
const uint32_t kRingBytes = COACHZ_AUDIO_RING_FRAMES * 4;
const uint32_t kInputBytes = COACHZ_AUDIO_RING_FRAMES * 2;
// Ahead of the DMA, for the audio server's late IO cycles on this CPU. There is no queue behind it, so this is all of the latency.
const uint32_t kLeadFrames = 2400;
const uint32_t kTickMs = 10;

struct Page {uint64_t pa; vm_offset_t va;};
Page pages[8];
unsigned pageCount;

volatile uint32_t *reg(uint64_t pa) {
	const uint64_t base = pa & ~0xfffull;
	for (unsigned i = 0; i < pageCount; i++) {
		if (pages[i].pa == base) {
			return reinterpret_cast<volatile uint32_t *>(pages[i].va + (pa & 0xfff));
		}
	}
	if (pageCount == sizeof(pages) / sizeof(pages[0])) {
		return nullptr;
	}
	const vm_offset_t va = ml_io_map(base, 0x1000);
	if (!va) {
		return nullptr;
	}
	pages[pageCount++] = {base, va};
	return reinterpret_cast<volatile uint32_t *>(va + (pa & 0xfff));
}
uint32_t rd(uint32_t offset) {return *reg(kLpass + offset);}
void wr(uint32_t offset, uint32_t value) {*reg(kLpass + offset) = value; __asm__ volatile ("dsb sy" ::: "memory");}

void amplifier(bool on) {
	volatile uint32_t *cfg = reg(kAmpCfg), *io = reg(kAmpCfg + 4);
	*io = (*io & ~2u) | (on ? 2u : 0u);
	*cfg = (*cfg & ~0x3ffu) | 0x200u;
	__asm__ volatile ("dsb sy" ::: "memory");
}

IOBufferMemoryDescriptor *controlMemory, *inputMemory;
IOMemoryDescriptor *outputMemory;
CoachzAudioControl *control;
volatile uint32_t *ring;
thread_call_t tickCall;
uint64_t ticksPerFrame, wraps, cleared;
uint32_t lastIndex;
bool anchored;
uint32_t outputOn;
uint32_t badReads, adjustments;

// The play position from the DMA, the clearing behind it, and the one timeline's anchor, corrected only when it has really moved.
void tick(thread_call_param_t, thread_call_param_t) {
	const uint64_t now = mach_absolute_time();
	const uint32_t current = rd(kDmaCurr);
	const uint32_t index = (current - (uint32_t)kRingPa) / 4;
	if (index < COACHZ_AUDIO_RING_FRAMES) {
		if (index < lastIndex) {
			wraps += COACHZ_AUDIO_RING_FRAMES;
		}
		lastIndex = index;
		const uint64_t position = wraps + index;
		const uint64_t anchor = now - position * ticksPerFrame;
		const uint64_t held = control->out_anchor_host;
		const uint64_t drift = anchor > held ? anchor - held : held - anchor;
		if (!anchored || drift > 480 * ticksPerFrame) {
			control->out_seq = control->out_seq + 1;
			__asm__ volatile ("dmb ish" ::: "memory");
			control->out_anchor_host = anchor;
			control->in_anchor_host = anchor;
			__asm__ volatile ("dmb ish" ::: "memory");
			control->out_seq = control->out_seq + 1;
			if (anchored) {
				adjustments++;
			}
			anchored = true;
		}
		for (; cleared < position; cleared++) {
			ring[cleared % COACHZ_AUDIO_RING_FRAMES] = 0;
		}
		control->out_frames = position;
		control->in_frames = position;
		control->in_written = position;   // the input ring stays silent
	} else if (!badReads++) {
		printf("COACHZ audio: DMA position 0x%x outside the ring\n", current);
	}
	uint64_t deadline;
	clock_interval_to_deadline(kTickMs, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(tickCall, deadline);
}

// CoreAudio takes the host clock to be 24 MHz, the timer of Apple's chips: AudioGetHostClockFrequency and the audio server's IO clock read
// one constant, and on this board's 19.2 MHz timer the audio server falls behind. The constant lives in the dyld shared cache, on a page
// with code, so the caller cannot write it; this sets the shared page itself. Only a 24 MHz value inside the shared region is changed.
IOReturn setHostClock(task_t task, uint64_t address) {
	constexpr uint64_t kApple = 0x4176e36000000000ULL;   // 24000000.0
	if ((address & 7) || address < 0x180000000ULL || address >= 0x400000000ULL) {
		return kIOReturnBadArgument;
	}
	IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
	if (!memory) {
		return kIOReturnNoMemory;
	}
	IOReturn result = memory->prepare();
	if (result != kIOReturnSuccess) {
		memory->release();
		return result;
	}
	uint64_t seen = 0;
	result = copyin(address, &seen, 8) ? kIOReturnNotReadable : kIOReturnNoResources;
	const uint64_t pa = result == kIOReturnNoResources ? pmap_find_pa(get_task_pmap(task), address) : 0;
	if (pa) {
		volatile uint64_t *value = reinterpret_cast<volatile uint64_t *>(phystokv(pa));
		mach_timebase_info_data_t timebase;
		clock_timebase_info(&timebase);
		// The timer frequency as a double, built from the integer (no floating point here).
		const uint64_t hz = 1000000000ULL * timebase.denom / timebase.numer;
		const unsigned top = 63 - (unsigned)__builtin_clzll(hz);
		const uint64_t bits = (uint64_t)(1023 + top) << 52 | ((hz << (52 - top)) & ((1ULL << 52) - 1));
		result = kIOReturnUnsupported;
		if (*value == seen && (seen == kApple || seen == bits)) {
			*value = bits;
			__asm__ volatile ("dsb sy" ::: "memory");
			result = kIOReturnSuccess;
		}
	}
	memory->complete();
	memory->release();
	printf("COACHZ audio: host clock request 0x%x\n", result);
	return result;
}
}  // namespace

class CoachzAudio : public IOService {
	OSDeclareDefaultStructors(CoachzAudio)
public:
	bool startAudio() {
		// iOS's audio server is sandboxed against unknown user clients; the kernel lets these processes open this service.
		setProperty("NativeSandboxGrantProcess", "audiomxd,mediaserverd");
		setProperty("IOUserClientClass", "CoachzAudioUserClient");
		registerService();
		return true;
	}
};
OSDefineMetaClassAndStructors(CoachzAudio, IOService)

class CoachzAudioUserClient : public IOUserClient {
	OSDeclareDefaultStructors(CoachzAudioUserClient)
	task_t owner = nullptr;
public:
	bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override {
		if (!IOUserClient::initWithTask(task, security, type, properties)) {
			return false;
		}
		owner = task;
		return true;
	}
	IOReturn clientClose() override {
		// The plug-in's process went away or closed: nothing plays from its ring any more.
		outputOn = 0;
		amplifier(false);
		terminate();
		return kIOReturnSuccess;
	}
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override {
		IOMemoryDescriptor *chosen = type == CoachzAudioMemoryControl ? controlMemory : type == CoachzAudioMemoryOutput ? outputMemory :
		    type == CoachzAudioMemoryInput ? inputMemory : nullptr;
		if (!chosen) {
			return kIOReturnBadArgument;
		}
		chosen->retain();   // the caller consumes the reference
		// The output ring is outside the kernel's memory, where the default is Device memory: the audio server's mixing needs Normal memory.
		*options = type == CoachzAudioMemoryOutput ? kIOMapWriteCombineCache : 0;
		*memory = chosen;
		return kIOReturnSuccess;
	}
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) override {
		if (selector == CoachzAudioSetHostClock && args->scalarInputCount == 1 && !args->structureInputSize) {
			return setHostClock(owner, args->scalarInput[0]);
		}
		if (selector > CoachzAudioStopInput || args->scalarInputCount || args->structureInputSize) {
			return kIOReturnBadArgument;
		}
		if (selector == CoachzAudioStartOutput || selector == CoachzAudioStopOutput) {
			const uint32_t on = selector == CoachzAudioStartOutput;
			if (on != outputOn) {
				outputOn = on;
				amplifier(on);
				control->state = on ? control->state | CoachzAudioOutputRunning : control->state & ~(uint32_t)CoachzAudioOutputRunning;
				printf("COACHZ audio: output %s at frame %llu, %u adjustments\n", on ? "on" : "off", control->out_frames, adjustments);
			}
		}
		return kIOReturnSuccess;
	}
};
OSDefineMetaClassAndStructors(CoachzAudioUserClient, IOUserClient)

// Called by CoachzSpeaker.cpp (coachz-speaker=2) once LPASS has power and its clocks run, in place of the test tone.
extern "C" void coachz_audio_start(void) {
	const vm_offset_t ringVa = ml_io_map_wcomb(kRingPa, kRingBytes);
	controlMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, (IOOptionBits)kIODirectionInOut | kIOMemoryKernelUserShared, 4096, 4096);
	inputMemory = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, (IOOptionBits)kIODirectionInOut | kIOMemoryKernelUserShared, kInputBytes, 4096);
	outputMemory = IOMemoryDescriptor::withPhysicalAddress(kRingPa, kRingBytes, kIODirectionInOut);
	tickCall = thread_call_allocate(tick, nullptr);
	if (!ringVa || !controlMemory || !inputMemory || !outputMemory || !tickCall || !reg(kLpass + kDmaCtl) || !reg(kLpass + kI2sCtl) ||
	    !reg(kLpass + kIrqEn) || !reg(kLpass + kBitBranch) || !reg(kLpass + kModeMux) || !reg(kAmpCfg)) {
		printf("COACHZ audio: no memory or mappings\n");
		return;
	}
	ring = reinterpret_cast<volatile uint32_t *>(ringVa);
	for (uint32_t i = 0; i < COACHZ_AUDIO_RING_FRAMES; i++) {
		ring[i] = 0;
	}
	bzero(controlMemory->getBytesNoCopy(), 4096);
	bzero(inputMemory->getBytesNoCopy(), kInputBytes);
	control = static_cast<CoachzAudioControl *>(controlMemory->getBytesNoCopy());
	mach_timebase_info_data_t timebase;
	clock_timebase_info(&timebase);
	const uint64_t hz = 1000000000ULL * timebase.denom / timebase.numer;
	ticksPerFrame = hz / COACHZ_AUDIO_RATE;
	control->magic = COACHZ_AUDIO_MAGIC;
	control->version = COACHZ_AUDIO_VERSION;
	control->rate = COACHZ_AUDIO_RATE;
	control->out_channels = COACHZ_AUDIO_OUT_CHANNELS;
	control->in_channels = COACHZ_AUDIO_IN_CHANNELS;
	control->period_frames = COACHZ_AUDIO_PERIOD_FRAMES;
	control->ring_frames = COACHZ_AUDIO_RING_FRAMES;
	control->lead_frames = kLeadFrames;
	control->ticks_per_frame_q32 = ticksPerFrame << 32;
	// The read DMA and the interface, as the factory driver sets them up; the speaker's amplifier stays off until the plug-in starts.
	amplifier(false);
	wr(kModeMux, 0);
	wr(kDmaCtl, 0);
	wr(kDmaBase, 0);
	wr(kDmaBufLen, 0);
	wr(kDmaPerLen, 0);
	wr(kDmaCtl, kReset);
	wr(kDmaCtl, 0);
	wr(kPcmI2sSel, 0);
	wr(kI2sCtl, kSpkrMode | kLongRate | kReset);
	wr(kI2sCtl, rd(kI2sCtl) & ~kReset);
	wr(kDmaBase, (uint32_t)kRingPa);
	wr(kDmaBaseExt, (uint32_t)(kRingPa >> 32));
	wr(kDmaBufLen, COACHZ_AUDIO_RING_FRAMES - 1);   // in 32-bit words, minus one
	wr(kDmaPerLen, COACHZ_AUDIO_RING_FRAMES / 2 - 1);
	wr(kDmaCtl, kDmaFlags);
	wr(kDmaCtl, rd(kDmaCtl) | 1);
	wr(kIrqClear, kIrqAll);
	wr(kIrqEn, 0);                                  // polled by the timer
	wr(kBitBranch, 1);
	wr(kI2sCtl, rd(kI2sCtl) | kSpkrEn);
	control->state = CoachzAudioOutputReady | CoachzAudioInputReady;
	tick(nullptr, nullptr);
	// Registered now rather than at boot, like the other late classes here.
	const OSSymbol *name = OSSymbol::withCString("CoachzAudio");
	const bool absent = name && !OSMetaClass::getMetaClassWithName(name);
	OSSafeReleaseNULL(name);
	void *handle = absent ? OSMetaClass::preModLoad(g_kernel_kmod_info.name) : nullptr;
	if (!handle) {
		printf("COACHZ audio: class not registered\n");
		return;
	}
	coachz_audio_static_init();
	const OSReturn meta = OSMetaClass::postModLoad(handle);
	CoachzAudio *service = meta ? nullptr : OSTypeAlloc(CoachzAudio);
	IOService *provider = IOService::getResourceService();
	const bool attached = service && service->init() && service->attach(provider);
	const bool running = attached && service->start(provider) && service->startAudio();
	printf("COACHZ audio: service meta 0x%x started %d, DMA position 0x%x, ring at 0x%llx, %llu ticks a frame\n", meta, running,
	    rd(kDmaCurr), kRingPa, ticksPerFrame);
	OSSafeReleaseNULL(service);
}
