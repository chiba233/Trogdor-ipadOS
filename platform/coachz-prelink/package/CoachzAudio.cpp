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
#include <kern/thread.h>
#include <mach/kmod.h>
#include "CoachzAudioAbi.h"
#include "CoachzMicXnu.hpp"

extern kmod_info_t g_kernel_kmod_info;
extern "C" {
extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t, vm_size_t);
vm_offset_t phystokv(uint64_t pa);
uint64_t pmap_find_pa(void *pmap, uint64_t va);
void coachz_audio_static_init(void) __asm__("__GLOBAL__sub_I_CoachzAudio.cpp");
void coachz_audio_start(void);
kern_return_t kernel_thread_start_priority(thread_continue_t, void *, integer_t, thread_t *);
void thread_deallocate(thread_t);
int coachz_rpmh_vreg_request(unsigned consumer, const char *resource, uint32_t mv, int hpm, int enable);
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
// The capture DMA (Primary MI2S into WRDMA5, CoachzMicCore.hpp): low DRAM like the ring, in no pool and no reserved-memory node. The apps
// SMMU is in bypass (sCR0 CLIENTPD), so LPASS's capture stream sees it at this physical address. The speaker's RDMA1 stays as it is.
const uint64_t kMicDmaPa = 0x81300000;
const unsigned kMicRailConsumer = 3;                 // coachz_rpmh_vreg_request's identity for this rail vote
// The capture's worker runs here. Under full load a 10 ms thread call came more than 64 ms late, so it has a thread of its own above user work.
const integer_t kTickPriority = 91;
// After a failed capture a client's recording starts again, with a hole in it, no more often than this.
const uint32_t kMicRestartTicks = 50;
// One short capture at boot, before any client: a second after the service starts, for three seconds. Only counters and peaks are kept.
const uint32_t kMicTestStartTick = 100, kMicTestStopTick = 400;

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
uint64_t ticksPerFrame, wraps, cleared;
uint32_t lastIndex;
bool anchored;
uint32_t outputOn;
uint32_t badReads, adjustments;

CoachzMic::XnuBinding *microphone;
CoachzMicShared *micStatus;
void *micOwner;
uint32_t micTicks, micRestartWait, micRestarts;
uint64_t micWorstGap;

// ADAU7002's IOVDD, pp1800_l15a. A refused enable takes its own vote back, so the rail is not left counted for this consumer.
int micSupply(void *, bool on) {
	const int result = coachz_rpmh_vreg_request(kMicRailConsumer, "ldoa15", 1800, 1, on);
	if (result && on) {
		const int rollback = coachz_rpmh_vreg_request(kMicRailConsumer, "ldoa15", 1800, 1, 0);
		if (rollback) {
			printf("COACHZ mic: rail vote rollback failed (%d)\n", rollback);
		}
	}
	return result;
}

void micReport(const char *why) {
	printf("COACHZ mic: %s: state %u error %u generation %u frames %llu first %llu written %llu peak %u/%u nonzero %u/%u irq %u/%u misses %u restarts %u worst tick gap %llu ms\n",
	    why, micStatus->state, micStatus->error, micStatus->generation, micStatus->frames_copied, micStatus->first, micStatus->written,
	    micStatus->peak[0], micStatus->peak[1], micStatus->nonzero[0], micStatus->nonzero[1], micStatus->irq_periods, micStatus->irq_faults,
	    reinterpret_cast<CoachzAudioDiag *>(reinterpret_cast<uint8_t *>(control) + COACHZ_AUDIO_DIAG_OFFSET)->in_misses, micRestarts, ticksPerFrame ? micWorstGap / (ticksPerFrame * 48) : 0);
	micWorstGap = 0;
}

