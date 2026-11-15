/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The early kernel log, kept across a warm reset for boots that stop before the disk log opens.
 *
 * Depthcharge (src/boot/ramoops.c) adds /reserved-memory/ramoops from coreboot's CBMEM entry and marks it used: a warm reset keeps
 * its contents and neither XNU nor Linux allocates it. The boot probe (boot/xnu_screen.c) checks it against the XNU window, the
 * framebuffer and the payload and leaves "CZRAMOOP", base and size at boot_args page + 0x800, which arm_init reads before it copies
 * the boot args (coachz_early_note_ramoops). Here, from kernel_bootstrap_thread once thread calls work (coachz-prelink-early-log.patch):
 *   - map it (Device memory, outside XNU's RAM: byte stores and aligned u64 stores only);
 *   - if the previous boot's header is valid and its head is past what reached the disk log (stored), keep up to 64 KiB of that
 *     tail; coachz_early_log_report prints it with "@prev " once the disk log is open, so it lands in the log ring;
 *   - start a fresh header, turn the byte mirror in coachz_early.c on and copy in what the capture ring already holds.
 * Header: u64 magic "CZEARLY1", head and stored (offsets of coachz_log_capture), data size; data from +4096, the largest power of
 * two that fits. NativeRamBlock.cpp reports stored after each successful log_store. If no store has happened 150 s after boot, the
 * kernel prints that and resets through PSCI SYSTEM_RESET (smc, as the device tree's /psci says), which keeps DRAM.
 * Debian's kernel has no pstore: only the next iPadOS boot reads this back.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern void delay(int usec);
/* osfmk/arm64/coachz_early.c */
extern uint64_t coachz_ramoops_base, coachz_ramoops_size;
extern volatile uint8_t *volatile coachz_early_mirror;
extern volatile uint64_t *coachz_early_mirror_head;
extern uint64_t coachz_early_mirror_mask;
extern char coachz_log_capture[];
extern volatile uint64_t coachz_log_head;

void coachz_early_log_start(void);
void coachz_early_log_stored(uint64_t stored);
void coachz_early_log_report(void);

#define EL_HEADER    4096u
#define EL_CAPTURE   (1ull << 20)   /* coachz_log_capture */
#define EL_PREV_MAX  (64u << 10)
#define EL_WATCHDOG_S 150u
#define EL_MAGIC     0x31594c5241455a43ull   /* "CZEARLY1", little-endian */
#define PSCI_SYSTEM_RESET 0x84000009ull

static volatile uint64_t *el_header;   /* [0] magic, [1] head, [2] stored, [3] data size */
static uint8_t el_prev[EL_PREV_MAX];
static uint32_t el_prev_bytes;
static volatile uint32_t el_stored_seen, el_reported;
static thread_call_t el_watchdog;

static void
el_watchdog_fire(thread_call_param_t p0, thread_call_param_t p1)
{
	(void)p0;
	(void)p1;
	if (__atomic_load_n(&el_stored_seen, __ATOMIC_ACQUIRE)) {
		return;
	}
	printf("COACHZ early log: no disk log after %u s, warm reset\n", EL_WATCHDOG_S);
	delay(200000);
	__asm__ volatile ("dsb sy" ::: "memory");
	register uint64_t x0 __asm__("x0") = PSCI_SYSTEM_RESET;
	/* SMCCC: x0-x17 may change across the call. */
	__asm__ volatile ("mov x1, xzr\n mov x2, xzr\n mov x3, xzr\n smc #0" : "+r"(x0) : :
	    "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	printf("COACHZ early log: SYSTEM_RESET returned %lld\n", (long long)x0);
}

void
coachz_early_log_start(void)
{
	const uint64_t base = coachz_ramoops_base, size = coachz_ramoops_size;
	if (!base || (base & 4095) || size < EL_HEADER + (64u << 10)) {
		printf("COACHZ early log: no ramoops buffer from the probe\n");
		return;
	}
	uint64_t data = 1;
	while (data * 2 <= size - EL_HEADER) {
		data *= 2;
	}
	const vm_offset_t va = ml_io_map((vm_offset_t)base, (vm_size_t)size);
	if (!va) {
		printf("COACHZ early log: ramoops at 0x%llx not mapped\n", base);
		return;
	}
	volatile uint64_t *h = (volatile uint64_t *)va;
	volatile uint8_t *d = (volatile uint8_t *)(va + EL_HEADER);

	/* What the boot before left past its last disk store. */
	const uint64_t magic = h[0], head = h[1], stored = h[2], dsize = h[3];
	if (magic == EL_MAGIC && dsize == data && head > stored) {
		uint64_t from = stored;
		if (head - from > EL_PREV_MAX) {
			from = head - EL_PREV_MAX;
		}
		if (head - from > data) {
			from = head - data;
		}
		for (uint64_t i = from; i < head; i++) {
			el_prev[el_prev_bytes++] = d[i & (data - 1)];
		}
	}

	/* A fresh header, invalid until the capture ring is copied in; the mirror runs from before the copy. */
	h[0] = 0;
	h[2] = 0;
	h[3] = data;
	coachz_early_mirror_mask = data - 1;
	coachz_early_mirror_head = &h[1];
	__atomic_store_n(&coachz_early_mirror, d, __ATOMIC_RELEASE);
	const uint64_t now = coachz_log_head, span = data < EL_CAPTURE ? data : EL_CAPTURE;
	for (uint64_t i = now > span ? now - span : 0; i < now; i++) {
		d[i & (data - 1)] = (uint8_t)coachz_log_capture[i & (EL_CAPTURE - 1)];
	}
	const uint64_t latest = coachz_log_head;
	if (h[1] < latest) {
		h[1] = latest;
	}
	h[0] = EL_MAGIC;
	__asm__ volatile ("dsb sy" ::: "memory");
	el_header = h;
	printf("COACHZ early log: ramoops at 0x%llx, %llu KiB mirrored; the boot before left %u bytes the disk log did not get\n",
	    base, data >> 10, el_prev_bytes);

	el_watchdog = thread_call_allocate(el_watchdog_fire, NULL);
	if (el_watchdog) {
		uint64_t deadline;
		clock_interval_to_deadline(EL_WATCHDOG_S, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(el_watchdog, deadline);
	}
}

/* NativeRamBlock.cpp, after each successful log_store: everything before this capture offset is on the disk. */
void
coachz_early_log_stored(uint64_t stored)
{
	__atomic_store_n(&el_stored_seen, 1, __ATOMIC_RELEASE);
	if (el_header) {
		el_header[2] = stored;
	}
}

/* NativeRamBlock.cpp, once the disk log is open: the previous boot's tail, a line at a time, into this boot's log. */
void
coachz_early_log_report(void)
{
	if (!el_prev_bytes || __atomic_exchange_n(&el_reported, 1, __ATOMIC_ACQ_REL)) {
		return;
	}
	printf("COACHZ early log: %u bytes from the boot before, after its last disk store\n", el_prev_bytes);
	char line[160];
	unsigned n = 0;
	for (uint32_t i = 0; i < el_prev_bytes; i++) {
		const uint8_t c = el_prev[i];
		if (c != '\n') {
			line[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
		}
		if (c == '\n' || n == sizeof(line) || i + 1 == el_prev_bytes) {
			printf("@prev %.*s\n", (int)n, line);
			n = 0;
		}
	}
	el_prev_bytes = 0;
}
