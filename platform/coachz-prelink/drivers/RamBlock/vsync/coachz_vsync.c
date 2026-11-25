/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The panel's hardware vsync for the display driver (NativeLegacyDisplay, GopFramebuffer.patch), in place of its 60 Hz software tick.
 *
 * Mainline Linux (v6.12): the display controller's interface for this board's DSI output is INTF_1, whose VSYNC interrupt is bit 27 of
 * the MDP's top interrupt registers (dpu_6_2_sc7180.h: intf_1 .intr_vsync = DPU_IRQ_IDX(MDP_SSPP_TOP0_INTR, 27)); those registers sit at
 * INTR_EN 0x010, INTR_STATUS 0x014 and INTR_CLEAR 0x018 of the MDP block (dpu_hwio.h, dpu_intr_set_legacy), at 0x0ae01000
 * (sc7180.dtsi mdp). The MDP raises hardware interrupt 0 of the MDSS wrapper (msm_mdss.c: HW_INTR_STATUS 0x0010 at 0x0ae00000), which
 * reaches the GIC as SPI 83, level high (sc7180.dtsi mdss). The handler follows dpu_core_irq: read the status, clear what was read,
 * keep only the enabled bits. Only the VSYNC bit is enabled here; the scanout flip (coachz_scanout_flip) keeps polling its own latch.
 */
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <machine/machine_routines.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);   /* ArmCpu.cpp, IOReturn */

int coachz_hw_vsync_register(void (*callback)(void *, uint64_t), void *context);
uint64_t coachz_hw_vsync_count(void);
uint64_t coachz_hw_vsync_stamp(void);

#define MDSS_PA          0x0ae00000u
#define MDSS_HW_INTR_STATUS 0x0010u
#define MDP_PA           0x0ae01000u
#define MDP_INTR_EN      0x010u
#define MDP_INTR_STATUS  0x014u
#define MDP_INTR_CLEAR   0x018u
#define INTF_1_VSYNC     (1u << 27)
#define MDSS_SPI         83u
#define MDSS_INTID       (32u + MDSS_SPI)

static volatile uint8_t *vs_mdss, *vs_mdp;
static void (*vs_callback)(void *, uint64_t);
static void *vs_context;
static thread_call_t vs_call;
static volatile uint64_t vs_count, vs_last, vs_delivered;

static uint32_t rd(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(uintptr_t)(b + o); }
static void wr(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(uintptr_t)(b + o) = v; __asm__ volatile ("dsb sy" ::: "memory"); }

uint64_t coachz_hw_vsync_count(void) { return vs_count; }
/* mach_absolute_time of the last vsync interrupt, 0 before the first: the GPU's deadline boost (CoachzGpu.cpp) aims at the next one. */
uint64_t coachz_hw_vsync_stamp(void) { return __atomic_load_n(&vs_last, __ATOMIC_ACQUIRE); }

/* Thread context: the display driver's work. One delivery per pending vsync is enough; the driver stamps it with the interrupt's time. */
static void vs_deliver(thread_call_param_t p0, thread_call_param_t p1)
{
	(void)p0;
	(void)p1;
	vs_delivered++;
	if (vs_delivered % 600u == 1u) {
		printf("COACHZ vsync: %llu interrupts, %llu delivered\n", (unsigned long long)vs_count, (unsigned long long)vs_delivered);
	}
	if (vs_callback) vs_callback(vs_context, __atomic_load_n(&vs_last, __ATOMIC_ACQUIRE));
}

/* Primary interrupt context (ArmCpu's SPI dispatch). */
static void vs_irq(void *context)
{
	(void)context;
	if (!(rd(vs_mdss, MDSS_HW_INTR_STATUS) & 1u)) return;
	uint32_t status = rd(vs_mdp, MDP_INTR_STATUS);
	const uint32_t enabled = rd(vs_mdp, MDP_INTR_EN);
	if (status) wr(vs_mdp, MDP_INTR_CLEAR, status);
	status &= enabled;
	if (status & INTF_1_VSYNC) {
		__atomic_store_n(&vs_last, mach_absolute_time(), __ATOMIC_RELEASE);
		vs_count++;
		if (vs_call) thread_call_enter(vs_call);
	}
}

/* 0 when the hardware vsync runs and `callback` gets each one (in a thread, with the interrupt's mach_absolute_time); otherwise the
 * caller keeps its own tick. Called once, by the primary display. */
int coachz_hw_vsync_register(void (*callback)(void *, uint64_t), void *context)
{
	static int registered;
	if (registered || !callback) return -1;
	vs_mdss = (volatile uint8_t *)ml_io_map(MDSS_PA, 0x1000u);
	vs_mdp = (volatile uint8_t *)ml_io_map(MDP_PA, 0x1000u);
	vs_call = thread_call_allocate_with_priority(vs_deliver, NULL, THREAD_CALL_PRIORITY_KERNEL_HIGH);
	if (!vs_mdss || !vs_mdp || !vs_call) {
		printf("COACHZ vsync: not mapped\n");
		return -2;
	}
	vs_callback = callback;
	vs_context = context;
	/* Stale status first, as dpu_hw_intr_init's clear_all does, then the one enable bit. */
	wr(vs_mdp, MDP_INTR_CLEAR, 0xffffffffu);
	uint32_t readback = 0;
	uint64_t router = 0;
	const int attached = armcpu_spi_attach(MDSS_INTID, false, vs_irq, NULL, &readback, &router);
	if (attached) {
		printf("COACHZ vsync: SPI %u not attached: 0x%x\n", MDSS_SPI, (unsigned)attached);
		vs_callback = NULL;
		return -3;
	}
	wr(vs_mdp, MDP_INTR_EN, rd(vs_mdp, MDP_INTR_EN) | INTF_1_VSYNC);
	registered = 1;
	printf("COACHZ vsync: INTF_1 vsync on SPI %u, MDP INTR_EN 0x%x, GIC readback 0x%x router 0x%llx\n", MDSS_SPI,
	    rd(vs_mdp, MDP_INTR_EN), readback, (unsigned long long)router);
	return 0;
}