void micState(bool running) {
	if (running) {
		__atomic_fetch_or(&control->state, (uint32_t)CoachzAudioInputRunning, __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_and(&control->state, ~(uint32_t)CoachzAudioInputRunning, __ATOMIC_RELAXED);
	}
}

// The first run with no client: cursor rate, signal and clean stop, whatever the audio server later does.
void micSelfTest(uint64_t now) {
	static uint64_t began;
	if (micOwner || !microphone->ready()) {
		return;
	}
	if (micTicks == kMicTestStartTick) {
		began = now;
		const int started = microphone->start(0);
		printf("COACHZ mic: self test start %d, DMA at 0x%llx, ring %u frames\n", started, kMicDmaPa, COACHZ_MIC_DMA_FRAMES);
	} else if (micTicks == kMicTestStopTick) {
		micReport("self test");
		const uint64_t frames = micStatus->frames_copied, ticks = now - began;
		printf("COACHZ mic: self test %llu frames over %llu ms from the codec's 80 ms wake-up on\n", frames, ticks / (ticksPerFrame * 48));
		const int stopped = microphone->stop();
		printf("COACHZ mic: self test stop %d, state %u\n", stopped, micStatus->state);
	}
}

// The play position from the DMA, the clearing behind it, and the one timeline's anchor, corrected only when it has really moved.
void tick(void *, void *) {
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
		// The input timeline shares the playback anchor; the capture's own valid range is in CoachzMicShared.
		if (microphone) {
			if (micTicks <= kMicTestStopTick) {
				micSelfTest(now);
				micTicks++;
			}
			static uint64_t lastTick;
			const uint64_t polledAt = mach_absolute_time();
			if (lastTick && polledAt - lastTick > micWorstGap) {
				micWorstGap = polledAt - lastTick;
			}
			lastTick = polledAt;
			const int polled = microphone->poll(polledAt, control->out_anchor_host, ticksPerFrame);
			static int lastPolled;
			if (polled != lastPolled) {
				lastPolled = polled;
				micReport(polled ? "capture failed" : "capture recovered");
				if (polled) {
					micState(false);
				}
			}
			if (polled && micOwner && ++micRestartWait >= kMicRestartTicks) {
				micRestartWait = 0;
				micRestarts++;
				const int started = microphone->start(0);
				if (started && micRestarts <= 8) {
					printf("COACHZ mic: restart %u refused: %d\n", micRestarts, started);
				}
			}
			control->in_frames = micStatus->frames_copied;
			control->in_written = micStatus->written;
			if (micOwner && micStatus->state == CoachzMicRunning) {
				micState(true);
				static uint32_t reportTicks;
				if (!(++reportTicks % 1000)) {
					micReport("running");
				}
			}
		}
	} else if (!badReads++) {
		printf("COACHZ audio: DMA position 0x%x outside the ring\n", current);
	}
}

void tickThread(void *, wait_result_t) {
	for (;;) {
		tick(nullptr, nullptr);
		IOSleep(kTickMs);
	}
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
		// The plug-in's process went away or closed: nothing plays from its ring any more, and nothing captures for it.
		outputOn = 0;
		amplifier(false);
		if (microphone && micOwner == this) {
			microphone->stop();
			micOwner = nullptr;
			micState(false);
		}
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
		if (selector == CoachzAudioStartInput) {
			if (!microphone || !microphone->ready()) {
				return kIOReturnUnsupported;
			}
			if (micOwner && micOwner != this) {
				return kIOReturnBusy;
			}
			if (micOwner == this) {
				return kIOReturnSuccess;
			}
			const int started = microphone->start(0);
			printf("COACHZ mic: input on: %d, state %u\n", started, micStatus->state);
			if (started) {
				micReport("start refused");
				return kIOReturnError;
			}
			micOwner = this;   // InputRunning follows once the 80 ms wake-up has passed
			return kIOReturnSuccess;
		}
		if (selector == CoachzAudioStopInput) {
			if (micOwner == this) {
				micReport("input off");
				microphone->stop();
				micOwner = nullptr;
				micState(false);
			}
			return kIOReturnSuccess;
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
	if (!ringVa || !controlMemory || !inputMemory || !outputMemory || !reg(kLpass + kDmaCtl) || !reg(kLpass + kI2sCtl) ||
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
	control->state = CoachzAudioOutputReady;
	// LPASS has power and its clocks run: the capture's resources can be taken. A failure leaves playback alone and Ready unset.
	microphone = new CoachzMic::XnuBinding;
	micStatus = reinterpret_cast<CoachzMicShared *>(reinterpret_cast<uint8_t *>(control) + COACHZ_MIC_CONTROL_OFFSET);
	if (microphone && microphone->prepare(kMicDmaPa, COACHZ_MIC_DMA_BYTES, true, true, micStatus, static_cast<int16_t *>(inputMemory->getBytesNoCopy()),
	    COACHZ_AUDIO_RING_FRAMES, micSupply, nullptr)) {
		control->state = CoachzAudioOutputReady | CoachzAudioInputReady;
	} else {
		printf("COACHZ mic: not prepared\n");
		microphone = nullptr;
	}
	thread_t tickWorker = nullptr;
	if (kernel_thread_start_priority(tickThread, nullptr, kTickPriority, &tickWorker) == KERN_SUCCESS) {
		thread_deallocate(tickWorker);
	} else {
		printf("COACHZ audio: no worker thread\n");
	}
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
