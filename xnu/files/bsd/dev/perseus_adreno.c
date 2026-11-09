/*
 * The GPU of the Xiaomi Mi 8 (dipper, SDM845): an Adreno 630 (chip id 0x06030001, "qcom,chipid" of the phone's
 * own device tree), woken step by step. Only with boot-arg mi8gpu=N, which performs steps 1 to N and stops.
 * Nothing here is touched without it. bsd/dev/redwood_adreno.c is the POCO X5 Pro's Adreno 642L (SC7280) and
 * its addresses and register layouts are not this phone's; this file follows its way of working (reads before
 * writes, a step says what it will touch, a step that finds something unexpected ends the sequence) with the
 * SDM845's own numbers.
 *
 *   1  reads   what feeds the GPU from outside: its branches in the global clock controller (GCC)
 *   2  reads   the names RPMh knows the GPU's supplies by (the command database the boot loader left)
 *   3  reads   the GPU's own clock controller (GPUCC) -- only if GCC_GPU_CFG_AHB_CLK is on: an unclocked
 *              access to it can hang the bus -- and the two power switches (GDSC) of the GPU block
 *   4  writes  the "CX" power switch on, as Linux's gdsc.c does for gpu_cx_gdsc (VOTABLE, status in the
 *              hardware-control register): the half of the GPU block that is not the 3D core -- the GMU, the
 *              bus interface, the GPU's SMMU. The end of the run switches it off again.
 *   5  writes  the clocks of that half: the GPU's two data paths in GCC (memnoc_gfx, snoc_dvm_gfx), the GMU's
 *              clock source from the crystal (19.2 MHz; the 200 MHz tap needs GPLL0 and comes later) and
 *              gpu_cc_cxo_clk, gpu_cc_cx_gmu_clk; each is checked against its own status bit
 *   6  reads   the GMU's registers (offsets of Linux's a6xx_gmu.xml, 4 bytes each from 0x506a000), among them
 *              the always-on counter, which must be seen counting if the GMU's clock really runs
 *   7  reads   the GPU's SMMU (an ARM SMMU v2, 0x5040000; in the CX domain, so only with steps 4 and 5 done):
 *              its configuration and ID registers and its stream-to-context table
 *   8  reads   its context banks as the boot loader and TrustZone left them
 *   9  writes  two words into the GMU's instruction memory and two into its data memory, reads them back and puts
 *              the old words back: the way its firmware goes in
 *  10  writes  the GMU's firmware (fw/a630_gmu.bin of the small image, 32 KB: a "legacy" GMU takes one raw image
 *              that Linux's a6xx_gmu_fw_load() copies to the start of its TCM) and reads all of it back. The GMU's
 *              processor stays in reset.
 *
 * Addresses: the phone's own device tree (qcom,gpucc@5090000, qcom,gdsc@509106c/509100c, qcom,kgsl-3d0@5000000,
 * qcom,gmu@506a000, arm,smmu-kgsl@5040000); register offsets and layouts from Linux drivers/clk/qcom/
 * {gcc,gpucc}-sdm845.c and drivers/clk/qcom/gdsc.c, drivers/soc/qcom/cmd-db.c (F-029).
 */
#include <console/video_console.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/redwood_input.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
#include <kern/locks.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern struct vc_info vinfo;    /* the console's own write-combining mapping of the frame buffer */
extern uint64_t kvtophys(vm_offset_t va);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static int say(const char *format, ...) __printflike(1, 2);

/* to the loader's log at once as well: a step that hangs the machine leaves no other trace */
static int
say(const char *format, ...)
{
	char line[400];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	return printf("%s", line);
}
#define printf say

static inline uint32_t
rd(vm_offset_t address)
{
	uint32_t value;
	/* Like Linux arm64 __raw_readl: firmware may trap MMIO and cannot be assumed
	 * to emulate the post-index loads clang creates from a volatile C loop. */
	__asm__ volatile ("ldr %w0, [%1]" : "=r" (value) : "r" (address) : "memory");
	return value;
}

/* ---- the global clock controller ----------------------------------------------------------------- */

#define GCC_PHYS                0x00100000UL
#define GCC_SIZE                0x001f0000UL
#define CBCR_OFF                (1u << 31)      /* the branch is off */
#define GCC_APPS_VOTE_0         0x52004         /* the apps processor's enables: GPLL0 to the GPU at bit 15, its /2 at 16 */

static vm_offset_t gcc;

static const struct {
	const char *name;
	uint32_t reg;
} gcc_branches[] = {
	{ "gcc_gpu_cfg_ahb_clk", 0x71004 },             /* the GPU's register bus: GPUCC and the GPU block need it */
	{ "gcc_gpu_memnoc_gfx_clk", 0x7100c },
	{ "gcc_gpu_snoc_dvm_gfx_clk", 0x71018 },
	{ "gcc_gpu_iref_clk", 0x8c010 },
	{ "gcc_gpu_vs_clk", 0x7a04c },
	{ "gcc_ddrss_gpu_axi_clk", 0x44038 },
};
#define GCC_GPU_CFG_AHB         0x71004

static bool
step_gcc(void)
{
	uint32_t votes;
	bool ahb_on = false;

	gcc = ml_io_map(GCC_PHYS, GCC_SIZE);
	for (unsigned i = 0; i < sizeof(gcc_branches) / sizeof(gcc_branches[0]); i++) {
		uint32_t v = rd(gcc + gcc_branches[i].reg);

		printf("mi8gpu: %-26s %#07x = %#010x: %s\n", gcc_branches[i].name, gcc_branches[i].reg, v, (v & CBCR_OFF) ? "OFF" : "on");
		if (gcc_branches[i].reg == GCC_GPU_CFG_AHB) {
			ahb_on = !(v & CBCR_OFF);
		}
	}
	votes = rd(gcc + GCC_APPS_VOTE_0);
	printf("mi8gpu: apps votes %#x = %#010x: GPLL0 to the GPU %s, its /2 %s\n", GCC_APPS_VOTE_0, votes,
	    (votes & (1u << 15)) ? "voted" : "not voted", (votes & (1u << 16)) ? "voted" : "not voted");
	printf("mi8gpu: the GPU's register bus (gcc_gpu_cfg_ahb_clk) is %s\n", ahb_on ? "on" : "OFF: step 3 will not touch GPUCC");
	return true;
}

/* ---- RPMh's command database ----------------------------------------------------------------------- */

#define CMD_DB_PHYS             0x85fe0000UL    /* EXP-0067: where the boot loader left it on this phone */
#define CMD_DB_SIZE             0x20000UL

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};

/* The command database is mapped once: every ml_io_map() takes address space for good, and the window for it is small
 * (EXP-0127: the touch driver's start failed with kmem_alloc 16384 after a few more of them). */
static vm_offset_t
cmd_db_map(void)
{
	static vm_offset_t va;

	if (va == 0) {
		va = (vm_offset_t)ml_io_map(CMD_DB_PHYS, CMD_DB_SIZE);
	}
	return va;
}

static uint32_t
rpmh_address(const struct cmd_db_header *db, const char *name)
{
	char id[8] = { 0 };

	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

static bool
step_rpmh(void)
{
	static const char *const names[] = { "cx.lvl", "mx.lvl", "gfx.lvl", "gmx.lvl", "ldoa17", "vddcx", "vddgfx" };
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db_map();

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		printf("mi8gpu: no command database at %#lx (magic %02x %02x %02x %02x)\n", CMD_DB_PHYS, db->magic[0], db->magic[1],
		    db->magic[2], db->magic[3]);
		return false;
	}
	for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		uint32_t address = rpmh_address(db, names[i]);

		if (address != 0) {
			printf("mi8gpu: RPMh resource \"%s\" is at %#x\n", names[i], address);
		} else {
			printf("mi8gpu: RPMh knows no resource \"%s\"\n", names[i]);
		}
	}
	return true;
}

/* ---- the GPU's own clock controller ---------------------------------------------------------------- */

#define GPUCC_PHYS              0x05090000UL
#define GPUCC_SIZE              0x9000UL
#define GDSC_PWR_ON             (1u << 31)      /* GDSCR: the switch is on */

static vm_offset_t gpucc;

static bool
step_gpucc(void)
{
	static const struct {
		const char *name;
		uint32_t reg;
	} regs[] = {
		{ "GX GDSCR (gpu_gx_gdsc)", 0x100c },
		{ "CX GDSCR (gpu_cx_gdsc)", 0x106c },
		{ "gpu_cc_cx_gmu_clk CBCR", 0x1098 },
		{ "gpu_cc_cxo_clk CBCR", 0x109c },
		{ "GMU clock source RCG CMD", 0x1120 },
		{ "GMU clock source RCG CFG", 0x1124 },
		{ "CX GDSC hardware control", 0x1540 },
		{ "GX clamp IO control", 0x1508 },
	};
	vm_offset_t cc;

	if (rd(gcc + GCC_GPU_CFG_AHB) & CBCR_OFF) {
		printf("mi8gpu: gcc_gpu_cfg_ahb_clk is off: GPUCC is not read (an access could hang the bus)\n");
		return false;
	}
	cc = gpucc = ml_io_map(GPUCC_PHYS, GPUCC_SIZE);
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint32_t v = rd(cc + regs[i].reg);

		printf("mi8gpu: GPUCC %-26s %#06x = %#010x%s\n", regs[i].name, regs[i].reg, v,
		    (regs[i].reg == 0x100c || regs[i].reg == 0x106c) ? ((v & GDSC_PWR_ON) ? ": power ON" : ": power off") : "");
	}
	return true;
}


/* ---- step 4: the CX power switch ------------------------------------------------------------------- */

#define GPUCC_GX_GDSCR          0x100c
#define GPUCC_CX_GDSCR          0x106c
#define GPUCC_CX_GDSC_HWCTRL    0x1540          /* gpu_cx_gdsc's gds_hw_ctrl: where its status is read (bit 31 power on) */
#define GDSCR_SW_COLLAPSE       (1u << 0)
#define GDSCR_CLK_DIS_WAIT_MASK (0xfu << 12)
#define GDSCR_CLK_DIS_WAIT(x)   ((uint32_t)(x) << 12)
#define CX_CLK_DIS_WAIT_VAL     8               /* the device tree's qcom,clk-dis-wait-val */
#define GDSC_POLL_US            2000            /* gdsc.c: STATUS_POLL_TIMEOUT_US */

static bool cx_on;

static inline void
wr(vm_offset_t address, uint32_t value)
{
	__asm__ volatile ("str %w0, [%1]" : : "r" (value), "r" (address) : "memory");
}

static void
sleep_us(uint32_t us)
{
	delay_for_interval(us, NSEC_PER_USEC);
}

/* Waits for the power-on bit in the hardware-control register to be `on`; returns whether it is. */
static bool
cx_wait(bool on)
{
	for (uint32_t waited = 0; waited <= GDSC_POLL_US; waited += 10) {
		if (!!(rd(gpucc + GPUCC_CX_GDSC_HWCTRL) & GDSC_PWR_ON) == on) {
			return true;
		}
		sleep_us(10);
	}
	return false;
}

static bool
step_cx_on(void)
{
	uint32_t v;

	if (gpucc == 0) {
		printf("mi8gpu: GPUCC was not read (step 3): the CX switch is not touched\n");
		return false;
	}
	v = rd(gpucc + GPUCC_CX_GDSCR);
	printf("mi8gpu: CX GDSCR before: %#010x, hardware control %#010x\n", v, rd(gpucc + GPUCC_CX_GDSC_HWCTRL));
	if (v & GDSC_PWR_ON) {
		printf("mi8gpu: the CX switch is already on: not touched\n");
		return false;
	}
	/* gdsc_init(): the delay of the clock-disable sequence from the device tree; then gdsc_toggle_logic(ON) */
	wr(gpucc + GPUCC_CX_GDSCR, (v & ~GDSCR_CLK_DIS_WAIT_MASK) | GDSCR_CLK_DIS_WAIT(CX_CLK_DIS_WAIT_VAL));
	v = rd(gpucc + GPUCC_CX_GDSCR);
	wr(gpucc + GPUCC_CX_GDSCR, v & ~GDSCR_SW_COLLAPSE);
	sleep_us(1);    /* gdsc.c: the hardware controller takes about 8 XO cycles to update its status bit */
	cx_on = cx_wait(true);
	printf("mi8gpu: CX GDSCR after: %#010x, hardware control %#010x: the switch is %s\n", rd(gpucc + GPUCC_CX_GDSCR),
	    rd(gpucc + GPUCC_CX_GDSC_HWCTRL), cx_on ? "ON" : "STILL OFF (2 ms)");
	return cx_on;
}


/* ---- step 5: the clocks of the CX half ------------------------------------------------------------- */

#define GCC_GPU_MEMNOC_GFX      0x7100c
#define GCC_GPU_SNOC_DVM_GFX    0x71018
#define GPUCC_CX_GMU_CBCR       0x1098
#define GPUCC_CXO_CBCR          0x109c
#define GPUCC_GMU_RCG_CMD       0x1120
#define GPUCC_GMU_RCG_CFG       0x1124
#define CBCR_ENABLE             (1u << 0)
#define RCG_CMD_UPDATE          (1u << 0)
#define RCG_CMD_ROOT_EN         (1u << 1)       /* force the root on (parked clocks use it) */
#define RCG_CMD_ROOT_OFF        (1u << 31)
#define CX_GMU_CBCR_WAKE_SLEEP  ((0xfu << 8) | (0xfu << 4))     /* gpucc-sdm845.c probe: recommended wakeup/sleep settings */

static bool gcc_memnoc_on, gcc_snoc_on, rcg_on, cxo_on, gmu_clk_on;

/* Enables a branch (bit 0) and waits for its status bit (31: off) to say it runs; the register is the branch's own. */
static bool
branch_enable(vm_offset_t base, uint32_t reg, const char *name)
{
	wr(base + reg, rd(base + reg) | CBCR_ENABLE);
	for (uint32_t waited = 0; waited <= 1000; waited += 5) {
		if (!(rd(base + reg) & CBCR_OFF)) {
			printf("mi8gpu: %s on: %#010x\n", name, rd(base + reg));
			return true;
		}
		sleep_us(5);
	}
	printf("mi8gpu: %s does not come on: %#010x\n", name, rd(base + reg));
	return false;
}

static void
branch_disable(vm_offset_t base, uint32_t reg, const char *name)
{
	wr(base + reg, rd(base + reg) & ~CBCR_ENABLE);
	sleep_us(10);
	printf("mi8gpu: %s off again: %#010x\n", name, rd(base + reg));
}

static bool
step_clocks(void)
{
	uint32_t v;
	bool done;

	if (!cx_on) {
		printf("mi8gpu: the CX switch is off (step 4): the clocks are not touched\n");
		return false;
	}
	if (!(gcc_memnoc_on = branch_enable(gcc, GCC_GPU_MEMNOC_GFX, "gcc_gpu_memnoc_gfx_clk")) ||
	    !(gcc_snoc_on = branch_enable(gcc, GCC_GPU_SNOC_DVM_GFX, "gcc_gpu_snoc_dvm_gfx_clk"))) {
		return false;
	}
	/* the GMU's clock source: crystal, undivided (parent index 0, divider field 0), then the update bit */
	v = rd(gpucc + GPUCC_GMU_RCG_CFG);
	wr(gpucc + GPUCC_GMU_RCG_CFG, v & ~((7u << 8) | 0x1fu));
	wr(gpucc + GPUCC_GMU_RCG_CMD, rd(gpucc + GPUCC_GMU_RCG_CMD) | RCG_CMD_ROOT_EN | RCG_CMD_UPDATE);
	rcg_on = true;
	done = false;
	for (uint32_t waited = 0; waited <= 1000 && !done; waited += 5) {
		v = rd(gpucc + GPUCC_GMU_RCG_CMD);
		done = !(v & RCG_CMD_UPDATE) && !(v & RCG_CMD_ROOT_OFF);
		if (!done) {
			sleep_us(5);
		}
	}
	printf("mi8gpu: GMU clock source: CMD %#010x CFG %#010x: %s\n", rd(gpucc + GPUCC_GMU_RCG_CMD), rd(gpucc + GPUCC_GMU_RCG_CFG),
	    done ? "updated, root on" : "NOT UPDATED");
	if (!done) {
		return false;
	}
	wr(gpucc + GPUCC_CX_GMU_CBCR, (rd(gpucc + GPUCC_CX_GMU_CBCR) & ~CX_GMU_CBCR_WAKE_SLEEP) | CX_GMU_CBCR_WAKE_SLEEP);
	return (cxo_on = branch_enable(gpucc, GPUCC_CXO_CBCR, "gpu_cc_cxo_clk")) &&
	       (gmu_clk_on = branch_enable(gpucc, GPUCC_CX_GMU_CBCR, "gpu_cc_cx_gmu_clk"));
}

/* ---- step 6: the GMU's registers --------------------------------------------------------------------- */

#define GMU_PHYS                0x0506a000UL
#define GMU_SIZE                0x30000UL
#define GMU_WORD(w)             ((w) * 4u)

static bool
step_gmu_registers(void)
{
	static const struct {
		const char *name;
		uint32_t word;
	} regs[] = {
		{ "GMU_CM3_SYSRESET", 0x5000 },
		{ "GMU_CM3_BOOT_CONFIG", 0x5001 },
		{ "GMU_CM3_FW_INIT_RESULT", 0x501c },
		{ "GMU_CM3_CFG", 0x502d },
		{ "GMU_CX_GMU_POWER_COUNTER_ENABLE", 0x5040 },
		{ "GMU_GMU_PWR_COL_KEEPALIVE", 0x50c3 },
		{ "GMU_RPMH_CTRL", 0x50e8 },
		{ "GMU_HFI_CTRL_STATUS", 0x5180 },
		{ "GMU_GENERAL_0", 0x51c5 },
	};
	vm_offset_t gmu;
	uint32_t first, second, high;
	uint64_t start, now, ns;

	if (!gmu_clk_on) {
		printf("mi8gpu: the GMU's clock is not running (step 5): the GMU is not read\n");
		return false;
	}
	gmu = ml_io_map(GMU_PHYS, GMU_SIZE);
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		printf("mi8gpu: GMU %-34s word %#06x (offset %#07x) = %#010x\n", regs[i].name, regs[i].word, GMU_WORD(regs[i].word),
		    rd(gmu + GMU_WORD(regs[i].word)));
	}
	first = rd(gmu + GMU_WORD(0x5088));
	start = mach_absolute_time();
	sleep_us(1000);
	second = rd(gmu + GMU_WORD(0x5088));
	now = mach_absolute_time();
	high = rd(gmu + GMU_WORD(0x5089));
	absolutetime_to_nanoseconds(now - start, &ns);
	printf("mi8gpu: GMU always-on counter: %u then %u (high word %u) over %llu us: %llu kHz\n", first, second, high, ns / 1000,
	    ns ? (uint64_t)(second - first) * 1000000 / ns : 0);
	return second != first;
}

/* ---- steps 7 and 8: the GPU's SMMU ------------------------------------------------------------------ */

#define SMMU_PHYS               0x05040000UL
#define SMMU_SIZE               0x10000UL
/* the global register space 0 (ARM SMMU architecture 2.0; the layout Linux's arm-smmu.h has) */
#define SMMU_SCR0               0x000
#define SMMU_SCR1               0x004
#define SMMU_SCR2               0x008
#define SMMU_SACR               0x010
#define SMMU_ID0                0x020
#define SMMU_ID1                0x024
#define SMMU_ID2                0x028
#define SMMU_ID7                0x03c
#define SMMU_SGFSR              0x048
#define SMMU_SGFSYNR0           0x050
#define SMMU_SMR(n)             (0x800 + 4 * (n))
#define SMMU_S2CR(n)            (0xc00 + 4 * (n))
/* register space 1: the context banks' attributes */
#define SMMU_GR1                0x1000
#define SMMU_CBAR(n)            (SMMU_GR1 + 4 * (n))
#define SMMU_CBA2R(n)           (SMMU_GR1 + 0x800 + 4 * (n))
/* one context bank (page n of the second half) */
#define CB_SCTLR                0x00
#define CB_TCR2                 0x10
#define CB_TTBR0_LO             0x20
#define CB_TTBR0_HI             0x24
#define CB_TCR                  0x30
#define CB_CONTEXTIDR           0x34
#define CB_MAIR0                0x38
#define CB_FSR                  0x58
#define CB_FSYNR0               0x68

static vm_offset_t smmu;
static uint32_t smmu_pages_cb_base;     /* where the context banks start, from the base */
static uint32_t smmu_page_size = 0x1000, smmu_num_cb, smmu_num_smr;

static bool
step_smmu_global(void)
{
	uint32_t id0, id1;

	if (!cx_on || !gcc_memnoc_on) {
		printf("mi8gpu: the CX switch or the GPU's memory clock is off (steps 4, 5): the SMMU is not touched\n");
		return false;
	}
	smmu = ml_io_map(SMMU_PHYS, SMMU_SIZE);
	id0 = rd(smmu + SMMU_ID0);
	id1 = rd(smmu + SMMU_ID1);
	printf("mi8gpu: SMMU sCR0 %#010x sCR1 %#010x sCR2 %#010x sACR %#010x\n", rd(smmu + SMMU_SCR0), rd(smmu + SMMU_SCR1),
	    rd(smmu + SMMU_SCR2), rd(smmu + SMMU_SACR));
	printf("mi8gpu: SMMU ID0 %#010x ID1 %#010x ID2 %#010x ID7 %#010x (architecture %u.%u)\n", id0, id1, rd(smmu + SMMU_ID2),
	    rd(smmu + SMMU_ID7), (rd(smmu + SMMU_ID7) >> 4) & 15, rd(smmu + SMMU_ID7) & 15);
	printf("mi8gpu: SMMU global fault status sGFSR %#010x sGFSYNR0 %#010x\n", rd(smmu + SMMU_SGFSR), rd(smmu + SMMU_SGFSYNR0));
	smmu_num_smr = id0 & 0xff;
	smmu_num_cb = id1 & 0xff;
	smmu_page_size = (id1 & (1u << 31)) ? 0x10000 : 0x1000;
	/* arm-smmu.h: the context banks start after 2^(NUMPAGENDXB+1) pages (ARM_SMMU_CB). EXP-0083: a /2 here read the
	 * implementation's pages in between (0x5044000), and that read never came back. The device tree's gpu-offset
	 * 0x48000 is 0x5040000 + 0x8000, which this gives for NUMPAGENDXB = 2. */
	smmu_pages_cb_base = (1u << (((id1 >> 28) & 7) + 1)) * smmu_page_size;
	if (smmu_pages_cb_base + smmu_page_size * smmu_num_cb > SMMU_SIZE) {
		printf("mi8gpu: the context banks would lie outside the SMMU's registers (%#x + %u pages): stopping\n", smmu_pages_cb_base, smmu_num_cb);
		return false;
	}
	printf("mi8gpu: SMMU: %u stream-mapping entries, %u context banks, page size %u, context banks from %#x (stage 1 %s, stage 2 %s, "
	    "stream matching %s, address translation operations %s)\n",
	    smmu_num_smr, smmu_num_cb, smmu_page_size, smmu_pages_cb_base, (id0 & (1u << 30)) ? "yes" : "no",
	    (id0 & (1u << 29)) ? "yes" : "no", (id0 & (1u << 27)) ? "yes" : "no", (id0 & (1u << 26)) ? "NO" : "yes");
	printf("mi8gpu: SMMU: unmatched streams %s (sCR0.USFCFG %u)\n", (rd(smmu + SMMU_SCR0) & (1u << 10)) ? "FAULT" : "pass",
	    (rd(smmu + SMMU_SCR0) >> 10) & 1);
	for (uint32_t n = 0; n < smmu_num_smr && n < 64; n++) {
		uint32_t smr = rd(smmu + SMMU_SMR(n)), s2cr = rd(smmu + SMMU_S2CR(n));

		if ((smr & (1u << 31)) || s2cr != 0) {
			printf("mi8gpu: SMMU stream entry %2u: SMR %#010x (id %#x mask %#x%s) S2CR %#010x (type %u, context bank %u)\n", n, smr,
			    smr & 0x7fff, (smr >> 16) & 0x7fff, (smr & (1u << 31)) ? ", valid" : "", s2cr, (s2cr >> 16) & 3, s2cr & 0xff);
		}
	}
	return true;
}

static bool
step_smmu_contexts(void)
{
	if (smmu == 0) {
		printf("mi8gpu: the SMMU was not read (step 7)\n");
		return false;
	}
	unsigned used = 0;

	for (uint32_t n = 0; n < smmu_num_cb && n < 32; n++) {
		uint32_t cbar = rd(smmu + SMMU_CBAR(n));
		vm_offset_t cb = smmu + smmu_pages_cb_base + smmu_page_size * n;

		if (cbar == 0 && rd(cb + CB_SCTLR) == 0) {
			continue;
		}
		used++;
		printf("mi8gpu: SMMU context bank %u: CBAR %#010x CBA2R %#010x; SCTLR %#010x TTBR0 %#010x%08x TCR %#010x TCR2 %#010x "
		    "MAIR0 %#010x FSR %#010x\n", n, cbar, rd(smmu + SMMU_CBA2R(n)), rd(cb + CB_SCTLR), rd(cb + CB_TTBR0_HI),
		    rd(cb + CB_TTBR0_LO), rd(cb + CB_TCR), rd(cb + CB_TCR2), rd(cb + CB_MAIR0), rd(cb + CB_FSR));
	}
	printf("mi8gpu: SMMU: %u of %u context banks are in use (CBAR or SCTLR not zero)\n", used, smmu_num_cb);
	return true;
}

/* ---- steps 9 and 10: the GMU's memory ------------------------------------------------------------------ */

extern const uint8_t *redwood_wifi_file(const char *name, uint64_t *size);  /* a file of fw/ in the small image, in place */

#define GMU_ITCM                GMU_WORD(0xc00)         /* a6xx_gmu.xml GMU_CM3_ITCM_START 0x1b400, less the 0x1a800 of the GMU's place */
#define GMU_DTCM                GMU_WORD(0x1c00)        /* GMU_CM3_DTCM_START 0x1c400; 16 KB after the instruction memory's start */
#define GMU_TCM_SIZE            0x8000u                 /* legacy: one image of 32 KB, a6xx_gmu_fw_load() */

static vm_offset_t gmu_base;

static bool
step_gmu_tcm_test(void)
{
	static const uint32_t pattern[2] = { 0xa5a5a5a5u, 0x5a5a5a5au };
	static const uint32_t offsets[2] = { GMU_ITCM, GMU_DTCM };
	static const char *const names[2] = { "instruction memory", "data memory" };
	bool good = true;

	if (!gmu_clk_on) {
		printf("mi8gpu: the GMU's clock is not running (step 5): its memory is not touched\n");
		return false;
	}
	gmu_base = ml_io_map(GMU_PHYS, GMU_SIZE);
	if (rd(gmu_base + GMU_WORD(0x5000)) != 1) {
		printf("mi8gpu: the GMU's processor is not held in reset (GMU_CM3_SYSRESET %#x): its memory is not touched\n",
		    rd(gmu_base + GMU_WORD(0x5000)));
		return false;
	}
	for (int m = 0; m < 2; m++) {
		uint32_t old[2], back[2];

		old[0] = rd(gmu_base + offsets[m]);
		old[1] = rd(gmu_base + offsets[m] + 4);
		wr(gmu_base + offsets[m], pattern[0]);
		wr(gmu_base + offsets[m] + 4, pattern[1]);
		back[0] = rd(gmu_base + offsets[m]);
		back[1] = rd(gmu_base + offsets[m] + 4);
		wr(gmu_base + offsets[m], old[0]);
		wr(gmu_base + offsets[m] + 4, old[1]);
		printf("mi8gpu: GMU %s (offset %#x): was %#010x %#010x; wrote %#010x %#010x, read back %#010x %#010x: %s; restored %#010x %#010x\n",
		    names[m], offsets[m], old[0], old[1], pattern[0], pattern[1], back[0], back[1],
		    (back[0] == pattern[0] && back[1] == pattern[1]) ? "same" : "DIFFERENT", rd(gmu_base + offsets[m]),
		    rd(gmu_base + offsets[m] + 4));
		good = good && back[0] == pattern[0] && back[1] == pattern[1];
	}
	return good;
}

static bool
step_gmu_firmware(void)
{
	uint64_t size = 0;
	const uint8_t *fw;
	uint32_t wrong = 0, sum = 0, first_wrong = 0;

	if (gmu_base == 0) {
		printf("mi8gpu: the GMU's memory was not tried (step 9)\n");
		return false;
	}
	fw = redwood_wifi_file("a630_gmu.bin", &size);
	if (fw == NULL) {
		printf("mi8gpu: the small image has no fw/a630_gmu.bin: stopping\n");
		return false;
	}
	printf("mi8gpu: fw/a630_gmu.bin: %llu bytes, first words %08x %08x %08x %08x\n", size, ((const uint32_t *)fw)[0],
	    ((const uint32_t *)fw)[1], ((const uint32_t *)fw)[2], ((const uint32_t *)fw)[3]);
	if (size == 0 || size > GMU_TCM_SIZE || (size & 3)) {
		printf("mi8gpu: that is not a size the GMU's memory takes (up to %#x, whole words): stopping\n", GMU_TCM_SIZE);
		return false;
	}
	for (uint32_t i = 0; i < size / 4; i++) {
		wr(gmu_base + GMU_ITCM + 4 * i, ((const uint32_t *)fw)[i]);
		sum += ((const uint32_t *)fw)[i];
	}
	for (uint32_t i = 0; i < size / 4; i++) {
		if (rd(gmu_base + GMU_ITCM + 4 * i) != ((const uint32_t *)fw)[i]) {
			if (wrong++ == 0) {
				first_wrong = i;
			}
		}
	}
	printf("mi8gpu: the firmware is in the GMU's memory (word sum %#010x): %u of %llu words read back wrong%s\n", sum, wrong,
	    size / 4, wrong ? "" : " -- all of it is as written");
	if (wrong) {
		printf("mi8gpu: the first wrong word is %u: %#010x, should be %#010x\n", first_wrong,
		    rd(gmu_base + GMU_ITCM + 4 * first_wrong), ((const uint32_t *)fw)[first_wrong]);
	}
	return wrong == 0;
}

/* ---- step 11: the RSC and the PDC (the GPU's RPMh sequencer) ----------------------------------------- */

#define PDC_PHYS                0x0b280000UL    /* sdm845.dtsi "gmu_pdc": the PDC's registers for the GPU's domain */
#define PDC_SEQ_PHYS            0x0b480000UL    /* "gmu_pdc_seq": the PDC's sequencer memory */
#define PDC_SIZE                0x10000UL
#define RSCC_OFFSET             0x23000u        /* a6xx_gmu.c: gmu->rscc = gmu->mmio + 0x23000 for a630 */

enum { RS_RSCC, RS_PDC, RS_SEQ };

/*
 * a6xx_gmu_rpmh_init() of Linux 7.2.10, the path for a630 (not a618/a640 family, not a619, not a650 family: the PDC's
 * sequencer is loaded by us, pdc_address_offset 0x30080, TCS3 command 1 data 3). The offsets are the register's word
 * offsets in its own region (Linux's gmu_write_rscc()/pdc_write() shift them left by two).
 */
static struct rs_write {
	uint8_t region;
	uint16_t word;
	const char *name;
	uint32_t value;
	uint32_t old;
} rs_writes[] = {
	{ RS_RSCC, 0x0004, "RSC_STATUS0_DRV0", 1u << 24, 0 },
	{ RS_RSCC, 0x000b, "RSCC_PDC_SLAVE_ID_DRV0", 1, 0 },
	{ RS_RSCC, 0x000e, "RSCC_HIDDEN_TCS_CMD0_DATA", 0, 0 },
	{ RS_RSCC, 0x000d, "RSCC_HIDDEN_TCS_CMD0_ADDR", 0, 0 },
	{ RS_RSCC, 0x0010, "RSCC_HIDDEN_TCS_CMD0_DATA+2", 0, 0 },
	{ RS_RSCC, 0x000f, "RSCC_HIDDEN_TCS_CMD0_ADDR+2", 0, 0 },
	{ RS_RSCC, 0x0012, "RSCC_HIDDEN_TCS_CMD0_DATA+4", 0x80000000u, 0 },
	{ RS_RSCC, 0x0011, "RSCC_HIDDEN_TCS_CMD0_ADDR+4", 0, 0 },
	{ RS_RSCC, 0x0100, "RSCC_OVERRIDE_START_ADDR", 0, 0 },
	{ RS_RSCC, 0x0008, "RSCC_PDC_SEQ_START_ADDR", 0x4520, 0 },
	{ RS_RSCC, 0x0009, "RSCC_PDC_MATCH_VALUE_LO", 0x4510, 0 },
	{ RS_RSCC, 0x000a, "RSCC_PDC_MATCH_VALUE_HI", 0x4514, 0 },
	{ RS_RSCC, 0x0180, "RSCC_SEQ_MEM_0_DRV0", 0xa7a506a0u, 0 },
	{ RS_RSCC, 0x0181, "RSCC_SEQ_MEM_0_DRV0+1", 0xa1e6a6e7u, 0 },
	{ RS_RSCC, 0x0182, "RSCC_SEQ_MEM_0_DRV0+2", 0xa2e081e1u, 0 },
	{ RS_RSCC, 0x0183, "RSCC_SEQ_MEM_0_DRV0+3", 0xe9a982e2u, 0 },
	{ RS_RSCC, 0x0184, "RSCC_SEQ_MEM_0_DRV0+4", 0x0020e8a8u, 0 },
	{ RS_SEQ, 0x0000, "PDC_GPU_SEQ_MEM_0", 0xfebea1e1u, 0 },
	{ RS_SEQ, 0x0001, "PDC_GPU_SEQ_MEM_0+1", 0xa5a4a3a2u, 0 },
	{ RS_SEQ, 0x0002, "PDC_GPU_SEQ_MEM_0+2", 0x8382a6e0u, 0 },
	{ RS_SEQ, 0x0003, "PDC_GPU_SEQ_MEM_0+3", 0xbce3e284u, 0 },
	{ RS_SEQ, 0x0004, "PDC_GPU_SEQ_MEM_0+4", 0x002081fcu, 0 },
	{ RS_PDC, 0x1573, "PDC_GPU_TCS1_CMD_ENABLE_BANK", 7, 0 },
	{ RS_PDC, 0x1574, "PDC_GPU_TCS1_CMD_WAIT_FOR_CMPL_BANK", 0, 0 },
	{ RS_PDC, 0x1572, "PDC_GPU_TCS1_CONTROL", 0, 0 },
	{ RS_PDC, 0x1575, "PDC_GPU_TCS1_CMD0_MSGID", 0x10108, 0 },
	{ RS_PDC, 0x1576, "PDC_GPU_TCS1_CMD0_ADDR", 0x30010, 0 },
	{ RS_PDC, 0x1577, "PDC_GPU_TCS1_CMD0_DATA", 1, 0 },
	{ RS_PDC, 0x1579, "PDC_GPU_TCS1_CMD0_MSGID+4", 0x10108, 0 },
	{ RS_PDC, 0x157a, "PDC_GPU_TCS1_CMD0_ADDR+4", 0x30000, 0 },
	{ RS_PDC, 0x157b, "PDC_GPU_TCS1_CMD0_DATA+4", 0, 0 },
	{ RS_PDC, 0x157d, "PDC_GPU_TCS1_CMD0_MSGID+8", 0x10108, 0 },
	{ RS_PDC, 0x157e, "PDC_GPU_TCS1_CMD0_ADDR+8", 0x30080, 0 },
	{ RS_PDC, 0x157f, "PDC_GPU_TCS1_CMD0_DATA+8", 0, 0 },
	{ RS_PDC, 0x15d7, "PDC_GPU_TCS3_CMD_ENABLE_BANK", 7, 0 },
	{ RS_PDC, 0x15d8, "PDC_GPU_TCS3_CMD_WAIT_FOR_CMPL_BANK", 0, 0 },
	{ RS_PDC, 0x15d6, "PDC_GPU_TCS3_CONTROL", 0, 0 },
	{ RS_PDC, 0x15d9, "PDC_GPU_TCS3_CMD0_MSGID", 0x10108, 0 },
	{ RS_PDC, 0x15da, "PDC_GPU_TCS3_CMD0_ADDR", 0x30010, 0 },
	{ RS_PDC, 0x15db, "PDC_GPU_TCS3_CMD0_DATA", 2, 0 },
	{ RS_PDC, 0x15dd, "PDC_GPU_TCS3_CMD0_MSGID+4", 0x10108, 0 },
	{ RS_PDC, 0x15de, "PDC_GPU_TCS3_CMD0_ADDR+4", 0x30000, 0 },
	{ RS_PDC, 0x15df, "PDC_GPU_TCS3_CMD0_DATA+4", 3, 0 },
	{ RS_PDC, 0x15e1, "PDC_GPU_TCS3_CMD0_MSGID+8", 0x10108, 0 },
	{ RS_PDC, 0x15e2, "PDC_GPU_TCS3_CMD0_ADDR+8", 0x30080, 0 },
	{ RS_PDC, 0x15e3, "PDC_GPU_TCS3_CMD0_DATA+8", 3, 0 },
	{ RS_PDC, 0x1148, "PDC_GPU_SEQ_START_ADDR", 0, 0 },
	{ RS_PDC, 0x1140, "PDC_GPU_ENABLE_PDC", 0x80000001u, 0 },
};
#define RS_COUNT ((uint32_t)(sizeof(rs_writes) / sizeof(rs_writes[0])))

static vm_offset_t rs_region[3];
static uint32_t rs_written;     /* how many table entries need restoring */
static bool rs_saved;
static uint32_t rs_old_enable;

static vm_offset_t
rs_address(const struct rs_write *w)
{
	return rs_region[w->region] + 4u * w->word;
}

/*
 * Which bits of a register read back as written. EXP-0087: the three RSCC_HIDDEN_TCS_CMD0_ADDR words read back
 * 0x822045xx whatever is written (they follow the match values), and the PDC's CMD0_MSGID does not keep bit 3 (0x10108
 * reads 0x10100). Linux writes them the same way.
 */
static uint32_t
rs_mask(const struct rs_write *w)
{
	if (w->region == RS_RSCC && (w->word == 0x000d || w->word == 0x000f || w->word == 0x0011)) {
		return 0;
	}
	if (w->region == RS_PDC && (w->word == 0x1575 || w->word == 0x1579 || w->word == 0x157d || w->word == 0x15d9 ||
	    w->word == 0x15dd || w->word == 0x15e1)) {
		return ~0x8u;
	}
	return ~0u;
}

static bool
step_rsc_pdc(void)
{
	uint32_t different = 0;

	if (gmu_base == 0 || !gmu_clk_on) {
		printf("mi8gpu: the GMU's registers are not mapped or its clock is not running: the RSC is not touched\n");
		return false;
	}
	rs_region[RS_RSCC] = gmu_base + RSCC_OFFSET;
	printf("mi8gpu: RSC at GMU+%#x: RSC_STATUS0_DRV0 %#010x, SEQ_BUSY_DRV0 %#010x, SEQ_START %#010x\n", RSCC_OFFSET,
	    rd(rs_region[RS_RSCC] + 4 * 0x4), rd(rs_region[RS_RSCC] + 4 * 0x101), rd(rs_region[RS_RSCC] + 4 * 0x8));
	printf("mi8gpu: mapping the PDC at %#lx and its sequencer memory at %#lx\n", PDC_PHYS, PDC_SEQ_PHYS);
	rs_region[RS_PDC] = ml_io_map(PDC_PHYS, PDC_SIZE);
	rs_region[RS_SEQ] = ml_io_map(PDC_SEQ_PHYS, PDC_SIZE);
	printf("mi8gpu: PDC: ENABLE_PDC %#010x\n", rd(rs_region[RS_PDC] + 4 * 0x1140));
	printf("mi8gpu: PDC: SEQ_START_ADDR %#010x, TCS1 CONTROL %#010x, TCS3 CONTROL %#010x\n",
	    rd(rs_region[RS_PDC] + 4 * 0x1148), rd(rs_region[RS_PDC] + 4 * 0x1572), rd(rs_region[RS_PDC] + 4 * 0x15d6));
	printf("mi8gpu: PDC sequencer memory: %#010x %#010x %#010x %#010x %#010x\n", rd(rs_region[RS_SEQ]),
	    rd(rs_region[RS_SEQ] + 4), rd(rs_region[RS_SEQ] + 8), rd(rs_region[RS_SEQ] + 12), rd(rs_region[RS_SEQ] + 16));

	/* A previous interrupted run may leave PDC enabled. Never edit its live tables. */
	if (rd(gmu_base + 4 * 0x5000) != 1 || (rd(gpucc + GPUCC_GX_GDSCR) & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) != GDSCR_SW_COLLAPSE ||
	    rd(rs_region[RS_RSCC] + 4 * 0x101) != 0) {
		printf("mi8gpu: RSC/PDC not idle with GMU reset/GX off; no programming\n");
		return false;
	}
	for (uint32_t i = 0; i < RS_COUNT; i++) {
		rs_writes[i].old = rd(rs_address(&rs_writes[i]));
	}
	rs_old_enable = rd(rs_region[RS_PDC] + 4 * 0x1140);
	rs_saved = true;
	wr(rs_region[RS_PDC] + 4 * 0x1140, rs_old_enable & ~1u);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("mi8gpu: PDC disabled for programming: %#010x (entry %#010x)\n",
	    rd(rs_region[RS_PDC] + 4 * 0x1140), rs_old_enable);
	if (rd(rs_region[RS_PDC] + 4 * 0x1140) & 1u) {
		return false;
	}
	for (uint32_t i = 0; i < RS_COUNT; i++) {
		struct rs_write *w = &rs_writes[i];
		uint32_t back;

		wr(rs_address(w), w->value);
		rs_written = i + 1;
		back = rd(rs_address(w));
		bool same = ((back ^ w->value) & rs_mask(w)) == 0;

		if (!same) {
			different++;
		}
		printf("mi8gpu:   %-40s was %#010x, wrote %#010x, read back %#010x%s\n", w->name, w->old, w->value, back,
		    same ? (back == w->value ? "" : "  (bits the hardware keeps fixed)") : "  DIFFERENT");
	}
	printf("mi8gpu: RSC after: RSC_STATUS0_DRV0 %#010x, SEQ_BUSY_DRV0 %#010x\n", rd(rs_region[RS_RSCC] + 4 * 0x4),
	    rd(rs_region[RS_RSCC] + 4 * 0x101));
	return different == 0;
}

static bool
rsc_pdc_restore(void)
{
	uint32_t count = rs_written, different = 0;

	if (!rs_saved) {
		return true;
	}
	wr(rs_region[RS_PDC] + 4 * 0x1140, rd(rs_region[RS_PDC] + 4 * 0x1140) & ~1u);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("mi8gpu: PDC disabled for restoration: %#010x\n", rd(rs_region[RS_PDC] + 4 * 0x1140));
	if (rd(rs_region[RS_PDC] + 4 * 0x1140) & 1u) {
		printf("mi8gpu: PDC did not disable; tables not changed\n");
		return false;
	}
	while (rs_written > 0) {
		struct rs_write *w = &rs_writes[--rs_written];
		if (!(w->region == RS_PDC && w->word == 0x1140)) {
			wr(rs_address(w), w->old);
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(rs_region[RS_PDC] + 4 * 0x1140, rs_old_enable);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < count; i++) {
		struct rs_write *w = &rs_writes[i];
		uint32_t back = rd(rs_address(w));
		if ((back ^ w->old) & rs_mask(w)) {
			printf("mi8gpu: RSC/PDC restore mismatch %s: wanted %#010x got %#010x\n", w->name, w->old, back);
			different++;
		}
	}
	if (rd(rs_region[RS_PDC] + 4 * 0x1140) != rs_old_enable) {
		different++;
	}
	printf("mi8gpu: RSC/PDC restoration %s (%u table entries, fixed-bit masks); ENABLE_PDC %#010x\n",
	    different ? "FAIL" : "PASS", count, rd(rs_region[RS_PDC] + 4 * 0x1140));
	rs_saved = false;
	return different == 0;
}

/* ---- step 12: the GMU's address space in the SMMU --------------------------------------------------------- */

/*
 * The GMU's processor reaches memory through the SMMU as stream 5 (dt: gmu_kernel iommus 5; Linux sdm845.dtsi
 * "iommus = <&adreno_smmu 5>"). Linux (a6xx_gmu_memory_probe/alloc) maps for a630 a 4 KB "dummy" page at
 * 0x60000000 and then the 16 KB "debug", "log" and "hfi" regions upward from 0x60001000, in tables of 4 KB pages.
 * Here: one context bank (2, retained from EXP-0088; Linux reserves bank 0 for the 3D core), three levels of 4K pages, 39 bits, the stream
 * programmed into entry 0 of the stream table, memory of ours behind each region. The SMMU's own address translation
 * (ATOS, ID0.ATOSNS = 0) asks it what it makes of an address, so no GMU is needed to see it work. Formats and order:
 * Linux drivers/iommu/io-pgtable-arm.c, arm-smmu.c; the values of the registers are those of the upstream (a660)
 * driver of this project, which Linux's arm-smmu produces for a 4 KB-page stage 1 context too.
 */
#define GMU_CTX                 2u
#define GMU_STREAM              5u
#define GMU_STREAM_MASK         0x400u
#define GMU_SMR_ENTRY           0u

#define PTE_TABLE               3ull
#define PTE_PAGE                3ull
#define PTE_SHARED              (2ull << 8)
#define PTE_ACCESSED            (1ull << 10)
#define PTE_NOT_GLOBAL          (1ull << 11)
#define PTE_ADDRESS             0x0000fffffffff000ull
#define PTE_GMU                 (PTE_SHARED | PTE_ACCESSED | PTE_NOT_GLOBAL)    /* memory attribute 0: not cached */

#define SMMU_SMR_VALID          (1u << 31)
#define CBAR_STAGE_1            (1u << 16 | 3u << 8 | 0xfu << 12)       /* our tables alone decide; weakest defaults */
#define CBA2R_64_BIT            1u
#define CB_TLBIASID             0x610
#define CB_TLBSYNC              0x7f0
#define CB_TLBSTATUS            0x7f4
#define CB_PAR                  0x50
#define CB_ATS1PR               0x800
#define CB_ATSR                 0x8f0
#define CB_TTBR1_LO             0x28
#define CB_TTBR1_HI             0x2c
#define CB_MAIR1                0x3c
#define CB_FAR_LO               0x60
#define SCTLR_ON                (1u << 0 | 1u << 1 | 1u << 2 | 1u << 5 | 1u << 12)      /* on; faults recorded, no interrupt */
#define TCR_39_BITS             (25u | 1u << 8 | 1u << 10 | 3u << 12 | 1u << 23)        /* one table of 4K pages */
#define TCR2_36_BITS            (1u | 1u << 4 | 7u << 15)       /* 36 bits of physical address, 16 of context tag */
#define MAIR0_LINUX             0xf404ff44u     /* attribute 0 not cached, 1 cached, 2 device */
#define FSR_ALL                 0xc00001feu

static struct gmu_region {
	const char *name;
	uint64_t address;               /* the GMU's */
	uint32_t size;
	vm_offset_t memory;             /* ours: one kernel page each */
} gmu_regions[] = {
	{ "dummy", 0x60000000ull, 0x1000, 0 },
	{ "debug", 0x60001000ull, 0x4000, 0 },
	{ "log", 0x60005000ull, 0x4000, 0 },
	{ "hfi", 0x60009000ull, 0x4000, 0 },
};
#define GMU_REGIONS ((uint32_t)(sizeof(gmu_regions) / sizeof(gmu_regions[0])))

static uint32_t smr_used;               /* the stream-matching entry the SMMU took */
static vm_offset_t gmu_tables;          /* one kernel page: three tables of 4 KB (levels 1 to 3; all regions lie in one 2 MB) */
static struct {
	bool saved;
	uint32_t smr, s2cr, cbar, cba2r, sctlr, tcr, tcr2, ttbr0_lo, ttbr0_hi, ttbr1_lo, ttbr1_hi, mair0, mair1;
} smmu_old;

static vm_offset_t
smmu_cb(uint32_t n)
{
	return smmu + smmu_pages_cb_base + smmu_page_size * n;
}

/* The SMMU reads the tables from memory, not from this processor's cache; the GMU reads its regions from memory. */
static void
to_memory(vm_offset_t start, vm_size_t size)
{
	for (vm_offset_t line = start & ~(vm_offset_t)63; line < start + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* A timeout is not evidence that an unmapped address was rejected. */
enum smmu_translation { SMMU_TIMEOUT = -1, SMMU_FAULT = 0, SMMU_TRANSLATED = 1 };
static enum smmu_translation
smmu_translate_in(uint32_t ctx, uint64_t iova, uint64_t *phys)
{
	vm_offset_t cb = smmu_cb(ctx);
	uint64_t par;

	*phys = 0;
	printf("mi8gpu: ATOS request %#llx at CB%u+0x800\n", iova, ctx);
	__asm__ volatile ("str %0, [%1]" : : "r" (iova & ~0xfffull), "r" (cb + CB_ATS1PR) : "memory");
	__asm__ volatile ("dsb sy");
	for (int i = 0; i < 100 && (rd(cb + CB_ATSR) & 1); i++) {
		sleep_us(10);
	}
	if (rd(cb + CB_ATSR) & 1) {
		printf("mi8gpu: ATOS TIMEOUT for %#llx\n", iova);
		return SMMU_TIMEOUT;
	}
	__asm__ volatile ("ldr %0, [%1]" : "=r" (par) : "r" (cb + CB_PAR) : "memory");
	printf("mi8gpu: ATOS result %#llx PAR %#llx\n", iova, par);
	if (par & 1) {
		printf("mi8gpu:   %#llx: the SMMU refuses it (PAR %#llx)\n", iova, par);
		return SMMU_FAULT;
	}
	*phys = par & PTE_ADDRESS;
	return SMMU_TRANSLATED;
}

static enum smmu_translation
smmu_translate(uint64_t iova, uint64_t *phys)
{
	return smmu_translate_in(GMU_CTX, iova, phys);
}

static void
gmu_smmu_free(void)
{
	for (uint32_t r = 0; r < GMU_REGIONS; r++) {
		if (gmu_regions[r].memory != 0) {
			kmem_free(kernel_map, gmu_regions[r].memory, PAGE_SIZE);
			gmu_regions[r].memory = 0;
		}
	}
	if (gmu_tables != 0) {
		kmem_free(kernel_map, gmu_tables, PAGE_SIZE);
		gmu_tables = 0;
	}
}

static void
gmu_smmu_restore(void)
{
	vm_offset_t cb;

	if (!smmu_old.saved) {
		return;
	}
	cb = smmu_cb(GMU_CTX);
	wr(smmu + SMMU_SMR(GMU_SMR_ENTRY), 0);
	wr(smmu + SMMU_S2CR(GMU_SMR_ENTRY), smmu_old.s2cr);
	wr(cb + CB_SCTLR, 0);
	__asm__ volatile ("dsb sy");
	wr(cb + CB_TLBIASID, GMU_CTX);
	wr(cb + CB_TLBSYNC, 1);
	for (int i = 0; i < 100 && (rd(cb + CB_TLBSTATUS) & 1); i++) {
		sleep_us(10);
	}
	if (rd(smmu + SMMU_SMR(GMU_SMR_ENTRY)) != 0 || rd(cb + CB_SCTLR) != 0 ||
	    (rd(cb + CB_TLBSTATUS) & 1)) {
		printf("mi8gpu: SMMU RESTORE FAILED to stop translation; retaining memory\n");
		return;
	}
	wr(cb + CB_TCR, smmu_old.tcr);
	wr(cb + CB_TCR2, smmu_old.tcr2);
	wr(cb + CB_TTBR0_LO, smmu_old.ttbr0_lo);
	wr(cb + CB_TTBR0_HI, smmu_old.ttbr0_hi);
	wr(cb + CB_TTBR1_LO, smmu_old.ttbr1_lo);
	wr(cb + CB_TTBR1_HI, smmu_old.ttbr1_hi);
	wr(cb + CB_MAIR0, smmu_old.mair0);
	wr(cb + CB_MAIR1, smmu_old.mair1);
	wr(cb + CB_FSR, FSR_ALL);
	wr(smmu + SMMU_CBAR(GMU_CTX), smmu_old.cbar);
	printf("mi8gpu: put back CBAR: wrote %#010x, read %#010x\n", smmu_old.cbar, rd(smmu + SMMU_CBAR(GMU_CTX)));
	wr(smmu + SMMU_CBA2R(GMU_CTX), smmu_old.cba2r);
	wr(smmu + SMMU_SMR(GMU_SMR_ENTRY), smmu_old.smr);
	wr(cb + CB_SCTLR, smmu_old.sctlr);
	printf("mi8gpu: put back S2CR: wrote %#010x, read %#010x\n", smmu_old.s2cr, rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY)));
	smmu_old.saved = false;
	printf("mi8gpu: SMMU put back: entry %u SMR %#010x S2CR %#010x, context bank %u CBAR %#010x SCTLR %#010x\n", GMU_SMR_ENTRY,
	    rd(smmu + SMMU_SMR(GMU_SMR_ENTRY)), rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY)), GMU_CTX, rd(smmu + SMMU_CBAR(GMU_CTX)),
	    rd(cb + CB_SCTLR));
	const struct { vm_offset_t address; uint32_t expected; } restored[] = {
		{ smmu + SMMU_SMR(GMU_SMR_ENTRY), smmu_old.smr },
		{ smmu + SMMU_S2CR(GMU_SMR_ENTRY), smmu_old.s2cr },
		{ smmu + SMMU_CBAR(GMU_CTX), smmu_old.cbar },
		{ smmu + SMMU_CBA2R(GMU_CTX), smmu_old.cba2r },
		{ cb + CB_SCTLR, smmu_old.sctlr }, { cb + CB_TCR, smmu_old.tcr },
		{ cb + CB_TCR2, smmu_old.tcr2 }, { cb + CB_TTBR0_LO, smmu_old.ttbr0_lo },
		{ cb + CB_TTBR0_HI, smmu_old.ttbr0_hi }, { cb + CB_TTBR1_LO, smmu_old.ttbr1_lo },
		{ cb + CB_TTBR1_HI, smmu_old.ttbr1_hi }, { cb + CB_MAIR0, smmu_old.mair0 },
		{ cb + CB_MAIR1, smmu_old.mair1 },
	};
	bool same = true;
	for (uint32_t i = 0; i < sizeof(restored) / sizeof(restored[0]); i++) {
		uint32_t back = rd(restored[i].address);
		if (back != restored[i].expected) {
			printf("mi8gpu: SMMU restore mismatch offset %#lx expected %#x read %#x\n",
			    restored[i].address - smmu, restored[i].expected, back);
			same = false;
		}
	}
	printf("mi8gpu: SMMU restoration %s\n", same ? "PASS (13 registers)" : "FAIL -- retaining memory");
	if (same) {
		gmu_smmu_free();
	}
}

static bool
gmu_smmu_idle(void)
{
	vm_offset_t cb = smmu_cb(GMU_CTX);
	vm_offset_t gmu = ml_io_map(GMU_PHYS, GMU_SIZE);
	uint32_t cbar = rd(smmu + SMMU_CBAR(GMU_CTX));
	uint32_t s2cr = rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY));

	printf("mi8gpu: SMMU idle check: reset %#x GX %#x S2CR %#x CBAR %#x\n",
	    rd(gmu + GMU_WORD(0x5000)), rd(gpucc + 0x100c), s2cr, cbar);
	if (rd(gmu + GMU_WORD(0x5000)) != 1 || (rd(gpucc + 0x100c) & GDSC_PWR_ON) ||
	    (s2cr != 0x200ff && s2cr != GMU_CTX) || (cbar != 0 && cbar != CBAR_STAGE_1) ||
	    rd(cb + CB_SCTLR) || rd(cb + CB_TTBR0_LO) || rd(cb + CB_TTBR0_HI) ||
	    rd(cb + CB_TTBR1_LO) || rd(cb + CB_TTBR1_HI)) {
		printf("mi8gpu: SMMU idle preconditions not met: untouched\n");
		return false;
	}
	for (uint32_t n = 0; n < smmu_num_smr; n++) {
		if (rd(smmu + SMMU_SMR(n)) != 0) {
			printf("mi8gpu: SMR %u is not empty: untouched\n", n);
			return false;
		}
	}
	return true;
}

static bool
gmu_smr_probe(void)
{
	static const uint32_t candidates[] = { 0x80000005u, 0x84000005u, 0x80040005u };
	uint32_t accepted = 0;

	if (!gmu_smmu_idle()) {
		return false;
	}
	for (uint32_t c = 0; c < sizeof(candidates) / sizeof(candidates[0]); c++) {
		uint32_t back;

		printf("mi8gpu: SMR-only about to write candidate %u: %#010x\n", c, candidates[c]);
		wr(smmu + SMMU_SMR(GMU_SMR_ENTRY), candidates[c]);
		back = rd(smmu + SMMU_SMR(GMU_SMR_ENTRY));
		printf("mi8gpu: SMR candidate %u: wrote %#010x, read back %#010x, global fault %#x\n",
		    c, candidates[c], back, rd(smmu + SMMU_SGFSR));
		wr(smmu + SMMU_SMR(GMU_SMR_ENTRY), 0);
		if (rd(smmu + SMMU_SMR(GMU_SMR_ENTRY)) != 0) {
			printf("mi8gpu: SMR-only RESTORE FAILED: stop\n");
			return false;
		}
		if (back == candidates[c]) {
			accepted = back;
			break;
		}
	}
	printf("mi8gpu: SMR-only accepted %#010x; restored SMR %#x S2CR %#x CBAR %#x; no ATOS or GMU start\n",
	    accepted, rd(smmu + SMMU_SMR(GMU_SMR_ENTRY)), rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY)), rd(smmu + SMMU_CBAR(GMU_CTX)));
	/* This diagnostic does not satisfy step 12, even when a candidate sticks. */
	return false;
}


/* ---- step 20, first half: the 3D core's own address space (stream 0 -> context bank 1) and its SQE/ring memory --- */

static uint32_t gmu_last_step;      /* mi8gpu=N */

#define G3D_CTX                 1u
#define G3D_SMR_ENTRY           1u
#define G3D_STREAM              0u      /* qcom,gfx3d_user: iommus <smmu 0x0> (the phone's device tree; Linux: adreno_smmu 0) */

/* the screen's frame buffer, seen by the 3D core at 0x20000000 (1080 x 2248, BGRA, 4320 bytes a row; F-015, F-023) */
#define G3D_FB_PHYS             0x9d400000ull
#define G3D_FB_IOVA             0x20000000ull
#define G3D_FB_WIDTH            1080u
#define G3D_FB_HEIGHT           2248u
#define G3D_FB_BYTES            (G3D_FB_WIDTH * 4u * G3D_FB_HEIGHT)
#define G3D_TABLE_POOL          (16u * 0x1000u) /* L1 + L2 + 14 level-3 tables */

static struct g3d_region {
	const char *name;
	uint64_t address;
	uint32_t size;
	vm_offset_t memory;
	uint64_t phys;          /* for a region that is not ours to allocate */
} g3d_regions[] = {
	{ "sqe", 0x10000000ull, 0x8000, 0 },    /* the SQE microcode (a630_sqe.fw less its first 4 bytes, adreno_fw_create_bo) */
	{ "ring", 0x10008000ull, 0x8000, 0 },   /* MSM_GPU_RINGBUFFER_SZ = 32 KB */
	{ "memptrs", 0x10010000ull, 0x1000, 0 },
	{ "screen", G3D_FB_IOVA, G3D_FB_BYTES, 0, G3D_FB_PHYS },
};
#define G3D_REGIONS ((uint32_t)(sizeof(g3d_regions) / sizeof(g3d_regions[0])))
#define G3D_SQE_FILE_SKIP       4u
#define PTE_AP_UNPRIV           (1ull << 6)     /* AP[1]: the 3D core's accesses are not privileged (EXP-0108: permission fault at FAR 0x10000000) */

static vm_offset_t g3d_tables;
static struct {
	bool saved;
	uint32_t smr, s2cr, cbar, cba2r, sctlr, tcr, tcr2, ttbr0_lo, ttbr0_hi, ttbr1_lo, ttbr1_hi, mair0, mair1;
} g3d_old;
static bool g3d_ready;

static void
g3d_free(void)
{
	for (uint32_t r = 0; r < G3D_REGIONS; r++) {
		if (g3d_regions[r].memory != 0) {
			kmem_free(kernel_map, g3d_regions[r].memory, round_page(g3d_regions[r].size));
			g3d_regions[r].memory = 0;
		}
	}
	if (g3d_tables != 0) {
		kmem_free(kernel_map, g3d_tables, round_page(G3D_TABLE_POOL));
		g3d_tables = 0;
	}
}

static void
g3d_smmu_restore(void)
{
	vm_offset_t cb = smmu_cb(G3D_CTX);
	bool stopped;

	if (!g3d_old.saved) {
		return;
	}
	g3d_ready = false;
	wr(smmu + SMMU_SMR(G3D_SMR_ENTRY), 0);
	wr(smmu + SMMU_S2CR(G3D_SMR_ENTRY), g3d_old.s2cr);
	wr(cb + CB_SCTLR, 0);
	__asm__ volatile ("dsb sy");
	wr(cb + CB_TLBIASID, G3D_CTX);
	wr(cb + CB_TLBSYNC, 1);
	for (int i = 0; i < 100 && (rd(cb + CB_TLBSTATUS) & 1); i++) {
		sleep_us(10);
	}
	stopped = rd(smmu + SMMU_SMR(G3D_SMR_ENTRY)) == 0 && rd(cb + CB_SCTLR) == 0 && !(rd(cb + CB_TLBSTATUS) & 1);
	if (!stopped) {
		printf("mi8gpu: 3D core SMMU RESTORE FAILED to stop translation; retaining memory\n");
		return;
	}
	wr(cb + CB_TCR, g3d_old.tcr);
	wr(cb + CB_TCR2, g3d_old.tcr2);
	wr(cb + CB_TTBR0_LO, g3d_old.ttbr0_lo);
	wr(cb + CB_TTBR0_HI, g3d_old.ttbr0_hi);
	wr(cb + CB_TTBR1_LO, g3d_old.ttbr1_lo);
	wr(cb + CB_TTBR1_HI, g3d_old.ttbr1_hi);
	wr(cb + CB_MAIR0, g3d_old.mair0);
	wr(cb + CB_MAIR1, g3d_old.mair1);
	wr(cb + CB_FSR, FSR_ALL);
	wr(smmu + SMMU_CBAR(G3D_CTX), g3d_old.cbar);
	wr(smmu + SMMU_CBA2R(G3D_CTX), g3d_old.cba2r);
	printf("mi8gpu: 3D core SMMU put back: SMR %#010x (was %#010x) S2CR %#010x (was %#010x) CBAR %#010x (was %#010x) SCTLR %#010x\n",
	    rd(smmu + SMMU_SMR(G3D_SMR_ENTRY)), g3d_old.smr, rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY)), g3d_old.s2cr,
	    rd(smmu + SMMU_CBAR(G3D_CTX)), g3d_old.cbar, rd(cb + CB_SCTLR));
	g3d_old.saved = false;
	g3d_free();
}

static bool
g3d_smmu_setup(void)
{
	uint64_t *l1, *l2, *l3, root, phys, expected;
	vm_offset_t cb = smmu_cb(G3D_CTX);
	uint32_t wrong = 0, pages = 0, smr_value = SMMU_SMR_VALID | G3D_STREAM;
	uint64_t fw_size = 0;
	const uint8_t *fw = redwood_wifi_file("a630_sqe.fw", &fw_size);

	if (fw == NULL || fw_size <= G3D_SQE_FILE_SKIP || fw_size - G3D_SQE_FILE_SKIP > g3d_regions[0].size) {
		printf("mi8gpu: a630_sqe.fw missing or not the expected size (%llu)\n", fw_size);
		return false;
	}
	if (smmu_num_cb <= G3D_CTX || smmu_num_smr <= G3D_SMR_ENTRY || rd(smmu + SMMU_SMR(G3D_SMR_ENTRY)) != 0 ||
	    rd(cb + CB_SCTLR) != 0 || rd(cb + CB_TTBR0_LO) != 0 ||
	    (rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY)) != 0x200ff && rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY)) != G3D_CTX)) {
		printf("mi8gpu: 3D core SMMU entry %u / context bank %u are not idle (SMR %#x S2CR %#x SCTLR %#x): untouched\n", G3D_SMR_ENTRY,
		    G3D_CTX, rd(smmu + SMMU_SMR(G3D_SMR_ENTRY)), rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY)), rd(cb + CB_SCTLR));
		return false;
	}
	printf("mi8gpu: a630_sqe.fw: %llu bytes, first words %08x %08x %08x %08x\n", fw_size, ((const uint32_t *)fw)[0],
	    ((const uint32_t *)fw)[1], ((const uint32_t *)fw)[2], ((const uint32_t *)fw)[3]);
	if (kmem_alloc(kernel_map, &g3d_tables, round_page(G3D_TABLE_POOL), KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	for (uint32_t r = 0; r < G3D_REGIONS; r++) {
		if (g3d_regions[r].phys != 0) {
			continue;
		}
		if (kmem_alloc_contig(kernel_map, &g3d_regions[r].memory, round_page(g3d_regions[r].size), PAGE_MASK, 0, 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mi8gpu: no memory for %s\n", g3d_regions[r].name);
			g3d_free();
			return false;
		}
	}
	memcpy((void *)g3d_regions[0].memory, fw + G3D_SQE_FILE_SKIP, fw_size - G3D_SQE_FILE_SKIP);
	l1 = (uint64_t *)g3d_tables;
	l2 = (uint64_t *)(g3d_tables + 0x1000);
	root = kvtophys((vm_offset_t)l1);
	l1[(g3d_regions[0].address >> 30) & 0x1ff] = kvtophys((vm_offset_t)l2) | PTE_TABLE;
	uint32_t next_l3 = 2;
	for (uint32_t r = 0; r < G3D_REGIONS; r++) {
		struct g3d_region *g = &g3d_regions[r];

		for (uint32_t off = 0; off < g->size; off += 0x1000) {
			uint64_t va = g->address + off, pa = g->memory ? kvtophys(g->memory) + off : g->phys + off;
			uint64_t *entry = &l2[(va >> 21) & 0x1ff];

			if (*entry == 0) {
				if (next_l3 >= G3D_TABLE_POOL / 0x1000) {
					printf("mi8gpu: out of level-3 tables\n");
					g3d_free();
					return false;
				}
				*entry = kvtophys(g3d_tables + 0x1000 * next_l3++) | PTE_TABLE;
			}
			l3 = NULL;
			for (uint32_t t = 2; t < next_l3; t++) {
				if (kvtophys(g3d_tables + 0x1000 * t) == (*entry & PTE_ADDRESS)) {
					l3 = (uint64_t *)(g3d_tables + 0x1000 * t);
				}
			}
			l3[(va >> 12) & 0x1ff] = pa | PTE_GMU | PTE_AP_UNPRIV | PTE_PAGE;
			pages++;
		}
		if (g->memory) {
			to_memory(g->memory, round_page(g->size));
		}
		printf("mi8gpu: the 3D core will see %#llx..%#llx (%s) in memory at %#llx\n", g->address, g->address + g->size - 1,
		    g->name, g->memory ? kvtophys(g->memory) : g->phys);
	}
	to_memory(g3d_tables, round_page(G3D_TABLE_POOL));
	g3d_old.smr = rd(smmu + SMMU_SMR(G3D_SMR_ENTRY));
	g3d_old.s2cr = rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY));
	g3d_old.cbar = rd(smmu + SMMU_CBAR(G3D_CTX));
	g3d_old.cba2r = rd(smmu + SMMU_CBA2R(G3D_CTX));
	g3d_old.sctlr = rd(cb + CB_SCTLR);
	g3d_old.tcr = rd(cb + CB_TCR);
	g3d_old.tcr2 = rd(cb + CB_TCR2);
	g3d_old.ttbr0_lo = rd(cb + CB_TTBR0_LO);
	g3d_old.ttbr0_hi = rd(cb + CB_TTBR0_HI);
	g3d_old.ttbr1_lo = rd(cb + CB_TTBR1_LO);
	g3d_old.ttbr1_hi = rd(cb + CB_TTBR1_HI);
	g3d_old.mair0 = rd(cb + CB_MAIR0);
	g3d_old.mair1 = rd(cb + CB_MAIR1);
	g3d_old.saved = true;

	root |= (uint64_t)G3D_CTX << 48;
	wr(cb + CB_SCTLR, 0);
	wr(smmu + SMMU_CBA2R(G3D_CTX), CBA2R_64_BIT);
	wr(smmu + SMMU_CBAR(G3D_CTX), CBAR_STAGE_1);
	wr(cb + CB_TCR2, TCR2_36_BITS);
	wr(cb + CB_TCR, TCR_39_BITS);
	wr(cb + CB_TTBR0_LO, (uint32_t)root);
	wr(cb + CB_TTBR0_HI, (uint32_t)(root >> 32));
	wr(cb + CB_TTBR1_LO, 0);
	wr(cb + CB_TTBR1_HI, 0);
	wr(cb + CB_MAIR0, MAIR0_LINUX);
	wr(cb + CB_MAIR1, 0);
	wr(cb + CB_FSR, FSR_ALL);
	wr(cb + CB_TLBIASID, G3D_CTX);
	wr(cb + CB_TLBSYNC, 1);
	for (int i = 0; i < 100 && (rd(cb + CB_TLBSTATUS) & 1); i++) {
		sleep_us(10);
	}
	wr(cb + CB_SCTLR, SCTLR_ON);
	wr(smmu + SMMU_S2CR(G3D_SMR_ENTRY), G3D_CTX);
	wr(smmu + SMMU_SMR(G3D_SMR_ENTRY), smr_value);
	printf("mi8gpu: 3D core context bank %u: CBAR %#010x SCTLR %#010x TTBR0 %#010x%08x; entry %u SMR %#010x S2CR %#010x\n", G3D_CTX,
	    rd(smmu + SMMU_CBAR(G3D_CTX)), rd(cb + CB_SCTLR), rd(cb + CB_TTBR0_HI), rd(cb + CB_TTBR0_LO), G3D_SMR_ENTRY,
	    rd(smmu + SMMU_SMR(G3D_SMR_ENTRY)), rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY)));
	if (rd(smmu + SMMU_SMR(G3D_SMR_ENTRY)) != smr_value || (rd(smmu + SMMU_S2CR(G3D_SMR_ENTRY)) & 0x300ff) != G3D_CTX ||
	    !(rd(cb + CB_SCTLR) & 1)) {
		printf("mi8gpu: the SMMU did not take what was written for the 3D core\n");
		return false;
	}
	for (uint32_t r = 0; r < G3D_REGIONS; r++) {
		uint32_t prior = wrong;

		for (uint32_t off = 0; off < g3d_regions[r].size; off += (g3d_regions[r].phys ? 0x1000 * 97 : 0x1000)) {
			expected = (g3d_regions[r].memory ? kvtophys(g3d_regions[r].memory) : g3d_regions[r].phys) + off;
			if (smmu_translate_in(G3D_CTX, g3d_regions[r].address + off, &phys) != SMMU_TRANSLATED || phys != expected) {
				wrong++;
				printf("mi8gpu:   %s +%#x: expected %#llx, the SMMU says %#llx\n", g3d_regions[r].name, off, expected, phys);
			}
		}
		printf("mi8gpu: 3D %s: %u pages checked wrong%s\n", g3d_regions[r].name, wrong - prior, g3d_regions[r].phys ? " (every 97th page)" : "");
	}
	const uint64_t holes[] = { 0x10100000ull, 0x00001000ull };
	for (uint32_t h = 0; h < 2; h++) {
		enum smmu_translation result = smmu_translate_in(G3D_CTX, holes[h], &phys);

		printf("mi8gpu: 3D unmapped %#llx: %s\n", holes[h],
		    result == SMMU_FAULT ? "explicit translation fault" : result == SMMU_TIMEOUT ? "TIMEOUT -- FAIL" : "TRANSLATED -- FAIL");
		if (result != SMMU_FAULT) {
			wrong++;
		}
	}
	printf("mi8gpu: 3D core address space: %u pages, %u wrong; context fault status %#010x\n", pages, wrong, rd(cb + CB_FSR));
	g3d_ready = wrong == 0;
	return g3d_ready;
}

static bool
step_gmu_smmu(void)
{
	uint64_t *l1, *l2, *l3, root, phys, expected;
	vm_offset_t cb;
	uint32_t wrong = 0, pages = 0;

	if (smmu == 0 || smmu_num_cb <= GMU_CTX || smmu_num_smr <= GMU_SMR_ENTRY) {
		printf("mi8gpu: the SMMU was not read (step 7), or has too few context banks\n");
		return false;
	}
	cb = smmu_cb(GMU_CTX);
	int smr_probe = 0;
	if (PE_parse_boot_argn("mi8gpu_smr_probe", &smr_probe, sizeof(smr_probe)) && smr_probe == 1) {
		return gmu_smr_probe();
	}
	if (!gmu_smmu_idle()) {
		return false;
	}
	if (kmem_alloc(kernel_map, &gmu_tables, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mi8gpu: no memory for the tables\n");
		return false;
	}
	for (uint32_t r = 0; r < GMU_REGIONS; r++) {
		if (kmem_alloc(kernel_map, &gmu_regions[r].memory, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mi8gpu: no memory for %s\n", gmu_regions[r].name);
			gmu_smmu_free();
			return false;
		}
	}
	/* the three tables: level 1 (indexed by address bits 38:30), level 2 (29:21), level 3 (20:12) */
	l1 = (uint64_t *)gmu_tables;
	l2 = (uint64_t *)(gmu_tables + 0x1000);
	l3 = (uint64_t *)(gmu_tables + 0x2000);
	root = kvtophys((vm_offset_t)l1);
	for (uint32_t r = 0; r < GMU_REGIONS; r++) {
		struct gmu_region *g = &gmu_regions[r];

		if ((g->address >> 30) != (gmu_regions[0].address >> 30) || (g->address >> 21) != (gmu_regions[0].address >> 21) ||
		    ((g->address + g->size - 1) >> 21) != (gmu_regions[0].address >> 21)) {
			printf("mi8gpu: %s is not in the 2 MB the tables of this step cover\n", g->name);
			gmu_smmu_free();
			return false;
		}
		l1[(g->address >> 30) & 0x1ff] = kvtophys((vm_offset_t)l2) | PTE_TABLE;
		l2[(g->address >> 21) & 0x1ff] = kvtophys((vm_offset_t)l3) | PTE_TABLE;
		for (uint32_t off = 0; off < g->size; off += 0x1000) {
			l3[((g->address + off) >> 12) & 0x1ff] = (kvtophys(g->memory) + off) | PTE_GMU | PTE_PAGE;
			pages++;
		}
		to_memory(g->memory, PAGE_SIZE);
		printf("mi8gpu: the GMU will see %#llx..%#llx (%s) in memory of ours at %#llx\n", g->address, g->address + g->size - 1,
		    g->name, kvtophys(g->memory));
	}
	to_memory(gmu_tables, PAGE_SIZE);
	printf("mi8gpu: tables at %#llx (level 1), %u pages mapped\n", root, pages);

	/* remember what is there, to put it back */
	smmu_old.smr = rd(smmu + SMMU_SMR(GMU_SMR_ENTRY));
	smmu_old.s2cr = rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY));
	smmu_old.cbar = rd(smmu + SMMU_CBAR(GMU_CTX));
	smmu_old.cba2r = rd(smmu + SMMU_CBA2R(GMU_CTX));
	smmu_old.sctlr = rd(cb + CB_SCTLR);
	smmu_old.tcr = rd(cb + CB_TCR);
	smmu_old.tcr2 = rd(cb + CB_TCR2);
	smmu_old.ttbr0_lo = rd(cb + CB_TTBR0_LO);
	smmu_old.ttbr0_hi = rd(cb + CB_TTBR0_HI);
	smmu_old.ttbr1_lo = rd(cb + CB_TTBR1_LO);
	smmu_old.ttbr1_hi = rd(cb + CB_TTBR1_HI);
	smmu_old.mair0 = rd(cb + CB_MAIR0);
	smmu_old.mair1 = rd(cb + CB_MAIR1);
	smmu_old.saved = true;
	printf("mi8gpu: before: SMR %#010x S2CR %#010x CBAR %#010x CBA2R %#010x SCTLR %#010x TCR %#010x TCR2 %#010x TTBR0 %#010x%08x MAIR0 %#010x\n",
	    smmu_old.smr, smmu_old.s2cr, smmu_old.cbar, smmu_old.cba2r, smmu_old.sctlr, smmu_old.tcr, smmu_old.tcr2,
	    smmu_old.ttbr0_hi, smmu_old.ttbr0_lo, smmu_old.mair0);

	/* arm-smmu.c: the context is made complete and tagged before it is switched on, then the stream is sent to it */
	root |= (uint64_t)GMU_CTX << 48;
	wr(cb + CB_SCTLR, 0);
	wr(smmu + SMMU_CBA2R(GMU_CTX), CBA2R_64_BIT);
	wr(smmu + SMMU_CBAR(GMU_CTX), CBAR_STAGE_1);
	wr(cb + CB_TCR2, TCR2_36_BITS);
	wr(cb + CB_TCR, TCR_39_BITS);
	wr(cb + CB_TTBR0_LO, (uint32_t)root);
	wr(cb + CB_TTBR0_HI, (uint32_t)(root >> 32));
	wr(cb + CB_TTBR1_LO, 0);
	wr(cb + CB_TTBR1_HI, 0);
	wr(cb + CB_MAIR0, MAIR0_LINUX);
	wr(cb + CB_MAIR1, 0);
	wr(cb + CB_FSR, FSR_ALL);
	wr(cb + CB_TLBIASID, GMU_CTX);
	wr(cb + CB_TLBSYNC, 1);
	for (int i = 0; i < 100 && (rd(cb + CB_TLBSTATUS) & 1); i++) {
		sleep_us(10);
	}
	if (rd(cb + CB_TLBSTATUS) & 1) {
		printf("mi8gpu: the SMMU does not finish forgetting old translations (%#x)\n", rd(cb + CB_TLBSTATUS));
		return false;
	}
	wr(cb + CB_SCTLR, SCTLR_ON);
	wr(smmu + SMMU_S2CR(GMU_SMR_ENTRY), GMU_CTX);
	/* EXP-0091 verified this exact value on the phone; both SDM845 device trees use SID 5 without a mask. */
	smr_used = SMMU_SMR_VALID | GMU_STREAM;
	wr(smmu + SMMU_SMR(GMU_SMR_ENTRY), smr_used);
	if (rd(smmu + SMMU_SMR(GMU_SMR_ENTRY)) != smr_used) {
		smr_used = 0;
	}
	printf("mi8gpu: wrote context bank %u: CBAR %#010x CBA2R %#x SCTLR %#010x TTBR0 %#010x%08x TCR %#010x TCR2 %#010x MAIR0 %#010x\n",
	    GMU_CTX, rd(smmu + SMMU_CBAR(GMU_CTX)), rd(smmu + SMMU_CBA2R(GMU_CTX)), rd(cb + CB_SCTLR), rd(cb + CB_TTBR0_HI),
	    rd(cb + CB_TTBR0_LO), rd(cb + CB_TCR), rd(cb + CB_TCR2), rd(cb + CB_MAIR0));
	printf("mi8gpu: stream entry %u: SMR %#010x S2CR %#010x\n", GMU_SMR_ENTRY, rd(smmu + SMMU_SMR(GMU_SMR_ENTRY)),
	    rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY)));
	if (smr_used == 0 ||
	    (rd(smmu + SMMU_S2CR(GMU_SMR_ENTRY)) & 0x300ff) != GMU_CTX || !(rd(cb + CB_SCTLR) & 1) ||
	    rd(cb + CB_TTBR0_LO) != (uint32_t)root) {
		printf("mi8gpu: the SMMU did not take what was written\n");
		return false;
	}

	/* what the SMMU makes of every page of the regions, and of two addresses nothing maps */
	for (uint32_t r = 0; r < GMU_REGIONS; r++) {
		uint32_t prior_wrong = wrong;
		for (uint32_t off = 0; off < gmu_regions[r].size; off += 0x1000) {
			expected = kvtophys(gmu_regions[r].memory) + off;
			if (smmu_translate(gmu_regions[r].address + off, &phys) != SMMU_TRANSLATED || phys != expected) {
				wrong++;
				printf("mi8gpu:   %s +%#x: expected %#llx, the SMMU says %#llx\n", gmu_regions[r].name, off, expected, phys);
			}
		}
		printf("mi8gpu: %s: %u of %u pages wrong\n", gmu_regions[r].name, wrong - prior_wrong, gmu_regions[r].size / 0x1000);
	}
	printf("mi8gpu: %u of %u pages translated wrong\n", wrong, pages);
	const uint64_t holes[] = { 0x60100000ull, 0x00001000ull };
	for (uint32_t h = 0; h < 2; h++) {
		enum smmu_translation result = smmu_translate(holes[h], &phys);
		printf("mi8gpu: unmapped %#llx: %s\n", holes[h],
		    result == SMMU_FAULT ? "explicit translation fault" : result == SMMU_TIMEOUT ? "TIMEOUT -- FAIL" : "TRANSLATED -- FAIL");
		if (result != SMMU_FAULT) {
			wrong++;
		}
	}
	printf("mi8gpu: context fault status %#010x, global %#010x\n", rd(cb + CB_FSR), rd(smmu + SMMU_SGFSR));
	if (wrong == 0) {
		uint32_t want3d = 0;

		if ((PE_parse_boot_argn("mi8gpu_3d", &want3d, sizeof(want3d)) && want3d) || gmu_last_step >= 20) {
			return g3d_smmu_setup();
		}
	}
	return wrong == 0;
}

/* ---- step 13: numerical SCM queries only; never hand TrustZone memory here --------------------------- */
#define MI8_SCM_FAST       (1ull << 31)
#define MI8_SCM_64         (1ull << 30)
#define MI8_SCM_SIP        (2ull << 24)
#define MI8_SCM_AVAILABLE  0x601ull
#define MI8_SCM_SUPPORTED  0x207ull

static int32_t
mi8_scm_query(uint64_t function, uint64_t argument, uint64_t *answer)
{
	uint64_t session = 0;

	*answer = 0;
	printf("mi8gpu: SCM query function %#llx arg %#llx (one value, no memory)\n", function, argument);
	for (unsigned i = 0; i < 500; i++) {
		register uint64_t x0 __asm__("x0") = i ? 1 : function;
		register uint64_t x1 __asm__("x1") = 1;
		register uint64_t x2 __asm__("x2") = argument;
		register uint64_t x3 __asm__("x3") = 0;
		register uint64_t x4 __asm__("x4") = 0;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4),
		    "+r" (x5), "+r" (x6), "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		    "x16", "x17", "memory", "cc");
		session = x6;
		if (x0 != 1) {
			*answer = x1;
			printf("mi8gpu: SCM result rc %d answer %#llx auxiliary %#llx/%#llx\n", (int32_t)x0, x1, x2, x3);
			return (int32_t)x0;
		}
	}
	printf("mi8gpu: SCM interrupted retry limit\n");
	return -1;
}

static bool
step_trustzone(void)
{
	static const uint32_t pas_queries[] = { 0x201, 0x202, 0x205, 0x206, 0x207 };
	uint64_t how = MI8_SCM_FAST | MI8_SCM_64 | MI8_SCM_SIP, answer = 0;
	bool available = true;

	if (mi8_scm_query(how | MI8_SCM_AVAILABLE, MI8_SCM_SIP | MI8_SCM_AVAILABLE, &answer) || answer != 1) {
		how &= ~MI8_SCM_64;
		if (mi8_scm_query(how | MI8_SCM_AVAILABLE, MI8_SCM_SIP | MI8_SCM_AVAILABLE, &answer) || answer != 1) {
			printf("mi8gpu: SCM convention not established; no other calls\n");
			return false;
		}
	}
	printf("mi8gpu: SCM convention SMC%u\n", (how & MI8_SCM_64) ? 64 : 32);
	/* Linux uses ordinary calls after the atomic convention probe. */
	how &= ~MI8_SCM_FAST;
	for (uint32_t i = 0; i < sizeof(pas_queries) / sizeof(pas_queries[0]); i++) {
		if (mi8_scm_query(how | MI8_SCM_AVAILABLE, MI8_SCM_SIP | pas_queries[i], &answer) || answer != 1) {
			available = false;
		}
		printf("mi8gpu: PAS call %#x available answer %#llx\n", pas_queries[i], answer);
	}
	if (!available) {
		printf("mi8gpu: required PAS calls not all available; no PAS request\n");
		return false;
	}
	if (mi8_scm_query((how & ~MI8_SCM_FAST) | MI8_SCM_SUPPORTED, 13, &answer)) {
		return false;
	}
	printf("mi8gpu: PAS id 13 supported answer %#llx; no image supplied or authenticated\n", answer);
	if (answer > 1) {
		return false;
	}
	printf("mi8gpu: SCM queries completed; zap authentication remains unverified (PAS_SUPPORTED=%llu)\n", answer);
	return true;
}

/* ---- step 14 preflight: configuration with the M3 still held in reset ---- */

/* a6xx_gmu.xml word offsets relative to 0x1a800; a6xx_gmu_fw_start/power_config.
 * RPMH_CTRL ORs the requested bits into the saved value, preserving other votes. */
static struct {
	const char *name;
	uint32_t word, value, old;
} gmu_boot_regs[] = {
	{ "GMU_GENERAL_7", 0x51cc, 0x1, 0 },
	{ "GMU_CM3_FW_INIT_RESULT", 0x501c, 0x0, 0 },
	{ "GMU_CM3_BOOT_CONFIG", 0x5001, 0x2, 0 },
	{ "GMU_HFI_QTBL_ADDR", 0x5185, 0x60009000, 0 },
	{ "GMU_HFI_QTBL_INFO", 0x5184, 0x1, 0 },
	{ "GMU_AHB_FENCE_RANGE_0", 0x9311, 0x802800a0, 0 },
	{ "GMU_CM3_CFG", 0x502d, 0x4052, 0 },
	{ "GMU_HFI_SFR_ADDR", 0x5182, 0x6030100, 0 },
	{ "GPU_GMU_CX_GMU_PWR_COL_CP_MSG", 0x5100, 0x60005003, 0 },
	{ "GMU_SYS_BUS_CONFIG", 0x4c0f, 0x1, 0 },
	{ "GMU_ICACHE_CONFIG", 0x4c00, 0x1, 0 },
	{ "GMU_DCACHE_CONFIG", 0x4c01, 0x1, 0 },
	{ "GMU_PWR_COL_INTER_FRAME_CTRL", 0x50c0, 0x9c40400, 0 },
	{ "GMU_RPMH_CTRL", 0x50e8, 0xf11, 0 },
	{ "GPU_GMU_CX_GMU_PWR_COL_CP_RESP", 0x5101, 0x0, 0 },
};
#define GMU_BOOT_REGS ((uint32_t)(sizeof(gmu_boot_regs) / sizeof(gmu_boot_regs[0])))
static uint32_t gmu_boot_written;

struct mi8_hfi_table {
	uint32_t version, size, qhdr0_offset, qhdr_size, num_queues, active_queues;
};
struct mi8_hfi_queue {
	uint32_t status, iova, type, size, msg_size, dropped;
	uint32_t rx_watermark, tx_watermark, rx_request, tx_request, read_index, write_index;
};
_Static_assert(sizeof(struct mi8_hfi_table) == 24, "HFI table layout");
_Static_assert(sizeof(struct mi8_hfi_queue) == 48, "HFI queue layout");

/* Linux gpucc-sdm845: GPLL0 / 2 then HID divider 1.5, parent selector 6. */
static bool gmu_fast_clock_saved;
static uint32_t gmu_old_cfg, gmu_old_vote;

static bool
gmu_clock_update(uint32_t cfg)
{
	wr(gpucc + GPUCC_GMU_RCG_CFG, cfg);
	wr(gpucc + GPUCC_GMU_RCG_CMD, rd(gpucc + GPUCC_GMU_RCG_CMD) | RCG_CMD_UPDATE);
	for (uint32_t i = 0; i < 200; i++) {
		uint32_t cmd = rd(gpucc + GPUCC_GMU_RCG_CMD);
		if (!(cmd & (RCG_CMD_UPDATE | RCG_CMD_ROOT_OFF))) {
			return rd(gpucc + GPUCC_GMU_RCG_CFG) == cfg;
		}
		sleep_us(5);
	}
	return false;
}

static bool
gmu_clock_200(void)
{
	uint32_t mode, l, frac, user, cfg;
	uint64_t hz;

	printf("mi8gpu: reading GPLL0 FABIA mode/L/frac/user at GCC +0/+4/+0x38/+0xc\n");
	mode = rd(gcc);
	l = rd(gcc + 4);
	frac = rd(gcc + 0x38);
	user = rd(gcc + 0xc);
	hz = 19200000ull * l + ((19200000ull * (frac & 0xffff)) >> 16);
	printf("mi8gpu: GPLL0 mode %#x L %#x frac %#x user %#x: derived %llu Hz, postdiv encoding %u\n",
	    mode, l, frac, user, hz, (user >> 8) & 0xf);
	if (!(mode & (1u << 31)) || hz < 599999000 || hz > 600001000 || ((user >> 8) & 0xf) != 1) {
		printf("mi8gpu: GPLL0 prerequisites not confirmed; no clock changes\n");
		return false;
	}
	gmu_old_cfg = rd(gpucc + GPUCC_GMU_RCG_CFG);
	gmu_old_vote = rd(gcc + GCC_APPS_VOTE_0) & (1u << 16);
	gmu_fast_clock_saved = true;
	wr(gcc + GCC_APPS_VOTE_0, rd(gcc + GCC_APPS_VOTE_0) | (1u << 16));
	__asm__ volatile ("dsb sy" ::: "memory");
	sleep_us(10); /* gcc branch uses BRANCH_HALT_DELAY, no branch status register */
	if (!(rd(gcc + GCC_APPS_VOTE_0) & (1u << 16))) {
		return false;
	}
	cfg = (gmu_old_cfg & ~0x71fu) | 0x602u;
	bool ok = gmu_clock_update(cfg);
	printf("mi8gpu: GMU 200MHz config %s: CMD %#x CFG %#x vote %#x (frequency derived, not measured)\n",
	    ok ? "PASS" : "FAIL", rd(gpucc + GPUCC_GMU_RCG_CMD), rd(gpucc + GPUCC_GMU_RCG_CFG), rd(gcc + GCC_APPS_VOTE_0));
	return ok;
}

static bool
gmu_clock_restore(void)
{
	if (!gmu_fast_clock_saved) {
		return true;
	}
	if (rd(gmu_base + 4 * 0x5000) != 1 || !gmu_clock_update(gmu_old_cfg)) {
		printf("mi8gpu: GMU clock restore failed; keeping parent vote\n");
		return false;
	}
	wr(gcc + GCC_APPS_VOTE_0, (rd(gcc + GCC_APPS_VOTE_0) & ~(1u << 16)) | gmu_old_vote);
	__asm__ volatile ("dsb sy" ::: "memory");
	bool ok = (rd(gcc + GCC_APPS_VOTE_0) & (1u << 16)) == gmu_old_vote;
	printf("mi8gpu: GMU clock restoration %s: CFG %#x vote %#x\n", ok ? "PASS" : "FAIL",
	    rd(gpucc + GPUCC_GMU_RCG_CFG), rd(gcc + GCC_APPS_VOTE_0));
	if (ok) {
		gmu_fast_clock_saved = false;
	}
	return ok;
}

static bool
step_gmu_prepare(void)
{
	struct gmu_region *hfi = &gmu_regions[3];
	struct mi8_hfi_table *t = (void *)hfi->memory;
	struct mi8_hfi_queue *q = (void *)(hfi->memory + sizeof(*t));
	static const uint32_t ids[] = { 0, 4, 5 }; /* legacy response/debug IDs */

	if (!gmu_clk_on || !smmu_old.saved || hfi->memory == 0 || hfi->size != 0x4000 ||
	    hfi->address != 0x60009000 || gmu_regions[2].address != 0x60005000 || gmu_regions[2].size != 0x4000 ||
	    rd(gmu_base + 4 * 0x5000) != 1 ||
	    (rd(gpucc + GPUCC_GX_GDSCR) & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) != GDSCR_SW_COLLAPSE) {
		printf("mi8gpu: GMU boot preflight prerequisites not met\n");
		return false;
	}
	uint32_t fast_clock = 0;
	if (PE_parse_boot_argn("mi8gpu_gmuclk", &fast_clock, sizeof(fast_clock)) && fast_clock && !gmu_clock_200()) {
		return false;
	}
	bzero((void *)hfi->memory, hfi->size);
	t->version = 0;
	/* Follow a6xx_hfi_init's byte count (despite the header's dwords comment). */
	t->size = sizeof(*t) + 3 * sizeof(*q);
	t->qhdr0_offset = sizeof(*t) / 4;
	t->qhdr_size = sizeof(*q) / 4;
	t->num_queues = t->active_queues = 3;
	for (uint32_t i = 0; i < 3; i++) {
		q[i].status = 1;
		q[i].iova = (uint32_t)hfi->address + 0x1000 * (i + 1);
		q[i].type = (10u << 8) | ids[i];
		q[i].size = 0x1000 / 4;
		q[i].rx_watermark = q[i].tx_watermark = q[i].rx_request = 1;
		printf("mi8gpu: HFI preflight queue %u id %u iova %#x type %#x capacity %u words\n",
		    i, ids[i], q[i].iova, q[i].type, q[i].size);
	}
	to_memory(hfi->memory, hfi->size);
	printf("mi8gpu: HFI table bytes %u header offset/size %u/%u words, queues %u; not started\n",
	    t->size, t->qhdr0_offset, t->qhdr_size, t->num_queues);
	for (uint32_t i = 0; i < GMU_BOOT_REGS; i++) {
		vm_offset_t addr = gmu_base + 4 * gmu_boot_regs[i].word;
		uint32_t back, value = gmu_boot_regs[i].value;
		printf("mi8gpu: boot config %s offset %#x (reset held)\n", gmu_boot_regs[i].name, 4 * gmu_boot_regs[i].word);
		gmu_boot_regs[i].old = rd(addr);
		if (gmu_boot_regs[i].word == 0x50e8) {
			value |= gmu_boot_regs[i].old;
		}
		wr(addr, value);
		gmu_boot_written = i + 1;
		back = rd(addr);
		printf("mi8gpu: boot config was %#010x wrote %#010x read %#010x\n", gmu_boot_regs[i].old, value, back);
		if (back != value) {
			return false;
		}
	}
	printf("mi8gpu: boot preflight configured %u registers; SYSRESET %#x FW_INIT_RESULT %#x GX %#x; not released\n",
	    GMU_BOOT_REGS, rd(gmu_base + 4 * 0x5000), rd(gmu_base + 4 * 0x501c), rd(gpucc + GPUCC_GX_GDSCR));
	return rd(gmu_base + 4 * 0x5000) == 1 && rd(gmu_base + 4 * 0x501c) == 0;
}

static bool
gmu_boot_restore(void)
{
	uint32_t count = gmu_boot_written, different = 0;
	if (!count) {
		return true;
	}
	/* This is only a reset-state restore, not a shutdown procedure for a running GMU. */
	if (rd(gmu_base + 4 * 0x5000) != 1) {
		printf("mi8gpu: GMU no longer in reset; cannot restore/free live resources\n");
		return false;
	}
	while (gmu_boot_written > 0) {
		uint32_t i = --gmu_boot_written;
		wr(gmu_base + 4 * gmu_boot_regs[i].word, gmu_boot_regs[i].old);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < count; i++) {
		uint32_t back = rd(gmu_base + 4 * gmu_boot_regs[i].word);
		if (back != gmu_boot_regs[i].old) {
			printf("mi8gpu: boot restore mismatch %s wanted %#x got %#x\n", gmu_boot_regs[i].name, gmu_boot_regs[i].old, back);
			different++;
		}
	}
	printf("mi8gpu: boot configuration restoration %s (%u registers)\n", different ? "FAIL" : "PASS", count);
	return different == 0;
}

/* ---- step 15: release the GMU's reset once, look at FW_INIT_RESULT, stop it again --------------------------- */

/* a6xx_gmu.xml word offsets relative to 0x1a800 (checked against the XML of the Linux 7.2.10 tree). */
#define GMU_CM3_SYSRESET_W      0x5000u
#define GMU_CM3_INIT_RESULT_W   0x501cu
#define GMU_GMU2HOST_MASK_W     0x5193u         /* GMU_GMU2HOST_INTR_MASK 0x1f993 */
#define GMU_PWR_COL_CP_RESP_W   0x5101u
#define GMU_GENERAL_7_W         0x51ccu
#define GMU_AO_HOST_MASK_W      0x9306u         /* GMU_AO_HOST_INTERRUPT_MASK 0x23b06 */
#define GMU_RSCC_REQ_W          0x9307u         /* GMU_RSCC_CONTROL_REQ 0x23b07 */
#define GMU_RSCC_ACK_W          0x9308u
#define GMU_AO_FENCE_CTRL_W     0x9310u         /* GMU_AO_AHB_FENCE_CTRL 0x23b10 */
#define GMU_FENCE_STATUS_W      0x9313u
#define GMU_FENCE_CLR_W         0x9314u
#define RSCC_STATUS0_DRV0_W     0x4u            /* GPU_RSCC_RSC_STATUS0_DRV0, in the RSC block at GMU+0x23000 */
#define RSCC_SEQ_BUSY_DRV0_W    0x101u

/* Set from the moment the reset is released: the processor may run and the RSC may sleep. While set, teardown keeps
 * every clock, table and page the GMU might still use. */
static bool gmu_unsafe;

static bool
gmu_poll(vm_offset_t address, uint32_t mask, uint32_t want, uint32_t tries, uint32_t step_us)
{
	for (uint32_t i = 0; i < tries; i++) {
		if ((rd(address) & mask) == want) {
			return true;
		}
		sleep_us(step_us);
	}
	return (rd(address) & mask) == want;
}

/* a6xx_gmu_rpmh_off: look (do not fail) at whether the four RSC command sets are idle. */
static void
gmu_rpmh_idle_look(vm_offset_t rscc)
{
	static const uint32_t status[4] = { 0x346, 0x3ee, 0x496, 0x53e };

	for (uint32_t i = 0; i < 4; i++) {
		bool idle = gmu_poll(rscc + 4 * status[i], 1, 1, 100, 100);
		printf("mi8gpu: RSC TCS%u DRV0 status %#010x (%s)\n", i, rd(rscc + 4 * status[i]), idle ? "idle" : "NOT idle");
	}
}

/* a6xx_rpmh_stop, then a6xx_rpmh_start at once: the RSC goes back to the state it was found in. */
static bool
gmu_rsc_sleep_wake(vm_offset_t rscc)
{
	bool ok;

	wr(gmu_base + GMU_WORD(GMU_RSCC_REQ_W), 1);
	ok = gmu_poll(rscc + 4 * RSCC_STATUS0_DRV0_W, 1u << 16, 1u << 16, 100, 100);
	printf("mi8gpu: RSC sleep request: RSC_STATUS0_DRV0 %#010x (bit 16 %s)\n", rd(rscc + 4 * RSCC_STATUS0_DRV0_W), ok ? "set" : "NOT set");
	wr(gmu_base + GMU_WORD(GMU_RSCC_REQ_W), 0);
	if (!ok) {
		return false;
	}
	wr(gmu_base + GMU_WORD(GMU_RSCC_REQ_W), 1u << 1);
	ok = gmu_poll(gmu_base + GMU_WORD(GMU_RSCC_ACK_W), 1u << 1, 1u << 1, 100, 100);
	printf("mi8gpu: RSC wake request: CONTROL_ACK %#010x (bit 1 %s)\n", rd(gmu_base + GMU_WORD(GMU_RSCC_ACK_W)), ok ? "set" : "NOT set");
	if (ok) {
		ok = gmu_poll(rscc + 4 * RSCC_SEQ_BUSY_DRV0_W, ~0u, 0, 100, 100);
		printf("mi8gpu: RSC wake: SEQ_BUSY_DRV0 %#010x (%s)\n", rd(rscc + 4 * RSCC_SEQ_BUSY_DRV0_W), ok ? "idle" : "BUSY");
	}
	wr(gmu_base + GMU_WORD(GMU_RSCC_REQ_W), 0);
	return ok;
}


#define GMU_BOOT_SLUMBER_OPT_W  0x23f8u         /* GMU_BOOT_SLUMBER_OPTION 0x1cbf8 */
#define GMU_GX_VOTE_IDX_W       0x23f9u
#define GMU_MX_VOTE_IDX_W       0x23fau
#define GMU_H2G_INTR_SET_W      0x5194u         /* GMU_HOST2GMU_INTR_SET 0x1f994 */
#define GMU_G2H_INTR_INFO_W     0x5192u         /* GMU_GMU2HOST_INTR_INFO 0x1f992 */
#define GMU_G2H_INTR_CLR_W      0x5191u
#define GMU_RPMH_POWER_STATE_W  0x50ecu         /* GPU_GMU_CX_GMU_RPMH_POWER_STATE 0x1f8ec */
#define GMU_POWER_COUNTER_EN_W  0x5040u         /* GMU_CX_GMU_POWER_COUNTER_ENABLE 0x1f840 */
#define GMU_SPTPRAC_STATUS_W    0x50d0u         /* GMU_SPTPRAC_PWR_CLK_STATUS 0x1f8d0 */
#define GMU_SPTPRAC_CTRL_W      0x81u           /* GMU_GX_SPTPRAC_POWER_CONTROL 0x1a881 */
#define GMU_GX_GDSCR_ALIAS_W    0x9c03u         /* GPU_CC_GX_GDSCR 0x24403 */
#define GMU_BUSY_STATUS_W       0x930cu         /* GPU_GMU_AO_GPU_CX_BUSY_STATUS 0x23b0c */
#define OOB_BOOT_SLUMBER_SET    22u             /* a6xx_gmu_oob_bits[BOOT_SLUMBER], legacy: set/ack/clear */
#define OOB_BOOT_SLUMBER_ACK    30u
#define OOB_GPU_SET             16u             /* a6xx_gmu_oob_bits[GPU_SET], legacy: set 16 / ack 24 / clear 24 */
#define OOB_GPU_SET_ACK         24u

/* a6xx_gmu_rpmh_arc_votes_init for one level: the command database's "auxiliary data" of a resource is its list of
 * u16 levels (cmd_db_read_aux_data: slave data_offset + entry offset). */
static const uint16_t *
rpmh_levels(const struct cmd_db_header *db, const char *name, uint32_t *count)
{
	char id[8] = { 0 };

	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				if (entry[i].length < 2 || entry[i].length > 64 || slave->data_offset + entry[i].offset + entry[i].length >= CMD_DB_SIZE - 0x1000) {
					return NULL;
				}
				*count = entry[i].length / 2;
				return (const uint16_t *)(db->data + slave->data_offset + entry[i].offset);
			}
		}
	}
	return NULL;
}

static bool
gmu_gx_votes(uint32_t level, uint32_t *gx_idx, uint32_t *mx_idx)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db_map();
	const uint16_t *pri, *sec;
	uint32_t pc = 0, sc = 0, pindex = 0, sindex = 0, j;

	pri = rpmh_levels(db, "gfx.lvl", &pc);
	sec = rpmh_levels(db, "mx.lvl", &sc);
	if (pri == NULL || sec == NULL) {
		printf("mi8gpu: no auxiliary level data for gfx.lvl/mx.lvl\n");
		return false;
	}
	for (j = 0; j < pc; j++) {
		printf("mi8gpu: gfx.lvl[%u] = %u\n", j, pri[j]);
	}
	for (j = 0; j < sc; j++) {
		printf("mi8gpu: mx.lvl[%u] = %u\n", j, sec[j]);
	}
	for (j = 0; j < pc; j++) {
		if (pri[j] >= level) {
			pindex = j;
			break;
		}
	}
	if (j == pc) {
		printf("mi8gpu: level %u not in gfx.lvl\n", level);
		return false;
	}
	for (j = 0; j < sc; j++) {
		if (sec[j] >= level) {
			sindex = j;
			break;
		} else if (sec[j]) {
			sindex = j;
		}
	}
	*gx_idx = pindex;
	*mx_idx = sindex;
	printf("mi8gpu: GX vote for level %u: gfx.lvl index %u (%u), mx.lvl index %u (%u)\n", level, pindex, pri[pindex], sindex, sec[sindex]);
	return true;
}


/* a6xx_gmu_rpmh_arc_votes_init for one level of one pair of resources. */
static bool
arc_vote(const struct cmd_db_header *db, const char *pri_name, const char *sec_name, uint32_t level, uint32_t *vote)
{
	const uint16_t *pri, *sec;
	uint32_t pc = 0, sc = 0, pindex = 0, sindex = 0, j;

	pri = rpmh_levels(db, pri_name, &pc);
	sec = rpmh_levels(db, sec_name, &sc);
	if (pri == NULL || sec == NULL) {
		return false;
	}
	for (j = 0; j < pc; j++) {
		if (pri[j] >= level) {
			pindex = j;
			break;
		}
	}
	if (j == pc) {
		return false;
	}
	for (j = 0; j < sc; j++) {
		if (sec[j] >= level) {
			sindex = j;
			break;
		} else if (sec[j]) {
			sindex = j;
		}
	}
	*vote = ((pri[pindex] & 0xffffu) << 16) | (sindex << 8) | pindex;
	return true;
}

/* ---- HFI (legacy, a6xx_hfi.c): the host's command queue 0, the GMU's response queue 1 ------------------------ */

#define GMU_HFI_CTRL_STATUS_W   0x5180u         /* GMU_HFI_CTRL_STATUS 0x1f980 */
#define GMU_HFI_CTRL_INIT_W     0x5186u         /* GMU_HFI_CTRL_INIT 0x1f986 */
#define HFI_H2F_MSG_INIT        0u
#define HFI_H2F_MSG_FW_VERSION  1u
#define HFI_H2F_MSG_BW_TABLE    3u
#define HFI_H2F_MSG_PERF_TABLE  4u
#define HFI_H2F_MSG_TEST        5u
#define HFI_F2H_MSG_ERROR       100u
#define HFI_QUEUE_WORDS         1024u

static uint32_t hfi_seqnum;

/* the two sides share cache lines of the header page and the GMU reads/writes memory directly */
static struct mi8_hfi_queue *
hfi_header(uint32_t q)
{
	struct gmu_region *hfi = &gmu_regions[3];

	return (struct mi8_hfi_queue *)(hfi->memory + sizeof(struct mi8_hfi_table)) + q;
}

static uint32_t *
hfi_data(uint32_t q)
{
	return (uint32_t *)(gmu_regions[3].memory + 0x1000 * (q + 1));
}

static bool
hfi_send(uint32_t id, uint32_t *msg, uint32_t dwords, uint32_t *payload, uint32_t payload_words)
{
	struct mi8_hfi_queue *cq = hfi_header(0), *rq = hfi_header(1);
	uint32_t seq = ++hfi_seqnum % 0xfff, index, space;

	to_memory(gmu_regions[3].memory, 0x1000);       /* take the GMU's view of the headers */
	index = cq->write_index;
	space = (cq->read_index - index - 1) & (HFI_QUEUE_WORDS - 1);
	if (space < dwords) {
		printf("mi8gpu: HFI command queue full\n");
		return false;
	}
	msg[0] = (seq << 20) | (0u << 16) | (dwords << 8) | id;
	for (uint32_t i = 0; i < dwords; i++) {
		hfi_data(0)[index] = msg[i];
		index = (index + 1) & (HFI_QUEUE_WORDS - 1);
	}
	to_memory((vm_offset_t)hfi_data(0), 0x1000);
	cq->write_index = index;
	to_memory(gmu_regions[3].memory, 0x1000);
	wr(gmu_base + GMU_WORD(GMU_H2G_INTR_SET_W), 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (unsigned round = 0; round < 3; round++) {
		if (!gmu_poll(gmu_base + GMU_WORD(GMU_G2H_INTR_INFO_W), 1, 1, 10000, 100)) {
			printf("mi8gpu: HFI message id %u seq %u: no response within 1 s (INTR_INFO %#x, queue write %u read %u)\n", id, seq,
			    rd(gmu_base + GMU_WORD(GMU_G2H_INTR_INFO_W)), cq->write_index, cq->read_index);
			return false;
		}
		wr(gmu_base + GMU_WORD(GMU_G2H_INTR_CLR_W), 1);
		for (;;) {
			uint32_t resp[19], hdr, n, ri;

			to_memory(gmu_regions[3].memory, 0x1000);
			to_memory((vm_offset_t)hfi_data(1), 0x1000);
			if (rq->read_index == rq->write_index) {
				break;
			}
			ri = rq->read_index;
			hdr = hfi_data(1)[ri];
			n = (hdr >> 8) & 0xff;
			if (n == 0 || n > 19) {
				printf("mi8gpu: HFI response header %#x has size %u\n", hdr, n);
				return false;
			}
			for (uint32_t i = 0; i < n; i++) {
				resp[i] = hfi_data(1)[ri];
				ri = (ri + 1) & (HFI_QUEUE_WORDS - 1);
			}
			rq->read_index = ri;
			to_memory(gmu_regions[3].memory, 0x1000);
			if ((hdr & 0xff) == HFI_F2H_MSG_ERROR) {
				printf("mi8gpu: HFI firmware error message: code %u\n", resp[1]);
				continue;
			}
			if (n < 3 || ((resp[1] >> 20) & 0xfff) != seq) {
				printf("mi8gpu: HFI unexpected response (header %#x ret_header %#x), waiting on\n", hdr, n > 1 ? resp[1] : 0);
				continue;
			}
			printf("mi8gpu: HFI message id %u seq %u acknowledged: error %u payload0 %#x\n", id, seq, resp[2], n > 3 ? resp[3] : 0);
			if (resp[2] != 0) {
				return false;
			}
			if (payload != NULL) {
				for (uint32_t i = 0; i < payload_words && 3 + i < n; i++) {
					payload[i] = resp[3 + i];
				}
			}
			return true;
		}
	}
	printf("mi8gpu: HFI message id %u: response queue stayed empty\n", id);
	return false;
}

static const struct { uint32_t khz, level; } gpu_perf[] = {
	{ 0, 0 }, { 257000, 64 }, { 342000, 128 }, { 414000, 192 }, { 520000, 256 }, { 596000, 320 }, { 675000, 384 }, { 710000, 416 },
};
static const struct { uint32_t khz, level; } gmu_perf[] = { { 0, 0 }, { 200000, 48 }, { 400000, 128 } };

static bool
gmu_hfi_start(void)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db_map();
	uint32_t init[5] = { 0 }, ver[2] = { 0 }, test[1] = { 0 }, version = 0;
	uint32_t perf[43] = { 0 }, bw[160] = { 0 };
	bool ok;

	wr(gmu_base + GMU_WORD(GMU_HFI_CTRL_INIT_W), 1);
	ok = gmu_poll(gmu_base + GMU_WORD(GMU_HFI_CTRL_STATUS_W), 1, 1, 100, 100);
	printf("mi8gpu: HFI_CTRL_INIT: HFI_CTRL_STATUS %#x (%s)\n", rd(gmu_base + GMU_WORD(GMU_HFI_CTRL_STATUS_W)), ok ? "started" : "NOT started");
	if (!ok) {
		return false;
	}
	/* a6xx_hfi_send_gmu_init: segment 0, debug buffer, cold boot (GMU_COLD_BOOT = 1) */
	init[2] = (uint32_t)gmu_regions[1].address;
	init[3] = gmu_regions[1].size;
	init[4] = 1;
	if (!hfi_send(HFI_H2F_MSG_INIT, init, 5, NULL, 0)) {
		return false;
	}
	/* a6xx_hfi_get_fw_version: ask for 1.10 */
	ver[1] = (1u << 28) | (1u << 19) | (1u << 17);
	if (!hfi_send(HFI_H2F_MSG_FW_VERSION, ver, 2, &version, 1)) {
		return false;
	}
	printf("mi8gpu: GMU firmware reports HFI version %#x\n", version);
	/* a6xx_hfi_send_perf_table_v1: levels with their RPMh ARC votes (gfx.lvl+mx.lvl for the GPU, cx.lvl+mx.lvl for the GMU) */
	perf[1] = sizeof(gpu_perf) / sizeof(gpu_perf[0]);
	perf[2] = sizeof(gmu_perf) / sizeof(gmu_perf[0]);
	for (uint32_t i = 0; i < perf[1]; i++) {
		uint32_t vote;

		if (!arc_vote(db, "gfx.lvl", "mx.lvl", gpu_perf[i].level, &vote)) {
			printf("mi8gpu: no GX vote for level %u\n", gpu_perf[i].level);
			return false;
		}
		perf[3 + 2 * i] = vote;
		perf[4 + 2 * i] = gpu_perf[i].khz;
		printf("mi8gpu: perf GX[%u] %u kHz level %u vote %#x\n", i, gpu_perf[i].khz, gpu_perf[i].level, vote);
	}
	for (uint32_t i = 0; i < perf[2]; i++) {
		uint32_t vote;

		if (!arc_vote(db, "cx.lvl", "mx.lvl", gmu_perf[i].level, &vote)) {
			printf("mi8gpu: no CX vote for level %u\n", gmu_perf[i].level);
			return false;
		}
		perf[3 + 32 + 2 * i] = vote;
		perf[4 + 32 + 2 * i] = gmu_perf[i].khz;
		printf("mi8gpu: perf CX[%u] %u kHz level %u vote %#x\n", i, gmu_perf[i].khz, gmu_perf[i].level, vote);
	}
	if (!hfi_send(HFI_H2F_MSG_PERF_TABLE, perf, 43, NULL, 0)) {
		return false;
	}
	/* a6xx_build_bw_table: the 630 GMU does not do bus scaling; one "off" level with fixed CX/DDR commands */
	bw[1] = 1;              /* bw_level_num */
	bw[2] = 3;              /* cnoc_cmds_num */
	bw[3] = 3;              /* ddr_cmds_num */
	bw[4] = 0x05;           /* cnoc_wait_bitmask */
	bw[5] = 0x07;           /* ddr_wait_bitmask */
	bw[6] = 0x50034; bw[7] = 0x5007c; bw[8] = 0x5004c;      /* cnoc_cmds_addrs[6] */
	/* cnoc_cmds_data[2][6] at 12 */
	bw[12] = 0x40000000; bw[13] = 0x00000000; bw[14] = 0x40000000;
	bw[18] = 0x60000001; bw[19] = 0x20000001; bw[20] = 0x60000001;
	bw[24] = 0x50000; bw[25] = 0x5005c; bw[26] = 0x5000c;   /* ddr_cmds_addrs[8] */
	/* ddr_cmds_data[16][8] at 32 */
	bw[32] = 0x40000000; bw[33] = 0x40000000; bw[34] = 0x40000000;
	if (!hfi_send(HFI_H2F_MSG_BW_TABLE, bw, 160, NULL, 0)) {
		return false;
	}
	return hfi_send(HFI_H2F_MSG_TEST, test, 1, NULL, 0);
}

/* a6xx_gmu_set_oob / clear_oob (legacy bits) */
static bool
gmu_oob_set(uint32_t set, uint32_t ack)
{
	bool ok;

	wr(gmu_base + GMU_WORD(GMU_H2G_INTR_SET_W), 1u << set);
	__asm__ volatile ("dsb sy" ::: "memory");
	ok = gmu_poll(gmu_base + GMU_WORD(GMU_G2H_INTR_INFO_W), 1u << ack, 1u << ack, 1000, 100);
	printf("mi8gpu: OOB request bit %u: GMU2HOST_INTR_INFO %#010x (ack bit %u %s)\n", set,
	    rd(gmu_base + GMU_WORD(GMU_G2H_INTR_INFO_W)), ack, ok ? "set" : "NOT set within 100 ms");
	wr(gmu_base + GMU_WORD(GMU_G2H_INTR_CLR_W), 1u << ack);
	return ok;
}

static void
gmu_oob_clear(uint32_t bit)
{
	wr(gmu_base + GMU_WORD(GMU_H2G_INTR_SET_W), 1u << bit);
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* a6xx_gmu_set_freq, legacy firmware: the wanted level's index in the table of the HFI PERF_TABLE message, the bus left to the
 * firmware (index 0xff), and the DCVS out-of-band request (set 23, ack 31, clear 31) to make the GMU act on it. */
#define GMU_DCVS_ACK_OPTION_W   0x23fcu
#define GMU_DCVS_PERF_SETTING_W 0x23fdu
#define GMU_DCVS_BW_SETTING_W   0x23feu
#define GMU_DCVS_RETURN_W       0x23ffu

static bool
gmu_set_freq(uint32_t index)
{
	bool ok;

	wr(gmu_base + GMU_WORD(GMU_DCVS_ACK_OPTION_W), 0);
	wr(gmu_base + GMU_WORD(GMU_DCVS_PERF_SETTING_W), (3u << 28) | index);
	wr(gmu_base + GMU_WORD(GMU_DCVS_BW_SETTING_W), 0xff);
	ok = gmu_oob_set(23, 31);
	gmu_oob_clear(31);
	sleep_us(1000);
	printf("mi8gpu: GPU frequency index %u asked for (%u kHz): DCVS request %s, GMU_DCVS_RETURN %#x\n", index,
	    index < sizeof(gpu_perf) / sizeof(gpu_perf[0]) ? gpu_perf[index].khz : 0, ok ? "acknowledged" : "NOT acknowledged",
	    rd(gmu_base + GMU_WORD(GMU_DCVS_RETURN_W)));
	return ok;
}

static void
gmu_power_look(const char *when)
{
	printf("mi8gpu: %s: GX GDSCR %#010x (alias %#010x) RPMH_POWER_STATE %#x SPTPRAC %#x BUSY %#010x INTR_INFO %#010x\n", when,
	    rd(gpucc + GPUCC_GX_GDSCR), rd(gmu_base + GMU_WORD(GMU_GX_GDSCR_ALIAS_W)),
	    rd(gmu_base + GMU_WORD(GMU_RPMH_POWER_STATE_W)), rd(gmu_base + GMU_WORD(GMU_SPTPRAC_STATUS_W)),
	    rd(gmu_base + GMU_WORD(GMU_BUSY_STATUS_W)), rd(gmu_base + GMU_WORD(GMU_G2H_INTR_INFO_W)));
}

/* a6xx_sptprac_enable / a6xx_sptprac_disable (legacy, idle level ACTIVE: the host owns SPTP power collapse) */
static void
gmu_sptprac_enable(void)
{
	wr(gmu_base + GMU_WORD(GMU_SPTPRAC_CTRL_W), 0x778000);
	bool ok = gmu_poll(gmu_base + GMU_WORD(GMU_SPTPRAC_STATUS_W), 0x38, 0x28, 100, 1);
	printf("mi8gpu: SPTPRAC enable: status %#x (%s)\n", rd(gmu_base + GMU_WORD(GMU_SPTPRAC_STATUS_W)), ok ? "on" : "NOT on");
}

static void
gmu_sptprac_disable(void)
{
	wr(gmu_base + GMU_WORD(GMU_GX_GDSCR_ALIAS_W), rd(gmu_base + GMU_WORD(GMU_GX_GDSCR_ALIAS_W)) | (1u << 11));
	wr(gmu_base + GMU_WORD(GMU_SPTPRAC_CTRL_W), 0x778001);
	bool ok = gmu_poll(gmu_base + GMU_WORD(GMU_SPTPRAC_STATUS_W), 0x04, 0x04, 100, 100);
	printf("mi8gpu: SPTPRAC disable: status %#x (%s), GX GDSCR %#010x\n", rd(gmu_base + GMU_WORD(GMU_SPTPRAC_STATUS_W)),
	    ok ? "off" : "NOT off", rd(gpucc + GPUCC_GX_GDSCR));
}


/* ---- step 18: read the 3D core's registers while the GMU holds GPU_SET (nothing is written) -------------------- */

#define GPU_PHYS                0x05000000UL    /* qcom,kgsl-3d0 (F-029) */
#define GPU_SIZE                0x40000UL

static vm_offset_t gpu3d;

/* ml_io_map allocates new address space on every call (and panics when it runs out, EXP-0106): map once. */
static vm_offset_t
gpu_map(void)
{
	if (gpu3d == 0) {
		gpu3d = ml_io_map(GPU_PHYS, GPU_SIZE);
	}
	return gpu3d;
}

static void
gpu_look(void)
{
	static const struct { const char *name; uint32_t word; } regs[] = {
		{ "RBBM_STATUS", 0x0210 }, { "RBBM_STATUS3", 0x0213 }, { "RBBM_INT_0_STATUS", 0x0201 },
		{ "RBBM_VBIF_GX_RESET_STATUS", 0x0215 }, { "RBBM_SECVID_TRUST_CNTL", 0xf400 }, { "RBBM_CLOCK_CNTL", 0x00ae },
		{ "RBBM_PERFCTR_CNTL", 0x0500 }, { "CP_SQE_CNTL", 0x0808 }, { "CP_RB_RPTR", 0x0806 }, { "CP_RB_WPTR", 0x0807 },
		{ "CP_CHICKEN_DBG", 0x0841 },
	};
	vm_offset_t gpu = gpu_map();

	for (uint32_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		printf("mi8gpu: reading 3D core %s (word %#x)\n", regs[i].name, regs[i].word);
		printf("mi8gpu: 3D core %-28s = %#010x\n", regs[i].name, rd(gpu + 4 * regs[i].word));
	}
}


/* ---- step 19: the a630 hw_init register writes, in TASK-002's ranges, each read back (no SQE, no CP, no IRQ) ---- */

#include "perseus_a630_hw_init.inc"

#define GMU_AO_CGC_MODE_W       0x9309u         /* GPU_GMU_AO_GMU_CGC_MODE_CNTL 0x23b09 */
#define GMU_AO_CGC_DELAY_W      0x930au
#define GMU_AO_CGC_HYST_W       0x930bu
#define GMU_AO_BUSY_MASK_W      0x930eu         /* GPU_GMU_AO_GPU_CX_BUSY_MASK 0x23b0e */
#define GMU_GX_SPTPRAC_CLK_W    0x0080u         /* GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL 0x1a880 */
#define GMU_PWR_CNT_ENABLE_W    0x5040u
#define GMU_PWR_CNT_SELECT0_W   0x5041u         /* GMU_CX_GMU_POWER_COUNTER_SELECT_0 0x1f841 */

static uint32_t gpu_writes, gpu_differs;

static void
gpu_put(uint32_t word, uint32_t value, const char *name)
{
	uint32_t back;

	if ((gpu_writes++ & 7) == 0) {
		printf("mi8gpu: 3D core write #%u %s (word %#x)\n", gpu_writes, name, word);
	}
	wr(gpu3d + 4 * word, value);
	__asm__ volatile ("dsb sy" ::: "memory");
	back = rd(gpu3d + 4 * word);
	if (back != value) {
		gpu_differs++;
		printf("mi8gpu: 3D core %s (word %#x): wrote %#010x read back %#010x\n", name, word, value, back);
	}
}

static void
gpu_range(const char *what, uint32_t first, uint32_t end)
{
	uint32_t before = gpu_differs;

	printf("mi8gpu: writing %s: entries %u..%u\n", what, first, end);
	for (uint32_t i = first; i < end; i++) {
		gpu_put(a630_hw_init_table[i].reg, a630_hw_init_table[i].value, a630_hw_init_table[i].name);
	}
	printf("mi8gpu: %s done: %u of %u read back different\n", what, gpu_differs - before, end - first);
}

static void
gpu_init(void)
{
	uint32_t clock_cntl, v;
	bool secvid_done;

	gpu_map();
	/*
	 * hw_init:1204, 1214-1215: the trusted-buffer registers are zeroed first. The phone's device tree says
	 * qcom,gpu-quirk-secvid-set-once: TrustZone lets the SECVID registers be written once, and a second write
	 * resets the phone (EXP-0110 -> EXP-0111, EXP-0114 run 2 -> 3, U-060). After the first run the 3D core is out of
	 * secure mode and keeps that across a slumber, so a core that reads SECVID_TRUST_CNTL 0 has had them set.
	 */
	secvid_done = rd(gpu3d + 4 * 0xf400) == 0;
	if (secvid_done) {
		printf("mi8gpu: SECVID_TRUST_CNTL is already 0: the SECVID registers have been set (set-once): not written again\n");
	} else {
		for (uint32_t i = 0; i < 4; i++) {
			gpu_put(a630_caller_security_table[i].reg, a630_caller_security_table[i].value, a630_caller_security_table[i].name);
		}
	}
	gpu_range("ADDRESS", A630_INIT_ADDRESS_BEGIN, A630_INIT_ADDRESS_END);
	if (!secvid_done) {
		gpu_put(a630_caller_security_table[4].reg, a630_caller_security_table[4].value, a630_caller_security_table[4].name);
	}
	/* a6xx_set_hwcg: GMU clock-gating mode/delay/hysteresis, then see whether the GPU's is already set */
	wr(gmu_base + GMU_WORD(GMU_AO_CGC_MODE_W), 0x20202);
	wr(gmu_base + GMU_WORD(GMU_AO_CGC_DELAY_W), 0x10111);
	wr(gmu_base + GMU_WORD(GMU_AO_CGC_HYST_W), 0x5555);
	printf("mi8gpu: GMU CGC mode/delay/hyst now %#x/%#x/%#x\n", rd(gmu_base + GMU_WORD(GMU_AO_CGC_MODE_W)),
	    rd(gmu_base + GMU_WORD(GMU_AO_CGC_DELAY_W)), rd(gmu_base + GMU_WORD(GMU_AO_CGC_HYST_W)));
	clock_cntl = rd(gpu3d + 4 * 0xae);
	printf("mi8gpu: RBBM_CLOCK_CNTL %#010x (the table assumes a value other than 0x8aa8aa02)\n", clock_cntl);
	if (clock_cntl == 0x8aa8aa02) {
		printf("mi8gpu: hardware clock gating is already on; HWCG and its commit are skipped as in Linux\n");
	} else {
		v = rd(gmu_base + GMU_WORD(GMU_GX_SPTPRAC_CLK_W));
		printf("mi8gpu: GX_SPTPRAC_CLOCK_CONTROL %#x: clearing bit 0\n", v);
		wr(gmu_base + GMU_WORD(GMU_GX_SPTPRAC_CLK_W), v & ~1u);
		gpu_range("HWCG", A630_INIT_HWCG_BEGIN, A630_INIT_HWCG_END);
		wr(gmu_base + GMU_WORD(GMU_GX_SPTPRAC_CLK_W), rd(gmu_base + GMU_WORD(GMU_GX_SPTPRAC_CLK_W)) | 1u);
		printf("mi8gpu: GX_SPTPRAC_CLOCK_CONTROL now %#x\n", rd(gmu_base + GMU_WORD(GMU_GX_SPTPRAC_CLK_W)));
		gpu_range("HWCG_COMMIT", A630_INIT_HWCG_COMMIT_BEGIN, A630_INIT_HWCG_COMMIT_END);
	}
	gpu_range("CORE", A630_INIT_CORE_BEGIN, A630_INIT_CORE_END);
	/* hw_init:1369-1373: GMU busy mask and power counters */
	wr(gmu_base + GMU_WORD(GMU_AO_BUSY_MASK_W), 0xff000000);
	v = rd(gmu_base + GMU_WORD(GMU_PWR_CNT_SELECT0_W));
	wr(gmu_base + GMU_WORD(GMU_PWR_CNT_SELECT0_W), (v & ~0xffu) | (1u << 5));
	wr(gmu_base + GMU_WORD(GMU_PWR_CNT_ENABLE_W), 1);
	printf("mi8gpu: GMU busy mask %#x, power counter select %#x, enable %#x\n", rd(gmu_base + GMU_WORD(GMU_AO_BUSY_MASK_W)),
	    rd(gmu_base + GMU_WORD(GMU_PWR_CNT_SELECT0_W)), rd(gmu_base + GMU_WORD(GMU_PWR_CNT_ENABLE_W)));
	gpu_range("PROTECT", A630_INIT_PROTECT_BEGIN, A630_INIT_PROTECT_END);
	printf("mi8gpu: IRQ entry %u is not written (no interrupt path); SECVID_TRUST_CNTL is not written (zap's job)\n", A630_INIT_IRQ_BEGIN);
	printf("mi8gpu: 3D core writes: %u, read back different: %u; RBBM_STATUS %#x, INT_0_STATUS %#x, SECVID_TRUST_CNTL %#x\n",
	    gpu_writes, gpu_differs, rd(gpu3d + 4 * 0x210), rd(gpu3d + 4 * 0x201), rd(gpu3d + 4 * 0xf400));
}


/* ---- step 20, second half: SQE microcode, ring, CP_SQE_CNTL=1 and the CP_ME_INIT packet (a6xx_gpu.c hw_init/cp_init) ---- */

static uint32_t
pm4_parity(uint32_t val)
{
	return (0x9669 >> (0xF & (val ^ (val >> 4) ^ (val >> 8) ^ (val >> 12) ^ (val >> 16) ^ (val >> 20) ^ (val >> 24) ^ (val >> 28)))) & 1;
}

static uint32_t
pkt7(uint32_t opcode, uint32_t cnt)
{
	return 0x70000000u | cnt | (pm4_parity(cnt) << 15) | ((opcode & 0x7f) << 16) | (pm4_parity(opcode) << 23);
}

#define CP_ME_INIT_OPCODE       0x48u
#define CP_MEM_WRITE_OPCODE    0x3du
#define CP_SET_SECURE_MODE_OPCODE 0x66u

static bool zap_authenticated;
static bool zap_load(bool keep);
static void zap_release(void);
static void gpu_fill_screen(void);
static void screen_check(const char *when);
static uint32_t cp_wptr;
static bool cp_ok;
static bool mi8_keep;           /* boot-arg gpu=1: the GPU is started and kept on for the window server */
static uint32_t gmu_old_ao, gmu_old_h2h;   /* the GMU interrupt masks found at the start */
static bool gpu_nonsecure;      /* zap authenticated and the CP has switched the 3D core out of secure mode */
static bool gpu_kept;

static void
gpu_cp_start(void)
{
	static const uint32_t me_init[8] = { 0x2f, 0x3, 0x20000000, 0, 0, 0, 0, 0 };
	uint32_t *ring = (uint32_t *)g3d_regions[1].memory, wptr = 0, rptr = 0, status = 0, hang = 0;
	vm_offset_t cb = smmu_cb(G3D_CTX);
	bool done = false;

	if (!g3d_ready || gpu3d == 0) {
		printf("mi8gpu: the 3D core's address space is not ready; the CP is not started\n");
		return;
	}
	/* GX registers survive a slumber (EXP-0108: HWCG was still on): make sure the CP of an earlier run is halted first */
	printf("mi8gpu: CP_SQE_CNTL was %#x, RB_RPTR %u WPTR %u; halting it before the setup\n", rd(gpu3d + 4 * 0x0808), rd(gpu3d + 4 * 0x0806), rd(gpu3d + 4 * 0x0807));
	gpu_put(0x0808, 0, "REG_A6XX_CP_SQE_CNTL (halt)");
	wr(gpu3d + 4 * 0x0807, 0);
	/* hw_init: SQE_INSTR_BASE, RB_BASE, RB_CNTL (default size/block with NO_UPDATE: no hw_apriv on a630) */
	gpu_put(0x0830, (uint32_t)g3d_regions[0].address, "REG_A6XX_CP_SQE_INSTR_BASE_LO");
	gpu_put(0x0831, (uint32_t)(g3d_regions[0].address >> 32), "REG_A6XX_CP_SQE_INSTR_BASE_HI");
	gpu_put(0x0800, (uint32_t)g3d_regions[1].address, "REG_A6XX_CP_RB_BASE_LO");
	gpu_put(0x0801, (uint32_t)(g3d_regions[1].address >> 32), "REG_A6XX_CP_RB_BASE_HI");
	gpu_put(0x0802, (12u | (2u << 8) | (1u << 27)), "REG_A6XX_CP_RB_CNTL");
	/* a6xx_cp_init: CP_ME_INIT with 8 words */
	ring[wptr++] = pkt7(CP_ME_INIT_OPCODE, 8);
	for (uint32_t i = 0; i < 8; i++) {
		ring[wptr++] = me_init[i];
	}
	to_memory(g3d_regions[1].memory, g3d_regions[1].size);
	printf("mi8gpu: ring: %u words, first %#010x %#010x %#010x; CP_SQE_CNTL <- 1\n", wptr, ring[0], ring[1], ring[2]);
	gpu_put(0x0808, 1, "REG_A6XX_CP_SQE_CNTL");
	printf("mi8gpu: CP_RB_WPTR <- %u (a6xx_flush)\n", wptr);
	wr(gpu3d + 4 * 0x0807, wptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < 500; i++) {
		rptr = rd(gpu3d + 4 * 0x0806);
		if (rptr == wptr) {
			done = true;
			break;
		}
		sleep_us(100);
	}
	status = rd(gpu3d + 4 * 0x0210);
	hang = rd(gpu3d + 4 * 0x0201);
	cp_wptr = wptr;
	cp_ok = done;
	printf("mi8gpu: CP %s: RB_RPTR %u WPTR %u, RBBM_STATUS %#010x INT_0_STATUS %#010x\n", done ? "CONSUMED the ring" : "did NOT consume the ring",
	    rptr, rd(gpu3d + 4 * 0x0807), status, hang);
	printf("mi8gpu: after: SQE_CNTL %#x, 3D core context bank %u FSR %#010x FSYNR0 %#010x FAR %#010x, global FSR %#010x\n",
	    rd(gpu3d + 4 * 0x0808), G3D_CTX, rd(cb + CB_FSR), rd(cb + CB_FSYNR0), rd(cb + CB_FAR_LO), rd(smmu + SMMU_SGFSR));
}


/* ---- step 21: zap shader + CP_SET_SECURE_MODE(0) (a6xx hw_init:1491-1498), then a first real command ------------- */

static bool
gpu_wait_ring(uint32_t wptr, const char *what)
{
	uint32_t rptr = 0;

	to_memory(g3d_regions[1].memory, g3d_regions[1].size);
	wr(gpu3d + 4 * 0x0807, wptr);
	__asm__ volatile ("dsb sy" ::: "memory");
	for (uint32_t i = 0; i < 500; i++) {
		rptr = rd(gpu3d + 4 * 0x0806);
		if (rptr == wptr) {
			break;
		}
		sleep_us(100);
	}
	/* a6xx_idle: RBBM_STATUS without CP_AHB_BUSY_CX_MASTER (bit 0) must be 0 and no hang interrupt */
	for (uint32_t i = 0; i < 1000 && (rd(gpu3d + 4 * 0x0210) & ~0x1u); i++) {
		sleep_us(100);
	}
	printf("mi8gpu: %s: RB_RPTR %u WPTR %u, RBBM_STATUS %#010x INT_0_STATUS %#010x, CB1 FSR %#010x FSYNR0 %#x\n", what, rptr, wptr,
	    rd(gpu3d + 4 * 0x0210), rd(gpu3d + 4 * 0x0201), rd(smmu_cb(G3D_CTX) + CB_FSR), rd(smmu_cb(G3D_CTX) + CB_FSYNR0));
	return rptr == wptr && !(rd(gpu3d + 4 * 0x0210) & ~0x1u);
}

static void
gpu_zap_and_command(void)
{
	uint32_t *ring = (uint32_t *)g3d_regions[1].memory, wptr = cp_wptr;
	volatile uint32_t *memptrs = (volatile uint32_t *)g3d_regions[2].memory;

	if (!cp_ok || !g3d_ready) {
		printf("mi8gpu: the CP has not taken CP_ME_INIT; no zap\n");
		return;
	}
	printf("mi8gpu: SECVID_TRUST_CNTL before: %#x\n", rd(gpu3d + 4 * 0xf400));
	bool ok;

	if (rd(gpu3d + 4 * 0xf400) == 0) {
		/* a slumber leaves the core where it was (EXP-0114): out of secure mode, with the zap still authenticated if it was kept */
		printf("mi8gpu: the 3D core is already out of secure mode: the zap shader is not loaded again, no CP_SET_SECURE_MODE\n");
		ok = true;
	} else {
		if (!zap_load(true)) {
			printf("mi8gpu: zap not authenticated; the secure mode is left as it is\n");
			return;
		}
		ring[wptr++] = pkt7(CP_SET_SECURE_MODE_OPCODE, 1);
		ring[wptr++] = 0;
		printf("mi8gpu: CP_SET_SECURE_MODE(0) submitted\n");
		ok = gpu_wait_ring(wptr, "after CP_SET_SECURE_MODE");
	}
	printf("mi8gpu: SECVID_TRUST_CNTL after: %#x (%s); GPU idle: %s\n", rd(gpu3d + 4 * 0xf400),
	    rd(gpu3d + 4 * 0xf400) == 0 ? "NON-SECURE MODE" : "still secure", ok ? "yes" : "NO");
	gpu_nonsecure = ok && rd(gpu3d + 4 * 0xf400) == 0;
	if (!ok) {
		return;
	}
	/* the first real command: CP_MEM_WRITE of two words into the memptrs page, read back by us afterwards */
	memptrs[2] = memptrs[3] = 0;
	to_memory((vm_offset_t)memptrs, 0x1000);
	ring[wptr++] = pkt7(CP_MEM_WRITE_OPCODE, 4);
	ring[wptr++] = (uint32_t)(g3d_regions[2].address + 8);
	ring[wptr++] = (uint32_t)((g3d_regions[2].address + 8) >> 32);
	ring[wptr++] = 0xc0ffee21;
	ring[wptr++] = 0x0badf00d;
	printf("mi8gpu: CP_MEM_WRITE submitted (iova %#llx <- c0ffee21 0badf00d)\n", g3d_regions[2].address + 8);
	ok = gpu_wait_ring(wptr, "after CP_MEM_WRITE");
	cp_wptr = wptr;
	to_memory((vm_offset_t)memptrs, 0x1000);
	printf("mi8gpu: memptrs[2..3] = %#010x %#010x -> the CP's first write %s\n", memptrs[2], memptrs[3],
	    (memptrs[2] == 0xc0ffee21 && memptrs[3] == 0x0badf00d) ? "ARRIVED in memory" : "did NOT arrive");
	if (ok && gmu_last_step >= 22 && memptrs[2] == 0xc0ffee21 && memptrs[3] == 0x0badf00d) {
		uint32_t passes = 1;

		PE_parse_boot_argn("mi8gpu_paints", &passes, sizeof(passes));
		cp_wptr = wptr;
		for (uint32_t pass = 0; pass < passes && pass < 60; pass++) {
			printf("mi8gpu: painting pass %u of %u\n", pass + 1, passes);
			gpu_fill_screen();
			if (pass + 1 < passes) {
				sleep_us(700000);
				screen_check("before the next pass (what is on the screen now)");
			}
		}
	}
}


/* ---- step 22: the CP, not the CPU, writes the picture into the screen's frame buffer ------------------------ */


static uint32_t
screen_expected(uint32_t x, uint32_t y)
{
	static const uint32_t bars[8] = { 0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000 };

	if (y >= G3D_FB_HEIGHT - 240) {
		uint32_t v = x * 255 / (G3D_FB_WIDTH - 1);

		return 0xff000000u | (v << 16) | (v << 8) | v;
	}
	if (y < 8 || x < 8 || x >= G3D_FB_WIDTH - 8) {
		return 0xffffffff;
	}
	return bars[x * 8 / G3D_FB_WIDTH];
}

/* Read the frame buffer back through the console's mapping (write-combining: no CPU cache holds old pixels). */
static void
screen_check(const char *when)
{
	volatile uint32_t *fb = (volatile uint32_t *)vinfo.v_baseaddr;
	uint32_t same = 0, differ = 0, seed = 12345;

	if (fb == NULL || vinfo.v_rowbytes != G3D_FB_WIDTH * 4 || vinfo.v_height != G3D_FB_HEIGHT) {
		printf("mi8gpu: %s: the console's mapping is not usable for a check (base %#lx rowbytes %u height %u)\n", when, vinfo.v_baseaddr,
		    vinfo.v_rowbytes, vinfo.v_height);
		return;
	}
	for (uint32_t i = 0; i < 4000; i++) {
		uint32_t x, y;

		seed = seed * 1664525u + 1013904223u;
		x = (seed >> 8) % G3D_FB_WIDTH;
		seed = seed * 1664525u + 1013904223u;
		y = (seed >> 8) % G3D_FB_HEIGHT;
		if ((fb[y * G3D_FB_WIDTH + x] & 0xffffffu) == (screen_expected(x, y) & 0xffffffu)) {
			same++;
		} else {
			differ++;
		}
	}
	printf("mi8gpu: %s: %u of 4000 sampled pixels are the CP's pattern, %u are something else\n", when, same, differ);
}

static void
gpu_fill_screen(void)
{
	uint32_t *ring = (uint32_t *)g3d_regions[1].memory, wptr = cp_wptr & 8191u, sent = 0;
	const uint32_t total = G3D_FB_WIDTH * G3D_FB_HEIGHT, chunk = 1000, ring_words = 8192;
	/* BGRA words 0xAARRGGBB: white, yellow, cyan, green, magenta, red, blue, black */
	static const uint32_t bars[8] = { 0xffffffff, 0xffffff00, 0xff00ffff, 0xff00ff00, 0xffff00ff, 0xffff0000, 0xff0000ff, 0xff000000 };
	uint64_t t0 = mach_absolute_time(), t1;
	uint32_t batches = 0;
	bool ok = true;

	printf("mi8gpu: painting the screen: %u x %u pixels, %u dwords, from iova %#llx (phys %#llx); kernel's own screen: base %#lx rowbytes %lu %lux%lu\n",
	    G3D_FB_WIDTH, G3D_FB_HEIGHT, total, (uint64_t)G3D_FB_IOVA, (uint64_t)G3D_FB_PHYS, PE_state.video.v_baseAddr,
	    PE_state.video.v_rowBytes, PE_state.video.v_width, PE_state.video.v_height);
	while (sent < total && ok) {
		uint32_t batch_start = wptr;

		for (uint32_t c = 0; c < 6 && sent < total; c++) {
			uint32_t n = total - sent < chunk ? total - sent : chunk;
			uint64_t addr = G3D_FB_IOVA + 4ull * sent;
			uint32_t w = wptr;

			ring[w++ & (ring_words - 1)] = pkt7(CP_MEM_WRITE_OPCODE, n + 2);
			ring[w++ & (ring_words - 1)] = (uint32_t)addr;
			ring[w++ & (ring_words - 1)] = (uint32_t)(addr >> 32);
			for (uint32_t i = 0; i < n; i++) {
				uint32_t pixel = sent + i, x = pixel % G3D_FB_WIDTH, y = pixel / G3D_FB_WIDTH, v;

				if (y >= G3D_FB_HEIGHT - 240) {                 /* the bottom strip: a grey ramp, shows the row/column order */
					v = x * 255 / (G3D_FB_WIDTH - 1);
					ring[w++ & (ring_words - 1)] = 0xff000000u | (v << 16) | (v << 8) | v;
				} else if (y < 8 || x < 8 || x >= G3D_FB_WIDTH - 8) {  /* a white frame at the top and the sides */
					ring[w++ & (ring_words - 1)] = 0xffffffff;
				} else {
					ring[w++ & (ring_words - 1)] = bars[x * 8 / G3D_FB_WIDTH];
				}
			}
			wptr = w & (ring_words - 1);
			sent += n;
		}
		(void)batch_start;
		/* flush the whole ring to memory, move WPTR, wait until the CP has read it all */
		to_memory(g3d_regions[1].memory, g3d_regions[1].size);
		wr(gpu3d + 4 * 0x0807, wptr);
		__asm__ volatile ("dsb sy" ::: "memory");
		bool drained = false;
		for (uint32_t i = 0; i < 20000; i++) {
			if (rd(gpu3d + 4 * 0x0806) == wptr) {
				drained = true;
				break;
			}
			sleep_us(10);
		}
		if (!drained) {
			printf("mi8gpu: the CP stopped reading the ring: RPTR %u WPTR %u after %u dwords; RBBM_STATUS %#x INT_0 %#x CB1 FSR %#x FAR %#x\n",
			    rd(gpu3d + 4 * 0x0806), wptr, sent, rd(gpu3d + 4 * 0x0210), rd(gpu3d + 4 * 0x0201),
			    rd(smmu_cb(G3D_CTX) + CB_FSR), rd(smmu_cb(G3D_CTX) + CB_FAR_LO));
			ok = false;
		}
		batches++;
	}
	t1 = mach_absolute_time();
	cp_wptr = wptr;
	printf("mi8gpu: the CP read %u dwords in %u submissions (%s); RBBM_STATUS %#x INT_0 %#x CB1 FSR %#x; timer ticks %llu\n", sent, batches,
	    ok ? "all" : "STOPPED", rd(gpu3d + 4 * 0x0210), rd(gpu3d + 4 * 0x0201), rd(smmu_cb(G3D_CTX) + CB_FSR), t1 - t0);
	screen_check("right after the CP finished");
}


/* ---- step 23: the GPU kept on for the window server (RedwoodGPU service; bsd/sys/redwood_input.h) --------------- */

/*
 * What Mesa's freedreno (through tools/mesa/redwood-drm and iokit/Kernel/RedwoodGPU.cpp) needs of the kernel: addresses
 * for buffers (the kernel is the allocator, buffers are asked MSM_INFO_GET_IOVA once), pages mapped at them in the 3D
 * core's context (CB1), the ring, submissions with a fence the core writes into memory, and Linux's GET_PARAM answers
 * for an a630. This follows the structure of bsd/dev/redwood_adreno.c's "GPU kept on", for this phone's registers.
 */
#define KEEP_VA_START           0x40000000ull   /* above the steps' own blocks, below 4 GB (a630: ADRENO_QUIRK_4GB_VA) */
#define KEEP_VA_SIZE            0xb0000000ull
#define KEEP_GRANULE            0x10000ull
#define KEEP_FENCE_AT           0x40u           /* in the memptrs page: where the core writes the finished submission */
#define KEEP_RING_WORDS         8192u
#define CP_INDIRECT_BUFFER_OP   0x3fu
#define CP_EVENT_WRITE_OP       0x46u
#define EVENT_CACHE_FLUSH_TS    0x04u
#define EVENT_PC_CCU_INV_DEPTH  0x18u
#define EVENT_PC_CCU_INV_COLOR  0x19u
#define KEEP_PTE_READ_ONLY      (1ull << 7)

static struct { uint64_t start, size; } keep_free[1024];
static unsigned keep_free_count, keep_free_most;
static uint64_t keep_used, keep_lost;
static uint32_t keep_live, keep_refused, keep_fence, keep_ring_at, keep_ring_sent;
static struct { uint64_t phys; vm_offset_t virt; } keep_tables[512];
static uint32_t keep_tables_n;
static vm_offset_t keep_pool;
static uint32_t keep_pool_used = 4;     /* of the 4 tables one page holds */

static uint64_t *
keep_table_virt(uint64_t phys)
{
	for (uint32_t i = 1; i < G3D_TABLE_POOL / 0x1000; i++) {
		if (kvtophys(g3d_tables + 0x1000 * i) == phys) {
			return (uint64_t *)(g3d_tables + 0x1000 * i);
		}
	}
	for (uint32_t i = 0; i < keep_tables_n; i++) {
		if (keep_tables[i].phys == phys) {
			return (uint64_t *)keep_tables[i].virt;
		}
	}
	return NULL;
}

/* One new (zeroed) 4 KB table of the 3D core's page tables. */
static uint64_t *
keep_table_new(uint64_t *phys)
{
	vm_offset_t v;

	if (keep_pool == 0 || keep_pool_used >= PAGE_SIZE / 0x1000) {
		if (keep_tables_n + PAGE_SIZE / 0x1000 > sizeof(keep_tables) / sizeof(keep_tables[0]) ||
		    kmem_alloc(kernel_map, &keep_pool, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			return NULL;
		}
		keep_pool_used = 0;
	}
	v = keep_pool + 0x1000 * keep_pool_used++;
	*phys = kvtophys(v);
	keep_tables[keep_tables_n].phys = *phys;
	keep_tables[keep_tables_n++].virt = v;
	to_memory(v, 0x1000);
	return (uint64_t *)v;
}

static uint64_t keep_attr_index;        /* boot-arg mi8gpu_attr=N: memory attribute index of the core's pages (0 not cached, 1 cached) */

static bool
keep_map(uint64_t va, uint64_t pa, uint64_t attributes)
{
	uint64_t *l1 = (uint64_t *)g3d_tables, *l2, *l3, phys;
	uint64_t *e1 = &l1[(va >> 30) & 0x1ff], *e2, *e3;

	if (*e1 == 0) {
		l2 = keep_table_new(&phys);
		if (l2 == NULL) {
			return false;
		}
		*e1 = phys | PTE_TABLE;
		to_memory((vm_offset_t)e1, 8);
	}
	l2 = keep_table_virt(*e1 & PTE_ADDRESS);
	if (l2 == NULL) {
		return false;
	}
	e2 = &l2[(va >> 21) & 0x1ff];
	if (*e2 == 0) {
		l3 = keep_table_new(&phys);
		if (l3 == NULL) {
			return false;
		}
		*e2 = phys | PTE_TABLE;
		to_memory((vm_offset_t)e2, 8);
	}
	l3 = keep_table_virt(*e2 & PTE_ADDRESS);
	if (l3 == NULL) {
		return false;
	}
	e3 = &l3[(va >> 12) & 0x1ff];
	*e3 = pa ? (pa | PTE_GMU | PTE_AP_UNPRIV | PTE_PAGE | (keep_attr_index << 2) | attributes) : 0;
	to_memory((vm_offset_t)e3, 8);
	return true;
}

static void
keep_tlb(void)
{
	vm_offset_t cb = smmu_cb(G3D_CTX);

	__asm__ volatile ("dsb sy" ::: "memory");
	wr(cb + CB_TLBIASID, G3D_CTX);
	wr(cb + CB_TLBSYNC, 1);
	for (int i = 0; i < 1000 && (rd(cb + CB_TLBSTATUS) & 1); i++) {
		sleep_us(1);
	}
}

static void
keep_ring(uint32_t word)
{
	((uint32_t *)g3d_regions[1].memory)[keep_ring_at] = word;
	keep_ring_at = (keep_ring_at + 1) & (KEEP_RING_WORDS - 1);
}

static void
keep_ring_cmd(uint32_t command, uint32_t words)
{
	keep_ring(pkt7(command, words));
}

static void
keep_ring_send(void)
{
	if (keep_ring_at >= keep_ring_sent) {
		to_memory(g3d_regions[1].memory + 4 * keep_ring_sent, 4 * (keep_ring_at - keep_ring_sent));
	} else {
		to_memory(g3d_regions[1].memory + 4 * keep_ring_sent, 4 * (KEEP_RING_WORDS - keep_ring_sent));
		to_memory(g3d_regions[1].memory, 4 * keep_ring_at);
	}
	keep_ring_sent = keep_ring_at;
	wr(gpu3d + 4 * 0x0807, keep_ring_at);
	__asm__ volatile ("dsb sy" ::: "memory");
}

bool
mi8_gpu_kept(void)
{
	return gpu_kept;
}

/* Linux's adreno_get_param for an a630 (the numbers are msm_drm.h's). */
bool
mi8_gpu_parameter(uint32_t which, uint64_t *value)
{
	switch (which) {
	case 0x01:      /* GPU_ID */
	case 0x06:      /* GMEM_BASE */
	case 0x08:      /* PP_PGTABLE */
	case 0x09:      /* FAULTS */
	case 0x0a:      /* SUSPENDS */
	case 0x11:      /* RAYTRACING */
		*value = 0;
		return true;
	case 0x02:      /* GMEM_SIZE: 1 MiB (a6xx_catalog.c, TASK-002) */
		*value = 1024 * 1024;
		return true;
	case 0x03:      /* CHIP_ID */
		*value = 0x06030001ull;
		return true;
	case 0x04:      /* MAX_FREQ */
		*value = 710000000ull;
		return true;
	case 0x05:      /* TIMESTAMP */
		*value = mach_absolute_time();
		return true;
	case 0x07:      /* PRIORITIES */
		*value = 1;
		return true;
	case 0x10:      /* HIGHEST_BANK_BIT (qcom,highest-bank-bit = 15) */
		*value = 15;
		return true;
	case 0x12:      /* UBWC_SWIZZLE (sdm845_data) */
		*value = 6;
		return true;
	case 0x13:      /* MACROTILE_MODE (sdm845_data: 0) */
		*value = 0;
		return true;
	default:
		return false;
	}
}

uint64_t
mi8_gpu_address(uint64_t size)
{
	size = (size + (KEEP_GRANULE - 1)) & ~(KEEP_GRANULE - 1);
	for (unsigned i = 0; i < keep_free_count; i++) {
		if (keep_free[i].size >= size) {
			uint64_t address = keep_free[i].start;

			keep_free[i].start += size;
			keep_free[i].size -= size;
			if (keep_free[i].size == 0) {
				memmove(&keep_free[i], &keep_free[i + 1], (--keep_free_count - i) * sizeof(keep_free[0]));
			}
			keep_used += size;
			keep_live++;
			return address;
		}
	}
	keep_refused++;
	return 0;
}

void
mi8_gpu_address_free(uint64_t address, uint64_t size)
{
	unsigned at;

	size = (size + (KEEP_GRANULE - 1)) & ~(KEEP_GRANULE - 1);
	for (at = 0; at < keep_free_count && keep_free[at].start < address; at++) {
	}
	keep_used -= size;
	keep_live--;
	if (at > 0 && keep_free[at - 1].start + keep_free[at - 1].size == address) {
		keep_free[at - 1].size += size;
		if (at < keep_free_count && address + size == keep_free[at].start) {
			keep_free[at - 1].size += keep_free[at].size;
			memmove(&keep_free[at], &keep_free[at + 1], (--keep_free_count - at) * sizeof(keep_free[0]));
		}
	} else if (at < keep_free_count && address + size == keep_free[at].start) {
		keep_free[at].start = address;
		keep_free[at].size += size;
	} else if (keep_free_count < sizeof(keep_free) / sizeof(keep_free[0])) {
		memmove(&keep_free[at + 1], &keep_free[at], (keep_free_count - at) * sizeof(keep_free[0]));
		keep_free[at].start = address;
		keep_free[at].size = size;
		keep_free_count++;
		if (keep_free_count > keep_free_most) {
			keep_free_most = keep_free_count;
		}
	} else {
		keep_lost += size;
	}
}

void
mi8_gpu_space(uint64_t *used, uint32_t *live, uint64_t *largest, unsigned *holes, unsigned *most, uint64_t *lost, uint32_t *refused)
{
	*largest = 0;
	for (unsigned i = 0; i < keep_free_count; i++) {
		if (keep_free[i].size > *largest) {
			*largest = keep_free[i].size;
		}
	}
	*used = keep_used;
	*live = keep_live;
	*holes = keep_free_count;
	*most = keep_free_most;
	*lost = keep_lost;
	*refused = keep_refused;
}

bool
mi8_gpu_map(uint64_t address, uint64_t phys, bool read_only)
{
	return keep_map(address, phys, read_only ? KEEP_PTE_READ_ONLY : 0);
}

void
mi8_gpu_mapped(void)
{
	keep_tlb();
}

static uint32_t keep_unmap_busy;

/* forward: the unmap path below uses it, the definition follows it in the Mi 8 file */
bool mi8_gpu_done(uint32_t fence);

void
mi8_gpu_unmap(uint64_t address, uint64_t size)
{
	if (!mi8_gpu_done(keep_fence)) {
		keep_unmap_busy++;                      /* U-063: taken away while the core still has work */
		if (keep_unmap_busy <= 8) {
			printf("mi8gpu: unmap %#llx+%#llx while the core has done %u of %u\n", address, size,
			    *(volatile uint32_t *)(g3d_regions[2].memory + KEEP_FENCE_AT), keep_fence);
		}
	}
	for (uint64_t offset = 0; offset < size; offset += 0x1000) {
		keep_map(address + offset, 0, 0);
	}
	keep_tlb();
}

static uint64_t keep_flush_ticks, keep_flush_bytes, keep_flush_calls, keep_submit_ticks;

void
mi8_gpu_to_memory(void *start, uint64_t size)
{
	uint64_t began = mach_absolute_time();

	to_memory((vm_offset_t)start, size);
	keep_flush_ticks += mach_absolute_time() - began;
	keep_flush_bytes += size;
	keep_flush_calls++;
}

bool
mi8_gpu_done(uint32_t fence)
{
	volatile uint32_t *word = (volatile uint32_t *)(g3d_regions[2].memory + KEEP_FENCE_AT);

	to_memory((vm_offset_t)word, 4);
	return (int32_t)(*word - fence) >= 0;
}

uint32_t
mi8_gpu_last(void)
{
	return keep_fence;
}

void
mi8_gpu_worked(uint64_t *microseconds, uint32_t *submissions, uint32_t by[7])
{
	*microseconds = 0;
	*submissions = 0;
	bzero(by, 7 * sizeof(uint32_t));
}

/* Command buffers for the core to run, in order, each an address and a count of words (Linux's a6xx_submit, as
 * redwood_adreno.c puts them): the new fence is returned; 0: the ring is full and the core is not reading it. */
static uint32_t keep_submissions, keep_ibs;
static uint64_t keep_words;

extern unsigned int ml_phys_read_word(vm_offset_t paddr);
#define KEEP_HISTORY 4096
static struct { uint64_t address; uint32_t words, submission; } keep_history[KEEP_HISTORY];

/* the word at a GPU address, by the table we built (0xdeadbeef: not mapped) */
static uint32_t
keep_peek(uint64_t va)
{
	uint64_t *l1 = (uint64_t *)g3d_tables, *l2, *l3;
	uint64_t e = l1[(va >> 30) & 0x1ff];

	if (e == 0 || (l2 = keep_table_virt(e & PTE_ADDRESS)) == NULL || (e = l2[(va >> 21) & 0x1ff]) == 0 ||
	    (l3 = keep_table_virt(e & PTE_ADDRESS)) == NULL || (e = l3[(va >> 12) & 0x1ff]) == 0) {
		return 0xdeadbeef;
	}
	return ml_phys_read_word((e & PTE_ADDRESS) + (va & 0xfff));
}

static uint32_t mi8_gpu_submit_inner(const uint64_t *addresses, const uint32_t *words, uint32_t count);

uint32_t
mi8_gpu_submit(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint64_t began = mach_absolute_time();
	uint32_t fence = mi8_gpu_submit_inner(addresses, words, count);

	keep_submit_ticks += mach_absolute_time() - began;
	return fence;
}

static uint32_t
mi8_gpu_submit_inner(const uint64_t *addresses, const uint32_t *words, uint32_t count)
{
	uint32_t wanted = 16 + 4 * count, room = 0;
	uint64_t fence_iova = g3d_regions[2].address + KEEP_FENCE_AT;

	for (int i = 0; i < 20000; i++) {
		room = (rd(gpu3d + 4 * 0x0806) + KEEP_RING_WORDS - keep_ring_at - 1) % KEEP_RING_WORDS;
		if (room >= wanted) {
			break;
		}
		sleep_us(100);
	}
	if (room < wanted) {
		return 0;
	}
	if (++keep_fence == 0) {
		keep_fence = 1;
	}
	keep_ring_cmd(CP_EVENT_WRITE_OP, 1);
	keep_ring(EVENT_PC_CCU_INV_DEPTH);
	keep_ring_cmd(CP_EVENT_WRITE_OP, 1);
	keep_ring(EVENT_PC_CCU_INV_COLOR);
	for (uint32_t i = 0; i < count; i++) {
		keep_ring_cmd(CP_INDIRECT_BUFFER_OP, 3);
		keep_ring((uint32_t)addresses[i]);
		keep_ring((uint32_t)(addresses[i] >> 32));
		keep_ring(words[i]);
	}
	keep_ring_cmd(CP_EVENT_WRITE_OP, 4);
	keep_ring(EVENT_CACHE_FLUSH_TS);
	keep_ring((uint32_t)fence_iova);
	keep_ring((uint32_t)(fence_iova >> 32));
	keep_ring(keep_fence);
	keep_ring_send();
	keep_submissions++;
	keep_ibs += count;
	for (uint32_t i = 0; i < count; i++) {
		keep_words += words[i];
		keep_history[(keep_ibs - count + i) % KEEP_HISTORY].address = addresses[i];
		keep_history[(keep_ibs - count + i) % KEEP_HISTORY].words = words[i];
		keep_history[(keep_ibs - count + i) % KEEP_HISTORY].submission = keep_submissions;
	}
	/* proof that the system's own frames reach the core: the first few and then every 256th */
	if (keep_submissions <= 40) {
		for (uint32_t i = 0; i < count; i++) {
			printf("mi8gpu: submission %u buffer %u: %#llx, %u words\n", keep_submissions, i, addresses[i], words[i]);
		}
	}
	if ((keep_submissions & 63) == 0) {
		uint64_t flush_ns, submit_ns;

		absolutetime_to_nanoseconds(keep_flush_ticks, &flush_ns);
		absolutetime_to_nanoseconds(keep_submit_ticks, &submit_ns);
		printf("mi8gpu: cost: %u submissions: %llu us in the ring code, %llu us flushing %llu KB in %llu calls\n", keep_submissions,
		    submit_ns / 1000, flush_ns / 1000, keep_flush_bytes >> 10, keep_flush_calls);
	}
	if (keep_submissions <= 3 || (keep_submissions & 255) == 0) {
		printf("mi8gpu: submission %u (fence %u): %u buffers, %u words in the first; ring written up to %u\n", keep_submissions,
		    keep_fence, count, count ? words[0] : 0, keep_ring_at);
	}
	return keep_fence;
}

void
mi8_gpu_report(const char *why)
{
	printf("mi8gpu: gpu: FAR %#x%08x, unmapped while busy %u times\n", rd(smmu_cb(G3D_CTX) + CB_FAR_LO + 4), rd(smmu_cb(G3D_CTX) + CB_FAR_LO),
	    keep_unmap_busy);
	{
		uint64_t ib1 = ((uint64_t)rd(gpu3d + 4 * 0x0929) << 32) | rd(gpu3d + 4 * 0x0928);
		uint32_t rem1 = rd(gpu3d + 4 * 0x092A), rem2 = rd(gpu3d + 4 * 0x092D);
		uint64_t ib2 = ((uint64_t)rd(gpu3d + 4 * 0x092C) << 32) | rd(gpu3d + 4 * 0x092B);

		printf("mi8gpu: gpu: CP IB1 %#llx remaining %u, IB2 %#llx remaining %u, SQE_STAT?\n", ib1, rem1, ib2, rem2);
		for (uint32_t i = 0; i < KEEP_HISTORY && ib1 != 0; i++) {
			uint32_t at = (keep_ibs + KEEP_HISTORY - 1 - i) % KEEP_HISTORY;

			if (keep_history[at].address == ib1 && keep_history[at].words >= rem1) {
				uint32_t pos = keep_history[at].words - rem1, from = pos > 24 ? pos - 24 : 0;

				printf("mi8gpu: gpu: IB1 is of submission %u, %u words, the core is at word %u:\n", keep_history[at].submission,
				    keep_history[at].words, pos);
				(void)from;
				for (uint32_t w = 0; w < keep_history[at].words; w += 6) {
					printf("mi8gpu: gpu:  [%u] %08x %08x %08x %08x %08x %08x\n", w, keep_peek(ib1 + 4 * w), keep_peek(ib1 + 4 * w + 4),
					    keep_peek(ib1 + 4 * w + 8), keep_peek(ib1 + 4 * w + 12), keep_peek(ib1 + 4 * w + 16), keep_peek(ib1 + 4 * w + 20));
				}
				printf("mi8gpu: gpu: IB2 words follow\n");
				for (uint32_t w = 0; ib2 != 0 && w < 3600; w += 6) {
					printf("mi8gpu: gpu: 2[%u] %08x %08x %08x %08x %08x %08x\n", w, keep_peek(ib2 + 4 * w), keep_peek(ib2 + 4 * w + 4),
					    keep_peek(ib2 + 4 * w + 8), keep_peek(ib2 + 4 * w + 12), keep_peek(ib2 + 4 * w + 16), keep_peek(ib2 + 4 * w + 20));
				}
				break;
			}
		}
	}
	printf("mi8gpu: gpu: %s: submission %u is the last, the core has done %u; ring: read up to %u, written up to %u; RBBM_STATUS %#x INT_0 %#x; "
	    "CB1 FSR %#x FSYNR0 %#x\n", why, keep_fence, *(volatile uint32_t *)(g3d_regions[2].memory + KEEP_FENCE_AT), rd(gpu3d + 4 * 0x0806),
	    keep_ring_at, rd(gpu3d + 4 * 0x0210), rd(gpu3d + 4 * 0x0201), rd(smmu_cb(G3D_CTX) + CB_FSR), rd(smmu_cb(G3D_CTX) + CB_FSYNR0));
}

bool
mi8_gpu_recover(void)
{
	return false;
}


/*
 * boot-arg mi8_cpufreq=N (EXP-0127): the two CPU clusters' frequency domains (Linux qcom-cpufreq-hw, sdm845.dtsi:
 * cpufreq@17d43000, domain 0 the little cores and domain 1 at 17d45800 the big): their table of frequencies and the
 * state they are in, and, for N != 0xffff, that state asked for -- N = the table row (the last one is the highest).
 */
/*
 * What mi8_cpufreq() changed is put back when the run ends (mi8_gpu_stop()): PERF_STATE keeps its value across kernel runs (nothing
 * resets it but a restart of the phone), and EXP-0172 ran at the 2092 MHz that EXP-0171 had asked for without anyone saying so.
 * A run that asks for nothing also takes the big cluster back to row 22 (2649 MHz: what the firmware leaves, EXP-0170) if a
 * run before left it somewhere else.
 */
static vm_offset_t cpufreq_base[2];
static uint32_t cpufreq_original[2];
static bool cpufreq_changed[2];

#define CPUFREQ_BIG_DEFAULT_ROW 22

static void
mi8_cpufreq_restore(void)
{
	for (int d = 0; d < 2; d++) {
		if (cpufreq_changed[d] && cpufreq_base[d] != 0) {
			wr(cpufreq_base[d] + 0x920, cpufreq_original[d]);
			__asm__ volatile ("dsb sy" ::: "memory");
			printf("mi8gpu: cpufreq domain %d: PERF_STATE put back to %u (now %u)\n", d, cpufreq_original[d],
			    rd(cpufreq_base[d] + 0x920));
			cpufreq_changed[d] = false;
		}
	}
}

static void
mi8_cpufreq(uint32_t want, uint32_t want_big)
{
	static const uint64_t bases[2] = { 0x17d43000ull, 0x17d45800ull };

	for (int d = 0; d < 2; d++) {
		vm_offset_t base = (vm_offset_t)ml_io_map(bases[d], 0x1400);
		uint32_t rows = 0, state;

		if (base == 0) {
			printf("mi8gpu: cpufreq domain %d: no mapping\n", d);
			continue;
		}
		printf("mi8gpu: cpufreq domain %d: ENABLE %#x, PERF_STATE %#x\n", d, rd(base + 0x0), rd(base + 0x920));
		for (uint32_t row = 0; row < 40; row++) {
			uint32_t lut = rd(base + 0x110 + 32 * row), volt = rd(base + 0x114 + 32 * row);

			if (lut == 0) {
				break;
			}
			rows = row + 1;
			/* LUT row: bits 31:30 clock source, 7:0 L value (multiples of the 19.2 MHz XO), 18:16 cores; source 0: GPLL0/2 */
			printf("mi8gpu: cpufreq domain %d row %u: lut %#010x -> %u MHz, volt %#x\n", d, row, lut,
			    (lut >> 30) ? (uint32_t)((lut & 0xff) * 192 / 10) : 300, volt);
		}
		/* EXP-0171: boot-arg mi8_cpufreq_big=N asks the big cluster (domain 1) for row N and leaves the little one alone */
		uint32_t asked = d == 1 && want_big != 0xffff ? want_big : want;

		if (asked != 0xffff && rows != 0) {
			uint32_t row = asked < rows ? asked : rows - 1;

			if (!cpufreq_changed[d]) {
				cpufreq_base[d] = base;
				cpufreq_original[d] = rd(base + 0x920);
				cpufreq_changed[d] = true;
			}
			wr(base + 0x920, row);
			__asm__ volatile ("dsb sy" ::: "memory");
			sleep_us(2000);
			state = rd(base + 0x920);
			printf("mi8gpu: cpufreq domain %d: row %u asked for, PERF_STATE now %#x\n", d, row, state);
		}
	}
}

/* MDSS at ae00000, mapped in 64 KB pieces as they are needed: the I/O window of the kernel is 8 MB all told, and a failed
 * allocation in it panics (io_map.c: KMA_NOFAIL), so one big mapping of the whole block is not an option (EXP-0128). */
static vm_offset_t
dpu_va(uint32_t off)
{
	static vm_offset_t piece[0x100000 / 0x10000];
	uint32_t n = off >> 16;

	if (piece[n] == 0) {
		piece[n] = (vm_offset_t)ml_io_map(0x0ae00000ull + (n << 16), 0x10000);
		printf("mi8dpu: MDSS +%#x mapped at %#lx\n", n << 16, (unsigned long)piece[n]);
	}
	return piece[n] + (off & 0xffff);
}

/*
 * boot-arg mi8disp=N (EXP-0128): reads only, nothing is written or kicked. What the boot loaders left of the display:
 * levels: 1 DISP_CC, 2 + map MDSS, 3 + read HW_VERSION, 4 + INTR/CTL/SSPP, 5 + PP config, 6 + PP sampling, 7 + DSI0.
 * N >= 1 the display clock controller (DISP_CC at af00000: MDSS_GDSCR at 3000, the AHB/AXI/MDP/VSYNC branch CBCRs;
 * addresses from EXP-0120, sdm845 dispcc), and only when the power domain and the AHB clock say "on", N >= 2 the DPU
 * itself (MDSS at ae00000; blocks at stock offset + ae00000, EXP-0120 audit-display.py): CTL top, the source pipes,
 * the four ping-pong blocks' tear-check / auto-refresh / counters, DSI0, and 100 ms of the ping-pong frame counter
 * sampled to see whether a 60 Hz tearing-effect signal arrives. CBCR bit 31 = CLK_OFF, GDSCR bit 31 = PWR_ON.
 */
static void
mi8_dpu_probe(uint32_t level)
{
	static const struct { const char *name; uint32_t off; } cc[] = {
		{ "MDSS_GDSCR", 0x3000 }, { "AHB_CBCR", 0x4004 }, { "AXI_CBCR", 0x4008 },
		{ "MDP_CBCR", 0x200c }, { "VSYNC_CBCR", 0x2024 },
	};
	vm_offset_t dcc = (vm_offset_t)ml_io_map(0x0af00000ull, 0x5000);
	uint32_t v[5];

	if (dcc == 0) {
		printf("mi8dpu: no mapping of DISP_CC\n");
		return;
	}
	for (int i = 0; i < 5; i++) {
		v[i] = rd(dcc + cc[i].off);
		printf("mi8dpu: %-11s %#07x = %#010x\n", cc[i].name, cc[i].off, v[i]);
	}
	if (!(v[0] & 0x80000000u) || (v[1] & 0x80000000u)) {
		printf("mi8dpu: the display power domain / AHB clock is not on: not going on\n");
		return;
	}
	if (level < 2) {
		return;
	}
	(void)dpu_va(0);
	if (level < 3) {
		return;
	}
	printf("mi8dpu: reading HW_VERSION\n");
	printf("mi8dpu: MDSS HW_VERSION %#010x\n", rd(dpu_va(0)));
	if (level < 4) {
		return;
	}
	printf("mi8dpu: MDSS HW_INTR_STATUS %#010x\n", rd(dpu_va(0x10)));
	for (int i = 0; i < 5; i++) {
		vm_offset_t b = dpu_va(0x2000 + 0x200 * i);

		printf("mi8dpu: CTL%d TOP %#010x FLUSH %#010x START %#010x\n", i, rd(b + 0x14), rd(b + 0x18), rd(b + 0x1c));
	}
	{
		static const uint32_t sspp[8] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };

		for (int i = 0; i < 8 && level >= 4; i++) {
			vm_offset_t b = dpu_va(sspp[i]);

			printf("mi8dpu: SSPP@%#x SIZE %#010x ADDR0 %#010x YSTRIDE0 %#010x FORMAT %#010x OPMODE %#010x\n", sspp[i],
			    rd(b), rd(b + 0x14), rd(b + 0x24), rd(b + 0x30), rd(b + 0x38));
		}
	}
	{
		static const uint32_t pp[4] = { 0x71000, 0x71800, 0x72000, 0x72800 };
		uint32_t enabled = 0;

		for (int i = 0; i < 4 && level >= 5; i++) {
			vm_offset_t b = dpu_va(pp[i]);
			uint32_t en = rd(b);

			printf("mi8dpu: PP%d TEAR_EN %#x VSYNC_CFG %#010x HEIGHT %#010x INIT %#010x COUNT %#010x LINE %#010x AUTOREF %#010x\n",
			    i, en, rd(b + 4), rd(b + 8), rd(b + 0x10), rd(b + 0x14), rd(b + 0x2c), rd(b + 0x30));
			if (en & 1) {
				enabled |= 1u << i;
			}
		}
		for (int i = 0; i < 4 && level >= 6; i++) {
			vm_offset_t b = dpu_va(pp[i]);
			uint64_t t0, now, last_change = 0, ns;
			uint32_t prev, cur, changes = 0, lmin = ~0u, lmax = 0, reads = 0;

			if (!(enabled & (1u << i))) {
				continue;
			}
			/* INT_COUNT_VAL low 16 bits: the read-pointer line count, counting up on the vsync clock and reloaded
			 * (VSYNC_INIT) by each tearing-effect pulse: a drop = one pulse seen */
			prev = rd(b + 0x14) & 0xffff;
			t0 = mach_absolute_time();
			do {
				uint32_t line = rd(b + 0x2c) & 0xffff;

				cur = rd(b + 0x14) & 0xffff;
				now = mach_absolute_time();
				reads++;
				if (cur < lmin) {
					lmin = cur;
				}
				if (cur > lmax) {
					lmax = cur;
				}
				if (line != 0) {
					changes += 1000;        /* the write pointer moved: noted below */
				}
				if (cur + 100 < prev) {
					absolutetime_to_nanoseconds(now - (last_change ? last_change : t0), &ns);
					if ((changes % 1000) < 12) {
						printf("mi8dpu: PP%d read-pointer line %u -> %u, %u us after the last drop\n", i, prev, cur, (uint32_t)(ns / 1000));
					}
					changes++;
					last_change = now;
				}
				prev = cur;
				absolutetime_to_nanoseconds(now - t0, &ns);
			} while (ns < 100000000ull);
			printf("mi8dpu: PP%d over 100 ms: %u reads, %u drops (write pointer moved: %u), read-pointer line %u..%u\n",
			    i, reads, changes % 1000, changes / 1000, lmin, lmax);
		}
	}
	for (int i = 0; i < 7 && level >= 7; i++) {
		static const uint32_t o[7] = { 0x0, 0x4, 0x8, 0x58, 0x5c, 0x84, 0x14 };

		printf("mi8dpu: DSI0 +%#x = %#010x\n", o[i], rd(dpu_va(0x94000 + o[i])));
	}
}

/*
 * boot-arg mi8flip=N (EXP-0129): the first writes to the display. Two buffers of the panel's size, physically contiguous
 * (the DPU's pipe SSPP0 reads 1080x2248 ARGB with stride 4320 from SRC0_ADDR; EXP-0128 found it reading the boot frame
 * buffer 0x9d400000 under tear-check/auto-refresh), are filled with vertical bars and shown in turn by writing SSPP0's
 * SRC0_ADDR and CTL0's FLUSH (bit 0 = VIG0, dpu_hw_catalog SDM845); the time until the FLUSH bit reads back 0 says when the
 * hardware took the new address. At the end the original address is restored. N rounds; a round = A, B (3 s each).
 */
static void
mi8_dpu_flip(uint32_t rounds)
{
	const uint32_t width = 1080, height = 2248, stride = 4320;
	vm_size_t size = round_page((vm_size_t)stride * height);
	vm_offset_t buf[2] = { 0, 0 };
	vm_offset_t src = dpu_va(0x5000), ctl = dpu_va(0x2000);
	uint32_t orig = rd(src + 0x14), ostride = rd(src + 0x24);

	printf("mi8flip: SSPP0 source %#x stride %#x; buffers of %lu bytes\n", orig, ostride, (unsigned long)size);
	if (ostride != stride || (rd(src) != ((height << 16) | width))) {
		printf("mi8flip: SSPP0 is not what EXP-0128 saw: not going on\n");
		return;
	}
	for (int b = 0; b < 2; b++) {
		static const uint32_t color[2][4] = {
			{ 0xffffffff, 0xffff0000, 0xff00ff00, 0xff0000ff },
			{ 0xff000000, 0xff00ffff, 0xffff00ff, 0xffffff00 },
		};

		if (kmem_alloc_contig(kernel_map, &buf[b], size, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mi8flip: no contiguous memory for buffer %d\n", b);
			goto done;
		}
		for (uint32_t y = 0; y < height; y++) {
			uint32_t *row = (uint32_t *)(buf[b] + (vm_offset_t)y * stride);

			for (uint32_t x = 0; x < width; x++) {
				/* four bars; the bar's colour darkens down the screen so that a wrong stride or offset shows */
				row[x] = (y % 200 < 8) ? 0xff808080u : color[b][(x * 4) / width];
			}
		}
		to_memory(buf[b], size);
		printf("mi8flip: buffer %d at physical %#llx\n", b, kvtophys(buf[b]));
	}
	for (uint32_t r = 0; r < rounds * 2; r++) {
		uint64_t pa = kvtophys(buf[r & 1]), t0, t1, ns = 0;
		uint32_t flush;

		wr(src + 0x14, (uint32_t)pa);
		__asm__ volatile ("dsb sy" ::: "memory");
		t0 = mach_absolute_time();
		wr(ctl + 0x18, 1);
		__asm__ volatile ("dsb sy" ::: "memory");
		do {
			flush = rd(ctl + 0x18);
			t1 = mach_absolute_time();
			absolutetime_to_nanoseconds(t1 - t0, &ns);
		} while ((flush & 1) && ns < 200000000ull);
		printf("mi8flip: showing buffer %lu: SRC0_ADDR now %#x, FLUSH read back %#x after %u us\n",
		    (unsigned long)(r & 1), rd(src + 0x14), flush, (uint32_t)(ns / 1000));
		for (int i = 0; i < 300; i++) {
			sleep_us(10000);
		}
	}
done:
	wr(src + 0x14, orig);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(ctl + 0x18, 1);
	for (int i = 0; i < 100; i++) {
		sleep_us(1000);
	}
	printf("mi8flip: original source %#x restored: SRC0_ADDR %#x, FLUSH %#x\n", orig, rd(src + 0x14), rd(ctl + 0x18));
}

/*
 * boot-arg mi8bl=1 (EXP-0138): the panel's brightness. The EA8074 OLED takes it as DCS command 0x51 with two bytes, high
 * first, 0x002..0x3ff (the phone's device tree: bl_ctrl_dcs, bl-dcs-type-ss, bl-max-level 0x3ff). Sent as Linux's msm DSI
 * host does (dsi_host.c dsi_cmd_dma_add/msm_dsi_host_cmd_xfer_commit): the packet in memory in the controller's own
 * order (word count low, high, data type, flags 0xc0 = last + long), DMA_BASE, DMA_LEN, TRIG_DMA = 1; TRIG_CTRL's DMA
 * trigger is software already (4, EXP-0128). DSI0 is the 6G kind: registers at XML offset + 4 (DSI_6G_REG_SHIFT).
 */
#define DSI0_REG(off)   (0x94000 + 4 + (off))
static vm_offset_t dsi_packet;
static uint32_t mi8bl_hs;          /* mi8bl=2: the commands in high speed */
static bool mi8bl_quiet;           /* the slider's commands: a line only for one that fails */
static uint64_t dsi_packet_pa;

static bool mi8_dsi_send(const uint8_t *packet, uint32_t len, const char *what);

static bool
mi8_dsi_brightness(uint32_t level)
{
	uint8_t pk[8];

	level = level > 0x3ff ? 0x3ff : level;     /* 0 is dark: only mi8_panel_set(false) asks for it */
	pk[0] = 3;              /* word count: 0x51 and two bytes */
	pk[1] = 0;
	pk[2] = 0x39;           /* DCS long write, virtual channel 0 */
	pk[3] = 0xc0;           /* the last packet, a long one */
	pk[4] = 0x51;
	pk[5] = (uint8_t)(level >> 8);
	pk[6] = (uint8_t)level;
	pk[7] = 0xff;
	return mi8_dsi_send(pk, 8, "brightness");
}

/* A DCS command without a parameter (short write, data type 0x05): 0x21 inverts the picture, 0x20 puts it back. */
static bool
mi8_dsi_short(uint8_t command)
{
	uint8_t pk[4] = { command, 0, 0x05, 0x80 };

	return mi8_dsi_send(pk, 4, "short command");
}

static bool mi8_dsi_send(const uint8_t *packet, uint32_t len, const char *what);

/*
 * Read a DCS register of the panel (Linux dsi_host.c: SET_MAXIMUM_RETURN_PACKET_SIZE, then a DCS read with bit 5 of the
 * flags = a turnaround for the answer, in low power), and the four read-back words RDBK_DATA0..3 (XML 0x68..0x74).
 */
static void
mi8_dsi_read(uint8_t command, uint8_t size)
{
	uint8_t max[4] = { size, 0, 0x37, 0x80 }, rd_pk[4] = { command, 0, 0x06, 0xa0 };

	mi8_dsi_send(max, 4, "max return size");
	mi8_dsi_send(rd_pk, 4, "DCS read");
	sleep_us(2000);
	printf("mi8bl: read of DCS %#x: RDBK %#010x %#010x %#010x %#010x, ACK_ERR %#x, INTR_CTRL %#x, STATUS0 %#x\n", command,
	    rd(dpu_va(DSI0_REG(0x68))), rd(dpu_va(DSI0_REG(0x6c))), rd(dpu_va(DSI0_REG(0x70))), rd(dpu_va(DSI0_REG(0x74))),
	    rd(dpu_va(DSI0_REG(0x64))), rd(dpu_va(DSI0_REG(0x10c))), rd(dpu_va(DSI0_REG(0x04))));
}

static bool
mi8_dsi_send(const uint8_t *packet, uint32_t len, const char *what)
{
	uint64_t t0, ns = 0;
	uint32_t status, intr;

	if (dsi_packet == 0) {
		/* DMA_BASE is 32 bits. Ask VM for a low physical page rather than
		 * rejecting an unconstrained allocation after userspace has started. */
		if (kmem_alloc_contig(kernel_map, &dsi_packet, PAGE_SIZE, PAGE_MASK, (ppnum_t)(0xffffffffull >> PAGE_SHIFT), 0,
		    KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			printf("mi8bl: no memory for the command\n");
			return false;
		}
		dsi_packet_pa = kvtophys(dsi_packet);
		if (dsi_packet_pa > 0x100000000ull - PAGE_SIZE) {
			printf("mi8bl: the command's memory is above 4 GB (%#llx): the DSI DMA address has 32 bits\n", dsi_packet_pa);
			kmem_free(kernel_map, dsi_packet, PAGE_SIZE);
			dsi_packet = 0;
			dsi_packet_pa = 0;
			return false;
		}
		printf("mi8bl: command buffer at %#llx, %lu bytes within the 32-bit DMA range\n", dsi_packet_pa, (unsigned long)PAGE_SIZE);
	}
	memcpy((void *)dsi_packet, packet, len);
	to_memory(dsi_packet, 64);
	/* INTR_CTRL: its status bits are cleared by writing 1 (the mask bits written back as they are): the DMA's "done" seen
	 * afterwards is this command's */
	intr = rd(dpu_va(DSI0_REG(0x10c)));
	wr(dpu_va(DSI0_REG(0x10c)), (intr & 0x02220202u) | 1u);
	wr(dpu_va(DSI0_REG(0x44)), (uint32_t)dsi_packet_pa);    /* DMA_BASE */
	wr(dpu_va(DSI0_REG(0x48)), len);                        /* DMA_LEN */
	__asm__ volatile ("dsb sy" ::: "memory");
	t0 = mach_absolute_time();
	wr(dpu_va(DSI0_REG(0x8c)), 1);                          /* TRIG_DMA */
	__asm__ volatile ("dsb sy" ::: "memory");
	do {
		intr = rd(dpu_va(DSI0_REG(0x10c)));
		status = rd(dpu_va(DSI0_REG(0x04)));
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	} while (!(intr & 1) && ns < 50000000ull);
	if (!mi8bl_quiet || !(intr & 1)) {
		printf("mi8bl: %s %02x %02x %02x %02x %02x %02x %02x %02x (%u bytes): DMA done %s after %u us; STATUS0 %#x INTR_CTRL %#x "
		    "ACK_ERR %#x\n", what, packet[0], packet[1], packet[2], packet[3], len > 4 ? packet[4] : 0, len > 5 ? packet[5] : 0,
		    len > 6 ? packet[6] : 0, len > 7 ? packet[7] : 0, len, (intr & 1) ? "yes" : "NO", (uint32_t)(ns / 1000), status, intr,
		    rd(dpu_va(DSI0_REG(0x64))));
	}
	return (intr & 1) != 0;
}

static void mi8_bl_test(void);

/*
 * sysctl kern.mi8_brightness (EXP-0139, 0142): 0 (the screen off while locked) or 2..1023, the panel's brightness, written by our library in backboardd when iOS's
 * brightness system is told a new display brightness (userland/src/inject.c); reads give the last level sent, 0 before
 * any. One command at a time; only while DSI0 is as the boot loaders left it (command mode, software DMA trigger).
 */
static LCK_GRP_DECLARE(mi8bl_lck_grp, "mi8-brightness");
static LCK_MTX_DECLARE(mi8bl_lock, &mi8bl_lck_grp);
static int mi8bl_level;
static bool mi8bl_off;             /* the panel is told DCS 0x28 (display off) and its brightness is 0 */

/* The panel dark (EXP-0142): brightness 0, then DCS 0x28 display off; on again with 0x29 first. Called with mi8bl_lock held. */
static bool
mi8_panel_set(bool on, uint32_t level)
{
	bool ok = true;

	if (on && mi8bl_off) {
		ok = mi8_dsi_short(0x29);
		mi8bl_off = !ok;
	}
	if (on) {
		return ok && mi8_dsi_brightness(level < 2 ? 2 : level);
	}
	ok = mi8_dsi_brightness(0) && mi8_dsi_short(0x28);
	mi8bl_off = ok;
	return ok;
}

/* For the way back to the loader (osfmk/arm64/loader_return.c): the loader's screen must not stay dark. */
void
mi8_panel_restore(void)
{
	if (mi8bl_off) {
		mi8bl_quiet = true;
		lck_mtx_lock(&mi8bl_lock);
		mi8_panel_set(true, 0x155);
		lck_mtx_unlock(&mi8bl_lock);
	}
}

static int
mi8_brightness_sysctl(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int level = mi8bl_level, error = sysctl_handle_int(oidp, &level, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if (level < 0 || level == 1 || level > 0x3ff) {
		return EINVAL;
	}
	lck_mtx_lock(&mi8bl_lock);
	mi8bl_quiet = true;
	if ((rd(dpu_va(DSI0_REG(0))) & 5) != 5 || (rd(dpu_va(DSI0_REG(0x80))) & 7) != 4) {
		error = ENXIO;
	} else if (mi8_panel_set(level != 0, (uint32_t)level)) {
		mi8bl_level = level;
	} else {
		error = EIO;
	}
	lck_mtx_unlock(&mi8bl_lock);
	return error;
}
SYSCTL_PROC(_kern, OID_AUTO, mi8_brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    mi8_brightness_sysctl, "I", "the panel's brightness, 2..1023 (DCS 0x51); 0 = display off (DCS 0x28)");

static void
mi8_bl_thread(__unused void *parameter, __unused wait_result_t result)
{
	delay_for_interval(70, NSEC_PER_SEC);
	printf("mi8bl: 70 s on: the brightness steps now\n");
	mi8_bl_test();
	thread_terminate(current_thread());
}

static void
mi8_bl_test(void)
{
	static const uint32_t levels[] = { 0x080, 0x3ff, 0x020, 0x200, 0x300 };

	printf("mi8bl: DSI0 CTRL %#x STATUS0 %#x CMD_DMA_CTRL %#x CMD_CFG0 %#x CMD_CFG1 %#x DMA_BASE %#x DMA_LEN %#x TRIG_CTRL %#x "
	    "ERR_INT_MASK0 %#x INTR_CTRL %#x\n", rd(dpu_va(DSI0_REG(0))), rd(dpu_va(DSI0_REG(0x04))), rd(dpu_va(DSI0_REG(0x38))),
	    rd(dpu_va(DSI0_REG(0x3c))), rd(dpu_va(DSI0_REG(0x40))), rd(dpu_va(DSI0_REG(0x44))), rd(dpu_va(DSI0_REG(0x48))),
	    rd(dpu_va(DSI0_REG(0x80))), rd(dpu_va(DSI0_REG(0x108))), rd(dpu_va(DSI0_REG(0x10c))));
	if ((rd(dpu_va(DSI0_REG(0))) & 5) != 5 || (rd(dpu_va(DSI0_REG(0x80))) & 7) != 4) {
		printf("mi8bl: DSI0 is not in command mode with a software DMA trigger: nothing sent\n");
		return;
	}
	{
		/* the last command the boot loaders sent: its packet is still where DMA_BASE says (free memory, not ours) */
		uint32_t base = rd(dpu_va(DSI0_REG(0x44))), len = rd(dpu_va(DSI0_REG(0x48)));
		vm_offset_t page = (base != 0 && len != 0 && len <= 256) ? ml_io_map(base & ~0x3fffu, 0x4000) : 0;

		if (page != 0) {
			const uint8_t *b = (const uint8_t *)(page + (base & 0x3fff));
			printf("mi8bl: the boot loaders' last command (%u bytes at %#x): %02x %02x %02x %02x  %02x %02x %02x %02x  "
			    "%02x %02x %02x %02x  %02x %02x %02x %02x\n", len, base, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
			    b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
		}
	}
	if (!mi8bl_hs) {
		wr(dpu_va(DSI0_REG(0x38)), rd(dpu_va(DSI0_REG(0x38))) | (1u << 26));      /* low power, as the boot loaders had it */
	}
	mi8_dsi_read(0x0a, 1);          /* power mode: 0x9c when the panel is awake and showing */
	mi8_dsi_read(0x52, 2);          /* the brightness it has */
	mi8_dsi_brightness(0x155);
	mi8_dsi_read(0x52, 2);          /* ... and after 0x155 was written */
	mi8_dsi_read(0x54, 1);          /* CTRL display (0x53's value: 0x20 = brightness control on) */
	mi8_dsi_read(0xda, 1);          /* ID1 */
	if (mi8bl_hs) {
		/* high speed instead of the boot loaders' low power (CMD_DMA_CTRL bit 26) */
		wr(dpu_va(DSI0_REG(0x38)), rd(dpu_va(DSI0_REG(0x38))) & ~(1u << 26));
		printf("mi8bl: commands in high speed: CMD_DMA_CTRL %#x\n", rd(dpu_va(DSI0_REG(0x38))));
	}
	for (unsigned i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		if (!mi8_dsi_brightness(levels[i])) {
			printf("mi8bl: the command DMA did not finish: stopping\n");
			return;
		}
		for (int j = 0; j < 250; j++) {
			sleep_us(10000);
		}
	}
}

/*
 * boot-arg mi8_ddr=N (EXP-0127): the memory system's bandwidth votes, which nothing here makes (Linux: interconnect
 * drivers, bwmon). The Apps RSC's HLOS DRV (drv-2 of apps_rsc@179c0000, sdm845.dtsi; qcom,tcs-offset 0xd00,
 * TCS 0 and 1 are the active-only ones) is given one request with the BCMs "MC0" (the memory controller) and "SH0"
 * (the LLCC / system cache) voted for bandwidth. BCM command word (bcm-voter): commit << 30 | valid << 29 |
 * vote_x << 14 | vote_y, each 14 bits; N is the share of the biggest vote (0x3fff) in 1/16ths, 16 = all of it.
 * Registers and sequence as rpmh-rsc.c (version 2.7 offsets).
 */
static void
mi8_ddr_vote(uint32_t sixteenths)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db_map();
	static vm_offset_t rsc_va;
	vm_offset_t base, tcs;
	const char *const names[2] = { "MC0", "SH0" };
	uint32_t address[2], vote = (0x3fffu * (sixteenths > 16 ? 16 : sixteenths)) / 16, id;

	if (rsc_va == 0) {
		rsc_va = (vm_offset_t)ml_io_map(0x179e0000ull, 0x10000);
	}
	base = rsc_va;
	tcs = base + 0xd00;
	if (base == 0) {
		printf("mi8gpu: ddr: no mapping of the Apps RSC\n");
		return;
	}
	printf("mi8gpu: ddr: reading the Apps RSC (drv-2 mapped at %#lx)\n", (unsigned long)base);
	id = rd(base + 0);
	printf("mi8gpu: ddr: apps_rsc drv-2 ID %#x, PRNT_CHLD_CONFIG %#x, SOLVER %#x; TCS 0: CONTROL %#x CMD_ENABLE %#x IRQ_STATUS %#x\n", id,
	    rd(base + 0x0c), rd(base + 0x04), rd(tcs + 0x14), rd(tcs + 0x1c), rd(tcs + 0x04));
	if (sixteenths >= 100) {
		return;         /* mi8_ddr=100: only looked */
	}
	for (int i = 0; i < 2; i++) {
		address[i] = rpmh_address(db, names[i]);
		if (address[i] == 0) {
			printf("mi8gpu: ddr: no resource %s\n", names[i]);
			return;
		}
	}
	/* a free TCS 0: nothing enabled in it, no transfer under way */
	if (rd(tcs + 0x1c) != 0 || (rd(tcs + 0x14) & (1u << 24))) {
		printf("mi8gpu: ddr: TCS 0 is in use (CONTROL %#x CMD_ENABLE %#x): not touched\n", rd(tcs + 0x14), rd(tcs + 0x1c));
		return;
	}
	for (int i = 0; i < 2; i++) {
		uint32_t command = (1u << 30) | (1u << 29) | (vote << 14) | vote;

		wr(tcs + 0x30 + 20 * i, (1u << 16) | (1u << 8) | 8u);  /* MSGID: write, response requested, length 8 */
		wr(tcs + 0x34 + 20 * i, address[i]);
		wr(tcs + 0x38 + 20 * i, command);
		printf("mi8gpu: ddr: command %d: %s at %#x <- %#010x\n", i, names[i], address[i], command);
	}
	wr(tcs + 0x1c, 3);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(tcs + 0x14, 1u << 16);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(tcs + 0x14, (1u << 16) | (1u << 24));
	__asm__ volatile ("dsb sy" ::: "memory");
	for (int t = 0; t < 20000; t++) {
		if (rd(tcs + 0x04) & 1u) {
			break;
		}
		sleep_us(10);
	}
	printf("mi8gpu: ddr: IRQ_STATUS %#x, command status %#x %#x, CONTROL %#x\n", rd(tcs + 0x04), rd(tcs + 0x3c), rd(tcs + 0x3c + 20), rd(tcs + 0x14));
	wr(tcs + 0x14, 0);
	wr(tcs + 0x1c, 0);
	wr(tcs + 0x08, 1u);
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* boot-arg mi8_cmddb=1 (EXP-0127): every resource of the command database that is a BCM (address 0x5xxxx) with its aux data. */
static void
mi8_cmddb_dump(void)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)cmd_db_map();

	for (unsigned sl = 0; sl < 8; sl++) {
		const struct cmd_db_slave *slave = &db->slave[sl];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id == 0 || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			char name[9] = { 0 };
			const uint8_t *aux;

			memcpy(name, entry[i].id, 8);
			if ((entry[i].address & 0xff0000) != 0x50000 && sl != 0) {
				continue;
			}
			aux = db->data + slave->data_offset + entry[i].offset;
			if (entry[i].length == 0 || entry[i].length > 64 || slave->data_offset + entry[i].offset + entry[i].length >= CMD_DB_SIZE - 0x1000) {
				printf("mi8gpu: cmddb slave %u: %-8s at %#x (%u bytes of aux: not shown)\n", slave->id, name, entry[i].address, entry[i].length);
				continue;
			}
			printf("mi8gpu: cmddb slave %u: %-8s at %#x, aux %u bytes:", slave->id, name, entry[i].address, entry[i].length);
			for (unsigned b = 0; b < entry[i].length && b < 32; b++) {
				printf(" %02x", aux[b]);
			}
			printf("\n");
		}
	}
}

/* boot-arg mi8_membench=1 (EXP-0127): what the processor gets out of the memory, in MB/s, three times each. */
static void
mi8_membench(void)
{
	const size_t size = 32u << 20;
	vm_offset_t buffer;

	if (kmem_alloc(kernel_map, &buffer, size, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mi8gpu: membench: no 32 MB\n");
		return;
	}
	{
		/* the processor's own clock: a chain of dependent additions is one cycle each */
		uint64_t t0, t1, ns, x = 1;

		t0 = mach_absolute_time();
		for (uint64_t i = 0; i < 100000000ull; i++) {
			__asm__ volatile ("add %0, %0, #1" : "+r" (x));
		}
		t1 = mach_absolute_time();
		absolutetime_to_nanoseconds(t1 - t0, &ns);
		printf("mi8gpu: membench: 100 M dependent additions took %llu us: about %llu MHz (%llu)\n", ns / 1000, 100000ull * 1000 / (ns / 1000 ? ns / 1000 : 1), x & 1);
	}
	for (int round = 0; round < 3; round++) {
		uint64_t t0, t1, t2, t3, ns_w, ns_c, ns_r, sum = 0;
		const uint64_t * volatile in = (const uint64_t *)buffer;

		t0 = mach_absolute_time();
		memset((void *)buffer, round + 1, size);
		t1 = mach_absolute_time();
		memcpy((void *)(buffer + size / 2), (void *)buffer, size / 2);
		t2 = mach_absolute_time();
		for (size_t i = 0; i < size / 8; i += 4) {
			sum += in[i] + in[i + 1] + in[i + 2] + in[i + 3];
		}
		t3 = mach_absolute_time();
		absolutetime_to_nanoseconds(t1 - t0, &ns_w);
		absolutetime_to_nanoseconds(t2 - t1, &ns_c);
		absolutetime_to_nanoseconds(t3 - t2, &ns_r);
		printf("mi8gpu: membench %d: write 32 MB %llu MB/s, copy 16 MB %llu MB/s (read+write), read 32 MB %llu MB/s (%llu)\n", round,
		    (uint64_t)size * 1000 / ns_w, (uint64_t)(size / 2) * 1000 / ns_c, (uint64_t)size * 1000 / ns_r, sum & 0xff);
	}
	kmem_free(kernel_map, buffer, size);
}

static void mi8_cpuclk_thread(void *parameter, wait_result_t result);

/* Called when the steps have brought the 3D core to a state that takes commands (secure mode left, ring running). */
static void
gpu_keep_begin(void)
{
	{
		uint32_t bench = 0, freq = 0xffff;

		uint32_t dump = 0, ddr = 16;    /* the memory bandwidth vote is on unless mi8_ddr=0: 22 GB/s writes instead of 2 (EXP-0127) */

		PE_parse_boot_argn("mi8_ddr", &ddr, sizeof(ddr));
		if (ddr) {
			mi8_ddr_vote(ddr);
		}

		if (PE_parse_boot_argn("mi8_cmddb", &dump, sizeof(dump)) && dump) {
			mi8_cmddb_dump();
		}
		{
			uint32_t big = 0xffff;
			bool have_big = PE_parse_boot_argn("mi8_cpufreq_big", &big, sizeof(big));

			if (!PE_parse_boot_argn("mi8_cpufreq", &freq, sizeof(freq))) {
				freq = 0xffff;
			}
			if (freq != 0xffff || have_big) {
				mi8_cpufreq(freq, have_big ? big : 0xffff);
			} else {
				/* nothing asked for: the big cluster where the firmware leaves it, if a run before moved it (see above) */
				vm_offset_t base = (vm_offset_t)ml_io_map(0x17d45800ull, 0x1400);

				if (base != 0 && rd(base + 0x920) != CPUFREQ_BIG_DEFAULT_ROW) {
					printf("mi8gpu: cpufreq domain 1: PERF_STATE was %u (a run before left it): back to %u\n",
					    rd(base + 0x920), CPUFREQ_BIG_DEFAULT_ROW);
					wr(base + 0x920, CPUFREQ_BIG_DEFAULT_ROW);
					__asm__ volatile ("dsb sy" ::: "memory");
				}
			}
		}
		{
			uint32_t dpu = 0;

			if (PE_parse_boot_argn("mi8disp", &dpu, sizeof(dpu)) && dpu) {
				mi8_dpu_probe(dpu);
			}
			if (PE_parse_boot_argn("mi8flip", &dpu, sizeof(dpu)) && dpu) {
				mi8_dpu_flip(dpu);
			}
			if (PE_parse_boot_argn("mi8bl", &dpu, sizeof(dpu)) && dpu) {
				thread_t thread;

				/* in a thread of its own, once the home screen is up: on a black screen an OLED's brightness shows nothing */
				mi8bl_hs = dpu == 2;
				if (kernel_thread_start(mi8_bl_thread, NULL, &thread) == KERN_SUCCESS) {
					thread_deallocate(thread);
				}
			}
		}
		if (PE_parse_boot_argn("mi8_membench", &bench, sizeof(bench)) && bench) {
			mi8_membench();
		}
		{
			uint32_t period = 0;
			thread_t thread;

			if (PE_parse_boot_argn("mi8_cpuclk", &period, sizeof(period)) && period &&
			    kernel_thread_start(mi8_cpuclk_thread, (void *)(uintptr_t)period, &thread) == KERN_SUCCESS) {
				thread_deallocate(thread);
			}
		}
	}
	keep_free[0].start = KEEP_VA_START;
	keep_free[0].size = KEEP_VA_SIZE;
	keep_free_count = 1;
	{
		uint32_t attr = 0;

		if (PE_parse_boot_argn("mi8gpu_attr", &attr, sizeof(attr))) {
			keep_attr_index = attr & 7;
			printf("mi8gpu: the pages of the system's buffers get memory attribute %u\n", attr & 7);
		}
	}
	keep_ring_at = keep_ring_sent = cp_wptr & (KEEP_RING_WORDS - 1);
	bzero((void *)(g3d_regions[2].memory + KEEP_FENCE_AT), 4);
	to_memory(g3d_regions[2].memory, 0x1000);
	gpu_kept = true;
	printf("mi8gpu: THE GPU STAYS ON for the system to draw with: addresses %#llx..%#llx are the kernel's to give\n", KEEP_VA_START,
	    KEEP_VA_START + KEEP_VA_SIZE - 1);
}

/* a6xx_gmu_gfx_rail_on; the OOB is cleared afterwards as a6xx_gpu.c does at the end of hw_init. */
static bool
gmu_gfx_rail_on(uint32_t gx_idx, uint32_t mx_idx, bool clear)
{
	bool ok;

	gmu_power_look("before gfx_rail_on");
	wr(gmu_base + GMU_WORD(GMU_BOOT_SLUMBER_OPT_W), 0);
	wr(gmu_base + GMU_WORD(GMU_GX_VOTE_IDX_W), gx_idx);
	wr(gmu_base + GMU_WORD(GMU_MX_VOTE_IDX_W), mx_idx);
	printf("mi8gpu: GX_VOTE_IDX %#x MX_VOTE_IDX %#x BOOT_SLUMBER_OPTION %#x\n", rd(gmu_base + GMU_WORD(GMU_GX_VOTE_IDX_W)),
	    rd(gmu_base + GMU_WORD(GMU_MX_VOTE_IDX_W)), rd(gmu_base + GMU_WORD(GMU_BOOT_SLUMBER_OPT_W)));
	ok = gmu_oob_set(OOB_BOOT_SLUMBER_SET, OOB_BOOT_SLUMBER_ACK);
	gmu_power_look(ok ? "gfx_rail_on acknowledged" : "gfx_rail_on TIMED OUT");
	if (ok) {
		gmu_sptprac_enable();
		gmu_power_look("after sptprac enable");
		if (clear) {
			gmu_oob_clear(OOB_BOOT_SLUMBER_ACK);
		}
	}
	return ok;
}

/* a6xx_gmu_notify_slumber (legacy): ask the GMU to put the GX domain to sleep and check that it did. */
static bool
gmu_slumber(void)
{
	bool ok;

	wr(gmu_base + GMU_WORD(GMU_POWER_COUNTER_EN_W), 0);
	gmu_sptprac_disable();
	wr(gmu_base + GMU_WORD(GMU_BOOT_SLUMBER_OPT_W), 1);
	ok = gmu_oob_set(OOB_BOOT_SLUMBER_SET, OOB_BOOT_SLUMBER_ACK);
	gmu_oob_clear(OOB_BOOT_SLUMBER_ACK);
	sleep_us(1000);
	gmu_power_look("after slumber request");
	if (ok && rd(gmu_base + GMU_WORD(GMU_RPMH_POWER_STATE_W)) != 0x0f) {
		printf("mi8gpu: the GMU did not go into slumber (RPMH_POWER_STATE != 0xf)\n");
		ok = false;
	}
	if (ok && (rd(gpucc + GPUCC_GX_GDSCR) & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) != GDSCR_SW_COLLAPSE) {
		printf("mi8gpu: GX power switch is not off after slumber\n");
		ok = false;
	}
	return ok;
}

static bool
gmu_run(bool rail, bool hfi)
{
	vm_offset_t rscc = gmu_base + RSCC_OFFSET, cb = smmu_cb(GMU_CTX);
	struct gmu_region *log = &gmu_regions[2];
	uint32_t tcm, mask, want, result;
	bool started, halted, rsc_ok, rail_ok = true;
	uint32_t gx_idx = 0, mx_idx = 0, level = 64;

	if (gmu_boot_written != GMU_BOOT_REGS || !rs_saved || rd(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W)) != 1 ||
	    (rd(gpucc + GPUCC_GX_GDSCR) & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) != GDSCR_SW_COLLAPSE ||
	    rd(rscc + 4 * RSCC_SEQ_BUSY_DRV0_W) != 0 || log->memory == 0 || !smmu_old.saved) {
		printf("mi8gpu: GMU start prerequisites not met (boot config, RSC/PDC tables, SMMU, reset held, GX off, RSC idle)\n");
		return false;
	}
	if (rail) {
		PE_parse_boot_argn("mi8gpu_level", &level, sizeof(level));
		if (!gmu_gx_votes(level, &gx_idx, &mx_idx)) {
			return false;
		}
	}
	/* a6xx_gmu_start(): the word 0xff8 after DTCM_START (a dword index, the last 32 bytes of the 16 KB) selects the check */
	tcm = rd(gmu_base + GMU_DTCM + 4 * 0xff8);
	if (tcm <= 0x20010004) {
		mask = 0xffffffff;
		want = 0xbabeface;
	} else {
		mask = 0x1ff;
		want = 0x100;
	}
	printf("mi8gpu: DTCM+0xff8 words = %#x: FW_INIT_RESULT & %#x must become %#x\n", tcm, mask, want);

	gmu_old_ao = rd(gmu_base + GMU_WORD(GMU_AO_HOST_MASK_W));
	gmu_old_h2h = rd(gmu_base + GMU_WORD(GMU_GMU2HOST_MASK_W));
	wr(gmu_base + GMU_WORD(GMU_AO_HOST_MASK_W), ~0u);
	wr(gmu_base + GMU_WORD(GMU_GMU2HOST_MASK_W), ~0u);
	printf("mi8gpu: GMU interrupts masked (were AO %#x, GMU2HOST %#x; now %#x/%#x)\n", gmu_old_ao, gmu_old_h2h,
	    rd(gmu_base + GMU_WORD(GMU_AO_HOST_MASK_W)), rd(gmu_base + GMU_WORD(GMU_GMU2HOST_MASK_W)));

	bzero((void *)log->memory, log->size);
	to_memory(log->memory, log->size);
	wr(gmu_base + GMU_WORD(GMU_PWR_COL_CP_RESP_W), 0);
	wr(gmu_base + GMU_WORD(GMU_CM3_INIT_RESULT_W), 0);
	__asm__ volatile ("dsb sy" ::: "memory");

	printf("mi8gpu: RELEASING the GMU reset now (SYSRESET 1 -> 0); the processor runs from here until it is held again\n");
	gmu_unsafe = true;
	wr(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W), 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	started = false;
	for (uint32_t i = 0; i < 200; i++) {
		if (((result = rd(gmu_base + GMU_WORD(GMU_CM3_INIT_RESULT_W))) & mask) == want) {
			started = true;
			break;
		}
		sleep_us(100);
	}
	printf("mi8gpu: GMU firmware init %s: FW_INIT_RESULT %#010x CP_RESP %#010x GENERAL_7 %#x SYSRESET %#x\n",
	    started ? "DONE" : "TIMED OUT (20 ms)", rd(gmu_base + GMU_WORD(GMU_CM3_INIT_RESULT_W)),
	    rd(gmu_base + GMU_WORD(GMU_PWR_COL_CP_RESP_W)), rd(gmu_base + GMU_WORD(GMU_GENERAL_7_W)),
	    rd(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W)));
	printf("mi8gpu: while running: RSC_STATUS0_DRV0 %#010x SEQ_BUSY %#010x; SMMU context %u FSR %#010x FSYNR0 %#010x; fence status %#x\n",
	    rd(rscc + 4 * RSCC_STATUS0_DRV0_W), rd(rscc + 4 * RSCC_SEQ_BUSY_DRV0_W), GMU_CTX, rd(cb + CB_FSR),
	    rd(cb + CB_FSYNR0), rd(gmu_base + GMU_WORD(GMU_FENCE_STATUS_W)));

	if (rail && started) {
		rail_ok = gmu_gfx_rail_on(gx_idx, mx_idx, !hfi);
		if (rail_ok && hfi) {
			bool hfi_ok = gmu_hfi_start();

			printf("mi8gpu: HFI start %s\n", hfi_ok ? "COMPLETE (INIT, FW_VERSION, PERF_TABLE, BW_TABLE, TEST acknowledged)" : "FAILED");
			gmu_power_look("after HFI");
			rail_ok = hfi_ok;
			if (hfi_ok) {
				/* a6xx_hw_init(): set_oob(GPU_SET) first; at its end clear GPU_SET, then clear BOOT_SLUMBER
				 * ("take the GMU out of its special boot mode"). Nothing touches the GPU in between here. */
				bool gpu_set = gmu_oob_set(OOB_GPU_SET, OOB_GPU_SET_ACK);

				gmu_power_look(gpu_set ? "GPU_SET acknowledged" : "GPU_SET NOT acknowledged");
				if (gpu_set && gmu_last_step >= 18) {
					gpu_look();
				}
				if (gpu_set && gmu_last_step >= 19) {
					gpu_init();
				}
				if (gpu_set && gmu_last_step >= 20) {
					gpu_cp_start();
				}
				if (gpu_set && gmu_last_step >= 21) {
					gpu_zap_and_command();
				}
				gmu_oob_clear(OOB_GPU_SET_ACK);
				gmu_oob_clear(OOB_BOOT_SLUMBER_ACK);
				sleep_us(1000);
				gmu_power_look("after GPU_SET and BOOT_SLUMBER cleared");
				rail_ok = gpu_set;
				if (mi8_keep && gpu_set && cp_ok && gpu_nonsecure) {
					/* nothing is stopped: the system draws with this GPU from here (gmu_unsafe stays set so that no
					 * teardown frees what the core uses) */
					uint32_t perf = 7;      /* the highest of the table; mi8gpu_perf=N another, 0 none (EXP-0116: 17 times the fill rate of the boot level) */

					PE_parse_boot_argn("mi8gpu_perf", &perf, sizeof(perf));
					if (perf != 0) {
						gmu_set_freq(perf);
					}
					gpu_keep_begin();
					return true;
				}
			} else {
				gmu_oob_clear(OOB_BOOT_SLUMBER_ACK);
			}
		}
		if (rail_ok || hfi) {
			bool slumber_ok = gmu_slumber();

			rail_ok = rail_ok && slumber_ok;
		}
	}
	if (rail && !rail_ok) {
		printf("mi8gpu: fault look: AO_HOST_INTERRUPT_STATUS %#x AHB_FENCE_STATUS %#x CM3_FW_BUSY %#x INIT_RESULT %#x CP_RESP %#x CP_MSG %#x\n",
		    rd(gmu_base + GMU_WORD(0x9305)), rd(gmu_base + GMU_WORD(GMU_FENCE_STATUS_W)), rd(gmu_base + GMU_WORD(0x501a)),
		    rd(gmu_base + GMU_WORD(GMU_CM3_INIT_RESULT_W)), rd(gmu_base + GMU_WORD(GMU_PWR_COL_CP_RESP_W)),
		    rd(gmu_base + GMU_WORD(0x5100)));
		for (uint32_t i = 0; i < 16; i += 4) {
			printf("mi8gpu: DTCM tail[%u] = %#010x %#010x %#010x %#010x\n", i, rd(gmu_base + GMU_DTCM + 4 * (0xfe0 + i)),
			    rd(gmu_base + GMU_DTCM + 4 * (0xfe0 + i + 1)), rd(gmu_base + GMU_DTCM + 4 * (0xfe0 + i + 2)),
			    rd(gmu_base + GMU_DTCM + 4 * (0xfe0 + i + 3)));
		}
	}
	/* Stop at once, in a6xx_gmu_force_off()'s order (no HFI, no SPTPRAC, no GX were started). */
	gmu_rpmh_idle_look(rscc);
	wr(gmu_base + GMU_WORD(GMU_FENCE_CLR_W), 7);
	wr(gmu_base + GMU_WORD(GMU_AO_FENCE_CTRL_W), 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W), 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	halted = rd(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W)) == 1;
	printf("mi8gpu: GMU processor %s (SYSRESET %#x)\n", halted ? "HELD in reset again" : "NOT HALTED",
	    rd(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W)));
	if (!halted) {
		return false;
	}
	rsc_ok = gmu_rsc_sleep_wake(rscc);

	/* Whatever the firmware wrote into the log region is in DRAM, not in our cache. */
	to_memory(log->memory, log->size);
	for (uint32_t i = 0; i < 8; i++) {
		printf("mi8gpu: log[%u] = %#010x %#010x %#010x %#010x\n", i, ((uint32_t *)log->memory)[4 * i],
		    ((uint32_t *)log->memory)[4 * i + 1], ((uint32_t *)log->memory)[4 * i + 2], ((uint32_t *)log->memory)[4 * i + 3]);
	}
	printf("mi8gpu: after stop: SMMU context %u FSR %#010x; RSC_STATUS0_DRV0 %#010x SEQ_BUSY %#010x; GX %#010x\n", GMU_CTX,
	    rd(cb + CB_FSR), rd(rscc + 4 * RSCC_STATUS0_DRV0_W), rd(rscc + 4 * RSCC_SEQ_BUSY_DRV0_W), rd(gpucc + GPUCC_GX_GDSCR));

	wr(gmu_base + GMU_WORD(GMU_AO_HOST_MASK_W), gmu_old_ao);
	wr(gmu_base + GMU_WORD(GMU_GMU2HOST_MASK_W), gmu_old_h2h);
	if (rail && !rail_ok &&
	    (rsc_ok && (rd(gpucc + GPUCC_GX_GDSCR) & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) == GDSCR_SW_COLLAPSE)) {
		/* The slumber request faulted the GMU, but GX is verified off, the processor is held and the RSC is awake. */
		printf("mi8gpu: slumber failed, but GX is off, CM3 held and RSC awake: the restoration may proceed\n");
		gmu_unsafe = false;
	} else if (rail && !rail_ok) {
		printf("mi8gpu: the GX domain may still be powered; resources are retained\n");
	} else if (rsc_ok) {
		gmu_unsafe = false;
	} else {
		printf("mi8gpu: RSC sleep/wake did not complete; resources are retained\n");
	}
	return started && rsc_ok && rail_ok;
}

static bool
step_gmu_start(void)
{
	if (gmu_last_step >= 16) {
		printf("mi8gpu: step 15 is carried out inside a later step (one start per run; a second start would be a warm boot)\n");
		return true;
	}
	return gmu_run(false, false);
}

static bool
step_gmu_rail(void)
{
	if (gmu_last_step >= 17) {
		printf("mi8gpu: step 16 is carried out inside a later step\n");
		return true;
	}
	return gmu_run(true, false);
}

static bool
step_gmu_hfi(void)
{
	if (gmu_last_step >= 18) {
		printf("mi8gpu: step 17 is carried out inside a later step\n");
		return true;
	}
	return gmu_run(true, true);
}

static bool
step_gpu_look(void)
{
	if (gmu_last_step >= 19) {
		printf("mi8gpu: step 18 is carried out inside step 19\n");
		return true;
	}
	return gmu_run(true, true);
}

static bool
step_gpu_init(void)
{
	if (gmu_last_step >= 20) {
		printf("mi8gpu: step 19 is carried out inside step 20\n");
		return true;
	}
	return gmu_run(true, true);
}

static bool
step_gpu_cp(void)
{
	if (gmu_last_step >= 21) {
		printf("mi8gpu: step 20 is carried out inside step 21\n");
		return true;
	}
	return gmu_run(true, true);
}

static bool
step_gpu_zap(void)
{
	if (gmu_last_step >= 22) {
		printf("mi8gpu: step 21 is carried out inside step 22\n");
		return true;
	}
	return gmu_run(true, true);
}

static bool
step_gpu_paint(void)
{
	return gmu_run(true, true);
}

/* The way back, in reverse: what the steps switched on is switched off. */
static void
teardown(void)
{
	if (gmu_unsafe) {
		printf("mi8gpu: the GMU or RSC may not be stopped; retaining clocks, CX, tables and SMMU memory\n");
		return;
	}
	if (!gmu_boot_restore()) {
		printf("mi8gpu: boot config restoration failed; retaining clocks, CX and SMMU memory\n");
		return;
	}
	if (!gmu_clock_restore()) {
		printf("mi8gpu: clock restoration failed; retaining clocks, CX and SMMU memory\n");
		return;
	}
	if (!rsc_pdc_restore()) {
		printf("mi8gpu: RSC/PDC restoration failed; retaining clocks, CX and SMMU memory\n");
		return;
	}
	g3d_smmu_restore();
	gmu_smmu_restore();
	if (gmu_clk_on) {
		branch_disable(gpucc, GPUCC_CX_GMU_CBCR, "gpu_cc_cx_gmu_clk");
		gmu_clk_on = false;
	}
	if (cxo_on) {
		branch_disable(gpucc, GPUCC_CXO_CBCR, "gpu_cc_cxo_clk");
		cxo_on = false;
	}
	if (rcg_on) {
		wr(gpucc + GPUCC_GMU_RCG_CMD, rd(gpucc + GPUCC_GMU_RCG_CMD) & ~RCG_CMD_ROOT_EN);
		sleep_us(10);
		printf("mi8gpu: GMU clock source CMD %#010x (root released)\n", rd(gpucc + GPUCC_GMU_RCG_CMD));
		rcg_on = false;
	}
	if (gcc_snoc_on) {
		branch_disable(gcc, GCC_GPU_SNOC_DVM_GFX, "gcc_gpu_snoc_dvm_gfx_clk");
		gcc_snoc_on = false;
	}
	if (gcc_memnoc_on) {
		branch_disable(gcc, GCC_GPU_MEMNOC_GFX, "gcc_gpu_memnoc_gfx_clk");
		gcc_memnoc_on = false;
	}
	if (cx_on) {
		wr(gpucc + GPUCC_CX_GDSCR, rd(gpucc + GPUCC_CX_GDSCR) | GDSCR_SW_COLLAPSE);
		sleep_us(500);
		printf("mi8gpu: CX switched off again: GDSCR %#010x, hardware control %#010x (%s)\n", rd(gpucc + GPUCC_CX_GDSCR),
		    rd(gpucc + GPUCC_CX_GDSC_HWCTRL), cx_wait(false) ? "off" : "STILL ON");
		cx_on = false;
	}
}

/*
 * What an earlier run may have left on. The GPU block is not reset when the loader takes the machine back, and a run
 * that did not end in order (EXP-0083: a thread that never came back from a read) never switched its clocks and its
 * CX switch off again: the next one found the CX switch on at step 4. So every run starts by putting the block back to
 * what the phone's own start leaves (EXP-0077): the branches of this file off, the GMU's clock source released, the CX
 * switch collapsed. Only what step 3 can read safely is looked at: with the register bus off, nothing is touched.
 */
static void
stale_stop(void)
{
	vm_offset_t g = ml_io_map(GCC_PHYS, GCC_SIZE), cc;
	uint32_t gdscr;

	if (rd(g + GCC_GPU_CFG_AHB) & CBCR_OFF) {
		return;
	}
	cc = ml_io_map(GPUCC_PHYS, GPUCC_SIZE);
	gdscr = rd(cc + 0x106c);
	if (!(gdscr & GDSC_PWR_ON)) {
		return;
	}
	printf("mi8gpu: an earlier run left the CX switch on (GDSCR %#010x, hw control %#010x, cx_gmu_clk %#010x): switching it off\n",
	    gdscr, rd(cc + 0x1540), rd(cc + 0x1098));
	wr(cc + 0x1098, rd(cc + 0x1098) & ~1u);
	wr(cc + 0x109c, rd(cc + 0x109c) & ~1u);
	wr(cc + 0x1120, rd(cc + 0x1120) & ~2u);
	wr(g + 0x71018, rd(g + 0x71018) & ~1u);
	wr(g + 0x7100c, rd(g + 0x7100c) & ~1u);
	sleep_us(100);
	wr(cc + 0x106c, rd(cc + 0x106c) | 1u);
	sleep_us(500);
	printf("mi8gpu: after it: CX GDSCR %#010x, hw control %#010x, cx_gmu_clk %#010x, memnoc %#010x\n", rd(cc + 0x106c),
	    rd(cc + 0x1540), rd(cc + 0x1098), rd(g + 0x7100c));
}


/* ---- zap shader authentication probe (boot-arg mi8gpu_zap=1; independent of the GMU) -------------------------- */

/*
 * Linux adreno_gpu.c zap_shader_load_mdt() -> qcom_mdt_load() -> qcom_scm_pas_*():
 *   PAS_INIT_IMAGE(13, metadata = the .mdt: ELF header + program headers + hash segment, physically contiguous, 4 KB
 *                  aligned; TrustZone protects it while it looks at it)
 *   PAS_MEM_SETUP(13, region, size of the loadable segments)
 *   the loadable segment(s) (a630_zap.b02) copied into the reserved region (phone's gpu_region@0x8c415000, 8 KB)
 *   PAS_AUTH_AND_RESET(13)
 * Here we add PAS_SHUTDOWN(13) at the end so nothing stays authenticated. SCM ids: svc 2 cmd 1/2/5/6; arginfo
 * QCOM_SCM_ARGS(2, VAL, RW) = 0x82, ARGS(3) = 3, ARGS(1) = 1; standard (not atomic) SMC64, owner SIP (as EXP-0093).
 */
#define ZAP_PAS_ID              13u
#define ZAP_REGION_PHYS         0x8c415000ul
#define ZAP_REGION_SIZE         0x2000ul

static int64_t
mi8_scm_call(uint64_t function, uint64_t arginfo, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t res[3])
{
	uint64_t session = 0;

	for (unsigned i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i ? 1 : function;
		register uint64_t x1 __asm__("x1") = arginfo;
		register uint64_t x2 __asm__("x2") = a0;
		register uint64_t x3 __asm__("x3") = a1;
		register uint64_t x4 __asm__("x4") = a2;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;
		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4),
		    "+r" (x5), "+r" (x6), "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		    "x16", "x17", "memory", "cc");
		session = x6;
		if (x0 == 1) {
			/* A630_SMC_GAP: the call was preempted; the loader's core is using the same
			 * channel, so wait a little instead of calling again at once. */
			sleep_us(2000);
		}
		if (x0 != 1) {
			res[0] = x1;
			res[1] = x2;
			res[2] = x3;
			return (int32_t)x0;
		}
	}
	return -1000;
}

static bool
zap_load(bool keep)
{
	const uint64_t how = MI8_SCM_64 | MI8_SCM_SIP;
	uint64_t mdt_size = 0, seg_size = 0, res[3], metadata_phys = 0;
	const uint8_t *mdt = redwood_wifi_file("a630_zap.mdt", &mdt_size);
	const uint8_t *seg = redwood_wifi_file("a630_zap.b02", &seg_size);
	vm_offset_t meta = 0, region;
	int64_t rc;
	bool shutdown_needed = false;

	zap_authenticated = false;

	printf("mi8gpu: zap probe: this talks to TrustZone with memory (PAS id %u)\n", ZAP_PAS_ID);
	if (mdt == NULL || seg == NULL || mdt_size != 6684 || seg_size != 0x7b0) {
		printf("mi8gpu: zap files missing or not the expected size (mdt %llu, b02 %llu)\n", mdt_size, seg_size);
		return false;
	}
	rc = mi8_scm_call(how | MI8_SCM_SUPPORTED, 1, ZAP_PAS_ID, 0, 0, res);
	printf("mi8gpu: PAS_SUPPORTED(13): rc %lld result %#llx\n", rc, res[0]);

	/* Physically contiguous and page aligned, as dma_alloc_coherent gives Linux's; it is never given back. */
	if (kmem_alloc_contig(kernel_map, &meta, round_page(mdt_size), PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mi8gpu: no physically contiguous metadata buffer\n");
		return false;
	}
	metadata_phys = kvtophys(meta);
	if ((metadata_phys & 0xfff) != 0 || kvtophys(meta + round_page(mdt_size) - 1) != metadata_phys + round_page(mdt_size) - 1) {
		printf("mi8gpu: metadata buffer %#llx is not 4 KB aligned/contiguous (page size %lu)\n", metadata_phys, (unsigned long)PAGE_SIZE);
		return false;
	}
	memcpy((void *)meta, mdt, mdt_size);
	to_memory(meta, round_page(mdt_size));
	region = ml_io_map(ZAP_REGION_PHYS, ZAP_REGION_SIZE);

	printf("mi8gpu: PAS_INIT_IMAGE(13): metadata %llu bytes at %#llx (buffer is never touched or freed after this)\n",
	    mdt_size, metadata_phys);
	rc = mi8_scm_call(how | 0x201, 0x82, ZAP_PAS_ID, metadata_phys, 0, res);
	printf("mi8gpu: PAS_INIT_IMAGE: rc %lld result %#llx %#llx %#llx\n", rc, res[0], res[1], res[2]);
	if (rc != 0 || res[0] != 0) {
		return false;
	}
	shutdown_needed = true;
	printf("mi8gpu: PAS_MEM_SETUP(13, %#lx, %#x)\n", ZAP_REGION_PHYS, 0x1000);
	rc = mi8_scm_call(how | 0x202, 3, ZAP_PAS_ID, ZAP_REGION_PHYS, 0x1000, res);
	printf("mi8gpu: PAS_MEM_SETUP: rc %lld result %#llx\n", rc, res[0]);
	if (rc != 0 || res[0] != 0) {
		goto out;
	}
	for (uint32_t i = 0; i < 0x7b0 / 4; i++) {
		wr(region + 4 * i, ((const uint32_t *)seg)[i]);
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	uint32_t wrong = 0;
	for (uint32_t i = 0; i < 0x7b0 / 4; i++) {
		wrong += rd(region + 4 * i) != ((const uint32_t *)seg)[i];
	}
	printf("mi8gpu: zap segment (%#x bytes) written to the reserved region; %u words read back wrong\n", 0x7b0, wrong);
	if (wrong) {
		goto out;
	}
	printf("mi8gpu: PAS_AUTH_AND_RESET(13)\n");
	rc = mi8_scm_call(how | 0x205, 1, ZAP_PAS_ID, 0, 0, res);
	printf("mi8gpu: PAS_AUTH_AND_RESET: rc %lld result %#llx %#llx %#llx -- %s\n", rc, res[0], res[1], res[2],
	    (rc == 0 && res[0] == 0) ? "ZAP AUTHENTICATED" : "authentication refused");
	zap_authenticated = rc == 0 && res[0] == 0;
out:
	if (shutdown_needed && !(keep && zap_authenticated)) {
		rc = mi8_scm_call(how | 0x206, 1, ZAP_PAS_ID, 0, 0, res);
		printf("mi8gpu: PAS_SHUTDOWN(13): rc %lld result %#llx\n", rc, res[0]);
		zap_authenticated = false;
	}
	return zap_authenticated;
}

static void
zap_release(void)
{
	uint64_t res[3];
	uint32_t keep_zap = 0;

	if (zap_authenticated && PE_parse_boot_argn("mi8gpu_zap_keep", &keep_zap, sizeof(keep_zap)) && keep_zap) {
		printf("mi8gpu: the zap shader stays authenticated (mi8gpu_zap_keep): Linux does not shut it down at a slumber either\n");
		zap_authenticated = false;
		return;
	}
	if (zap_authenticated) {
		int64_t rc = mi8_scm_call(MI8_SCM_64 | MI8_SCM_SIP | 0x206, 1, ZAP_PAS_ID, 0, 0, res);

		printf("mi8gpu: PAS_SHUTDOWN(13): rc %lld result %#llx\n", rc, res[0]);
		zap_authenticated = false;
	}
}

static bool (*const steps[])(void) = { step_gcc, step_rpmh, step_gpucc, step_cx_on, step_clocks, step_gmu_registers, step_smmu_global, step_smmu_contexts, step_gmu_tcm_test, step_gmu_firmware, step_rsc_pdc, step_gmu_smmu, step_trustzone, step_gmu_prepare, step_gmu_start, step_gmu_rail, step_gmu_hfi, step_gpu_look, step_gpu_init, step_gpu_cp, step_gpu_zap, step_gpu_paint };
#define STEPS ((uint32_t)(sizeof(steps) / sizeof(steps[0])))

static void
mi8_adreno_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t last = (uint32_t)(uintptr_t)parameter;

	gmu_last_step = last;

	{
		uint32_t delay_s = 0;

		if (PE_parse_boot_argn("mi8gpu_delay", &delay_s, sizeof(delay_s)) && delay_s) {
			printf("mi8gpu: waiting %u s for the system to come up\n", delay_s);
			for (uint32_t i = 0; i < delay_s; i++) {
				sleep_us(1000000);
			}
		}
	}
	stale_stop();
	{
		uint32_t zap = 0;

		if (PE_parse_boot_argn("mi8gpu_zap", &zap, sizeof(zap)) && zap) {
			(void)zap_load(false);
		}
	}
	for (uint32_t step = 1; step <= last && step <= STEPS; step++) {
		printf("mi8gpu: step %u\n", step);
		if (!steps[step - 1]()) {
			printf("mi8gpu: step %u: stopped; the steps after it are not attempted\n", step);
			break;
		}
		printf("mi8gpu: step %u: done\n", step);
	}
	teardown();
	zap_release();
	printf("mi8gpu: over (mi8gpu=%u)\n", last);
	thread_terminate(current_thread());
}


/* What Mesa will do through the service, done here with the same calls: an address, pages mapped at it, a command buffer
 * the core runs from there (CP_MEM_WRITE into the same buffer), a fence waited for, the address given back. */
static void
mi8_gpu_selftest(void)
{
	vm_offset_t page = 0;
	uint64_t va, t0, t1;
	volatile uint32_t *data;
	uint32_t *ib, n, fence = 0, good = 0, bad = 0;

	if (kmem_alloc_contig(kernel_map, &page, PAGE_SIZE, PAGE_MASK, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		printf("mi8gpu: selftest: no memory\n");
		return;
	}
	va = mi8_gpu_address(PAGE_SIZE);
	printf("mi8gpu: selftest: address %#llx for %lu bytes at physical %#llx\n", va, (unsigned long)PAGE_SIZE, kvtophys(page));
	for (uint64_t off = 0; off < PAGE_SIZE; off += 0x1000) {
		if (!mi8_gpu_map(va + off, kvtophys(page) + off, false)) {
			printf("mi8gpu: selftest: map failed\n");
			return;
		}
	}
	mi8_gpu_mapped();
	ib = (uint32_t *)page;
	data = (volatile uint32_t *)(page + 0x800);
	mi8_gpu_to_memory((void *)page, PAGE_SIZE);    /* the page was just zeroed: its dirty lines must not be written back over the core's data later */
	t0 = mach_absolute_time();
	for (uint32_t i = 1; i <= 200; i++) {
		n = 0;
		ib[n++] = pkt7(CP_MEM_WRITE_OPCODE, 4);
		ib[n++] = (uint32_t)(va + 0x800);
		ib[n++] = (uint32_t)((va + 0x800) >> 32);
		ib[n++] = 0xabcd0000u | i;
		ib[n++] = i * 3;
		mi8_gpu_to_memory(ib, 64);
		fence = mi8_gpu_submit(&va, &n, 1);
		if (fence == 0) {
			printf("mi8gpu: selftest: ring full at %u\n", i);
			break;
		}
		for (int w = 0; w < 5000 && !mi8_gpu_done(fence); w++) {
			sleep_us(20);
		}
		mi8_gpu_to_memory((void *)data, 8);
		if (data[0] == (0xabcd0000u | i) && data[1] == i * 3) {
			good++;
		} else {
			bad++;
			if (bad <= 3) {
				printf("mi8gpu: selftest: #%u fence %u: memory has %#x %#x, wanted %#x %#x\n", i, fence, data[0], data[1], 0xabcd0000u | i, i * 3);
				mi8_gpu_report("selftest");
			}
		}
	}
	t1 = mach_absolute_time();
	printf("mi8gpu: selftest: %u submissions through the ring, %u results right, %u wrong; %llu timer ticks; fence %u, last %u\n", good + bad, good, bad,
	    t1 - t0, fence, mi8_gpu_last());
	mi8_gpu_report("after the selftest");
	mi8_gpu_unmap(va, PAGE_SIZE);
	mi8_gpu_address_free(va, PAGE_SIZE);
}


/* The run is ending in order (redwood_adreno_stop() -> RedwoodGPUStop() -> redwood_gpu_stop()): what the core was given it
 * finishes, then the GMU is told to slumber (GX off), its processor held, the RSC put to sleep and woken, and the steps'
 * own restoration run -- so that the next kernel finds the GPU block as the phone's start leaves it (EXP-0104..0111). */
void
mi8_gpu_stop(void)
{
	vm_offset_t rscc = gmu_base + RSCC_OFFSET;
	uint32_t waited = 0;
	bool slumber_ok, halted, rsc_ok;

	if (!gpu_kept) {
		return;
	}
	gpu_kept = false;
	mi8_cpufreq_restore();
	while (!mi8_gpu_done(keep_fence) && waited < 5000) {
		sleep_us(100);
		waited++;
	}
	printf("mi8gpu: in all %u submissions, %u command buffers, %llu words were given to the core\n", keep_submissions, keep_ibs,
	    keep_words);
	printf("mi8gpu: the run is about to end: the GPU is given back (%s after %u us)\n",
	    mi8_gpu_done(keep_fence) ? "it had finished what it was given" : "STILL AT WORK", 100 * waited);
	slumber_ok = gmu_slumber();
	gmu_rpmh_idle_look(rscc);
	wr(gmu_base + GMU_WORD(GMU_FENCE_CLR_W), 7);
	wr(gmu_base + GMU_WORD(GMU_AO_FENCE_CTRL_W), 0);
	__asm__ volatile ("dsb sy" ::: "memory");
	wr(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W), 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	halted = rd(gmu_base + GMU_WORD(GMU_CM3_SYSRESET_W)) == 1;
	printf("mi8gpu: GMU processor %s\n", halted ? "HELD in reset again" : "NOT HALTED");
	if (!halted) {
		return;
	}
	rsc_ok = gmu_rsc_sleep_wake(rscc);
	wr(gmu_base + GMU_WORD(GMU_AO_HOST_MASK_W), gmu_old_ao);
	wr(gmu_base + GMU_WORD(GMU_GMU2HOST_MASK_W), gmu_old_h2h);
	if (rsc_ok && (rd(gpucc + GPUCC_GX_GDSCR) & (GDSC_PWR_ON | GDSCR_SW_COLLAPSE)) == GDSCR_SW_COLLAPSE) {
		printf("mi8gpu: slumber %s; GX is off, CM3 held, RSC awake: the restoration runs\n", slumber_ok ? "answered" : "was not answered");
		gmu_unsafe = false;
		teardown();
		zap_release();
		mi8_keep = false;
	} else {
		printf("mi8gpu: the GPU is NOT back in its starting state (GX %#x, RSC %s): resources are retained\n", rd(gpucc + GPUCC_GX_GDSCR),
		    rsc_ok ? "awake" : "not awake");
	}
}

extern void RedwoodGPUStart(void);

/* Boot-arg gpu=1 on this phone: the steps to the 3D core's first commands, then nothing is stopped and the service starts. */
static void
mi8_gpu_start_by_itself(void)
{
	mi8_keep = true;
	gmu_last_step = 21;
	stale_stop();
	for (uint32_t step = 1; step <= 21 && step <= STEPS; step++) {
		if (!steps[step - 1]()) {
			printf("mi8gpu: step %u: stopped; the steps after it are not attempted\n", step);
			break;
		}
	}
	if (gpu_kept) {
		uint32_t test = 0;

		if (PE_parse_boot_argn("mi8gpu_selftest", &test, sizeof(test)) && test) {
			mi8_gpu_selftest();
		}
		printf("mi8gpu: gpu_kept is set: calling RedwoodGPUStart\n");
		RedwoodGPUStart();
		printf("mi8gpu: RedwoodGPUStart returned\n");
		return;
	}
	printf("mi8gpu: gpu_kept is NOT set after the steps: the GPU is torn down\n");
	teardown();
	zap_release();
	mi8_keep = false;
	printf("mi8gpu: THE GPU IS NOT STARTED; the system draws with its software renderer\n");
}

/*
 * A630_START_IN_THREAD: the gpu=1 bring-up, run from a kernel thread rather than the boot thread.
 */
static void
mi8_gpu_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t wait_s = 0;

	(void)parameter;
	if (PE_parse_boot_argn("mi8gpu_delay", &wait_s, sizeof(wait_s)) && wait_s > 0) {
		printf("mi8gpu: waiting %u s before the GPU steps\n", wait_s);
		for (uint32_t i = 0; i < wait_s; i++) {
			sleep_us(1000000);
		}
	}
	mi8_gpu_start_by_itself();
}

void
mi8_adreno_init(void)
{
	uint32_t last = 0, only = 0;
	thread_t thread;

	if (!PE_parse_boot_argn("mi8gpu", &last, sizeof(last)) || last == 0) {
		if (PE_parse_boot_argn("gpu", &only, sizeof(only)) && only) {
			/* A630_START_IN_THREAD: from the boot thread, during bsd_init, TrustZone answers
			 * the zap's PAS_INIT_IMAGE with "interrupted" every time; from a kernel thread the
			 * same call authenticates (mi8gpu=21 proves it). Start it there. */
			if (kernel_thread_start(mi8_gpu_thread, NULL, &thread) != KERN_SUCCESS) {
				printf("mi8gpu: no thread for the GPU; starting it here\n");
				mi8_gpu_start_by_itself();
			} else {
				thread_deallocate(thread);
			}
		}
		return;
	}
	if (kernel_thread_start(mi8_adreno_thread, (void *)(uintptr_t)last, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* The calling thread runs on processor `cpu` from now on (the display driver's scaling threads: EXP-0117). */
#include <kern/processor.h>
#include <kern/sched_prim.h>
extern processor_t thread_bind(processor_t processor);
extern unsigned int ml_get_max_cpu_number(void);

void
mi8_bind_thread(int cpu)
{
	processor_t processor;

	if (cpu < 1 || cpu > ml_get_max_cpu_number() || (processor = cpu_to_processor(cpu)) == PROCESSOR_NULL) {
		return;
	}
	thread_bind(processor);
	thread_block(THREAD_CONTINUE_NULL);
}

/*
 * boot-arg mi8_cpuclk=S (EXP-0151): every S seconds, 60 times, each processor's real clock: a thread bound to it in turn runs
 * 5 M dependent additions (one cycle each) three times and the fastest is said in MHz, with the processor number it ran on
 * and the battery's temperature. The frequency domains' PERF_STATE is only what was asked for (mi8_cpufreq); this is what
 * is had -- a thermal limit, or a core that is not the one it seems, shows here. Reads only.
 * EXP-0162: the additions are eight to a turn of the loop (with one a turn the loop's own counting and branch made an in-order
 * little core, which issues two instructions a cycle, look half as fast as it is), and every round also says each domain's
 * CURRENT_VOTE (0x704: the state the limits management lets it have, Linux qcom-cpufreq-hw reg_current_vote; bits 9:0 times
 * 19.2 MHz) beside PERF_STATE (0x920, what was asked for).
 */
extern int redwood_battery_temperature(void);
extern int cpu_number(void);

static void
mi8_cpuclk_thread(void *parameter, __unused wait_result_t result)
{
	uint32_t period = (uint32_t)(uintptr_t)parameter;
	static const uint64_t domains[2] = { 0x17d43000ull, 0x17d45800ull };
	vm_offset_t domain[2];
	/* EXP-0163: the big domain's registers in the ranges Linux and the downstream OSM driver use (config 0x0-0xfc,
	 * status 0x700-0x7fc, 0x900-0x9fc), compared round to round: which of them changes when the big cores go to half */
	static const uint32_t ranges[3][2] = { { 0x0, 0x100 }, { 0x700, 0x800 }, { 0x900, 0xa00 } };
	static uint32_t before[3][64];
	bool have_before = false;

	for (int d = 0; d < 2; d++) {
		domain[d] = (vm_offset_t)ml_io_map(domains[d], 0x1000);
	}
	for (unsigned round = 0; round < 60; round++) {
		char line[400];
		uint64_t now_ns;
		int n;

		absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
		n = snprintf(line, sizeof(line), "mi8clk: round %u, t=%llu s, battery %d C,", round, now_ns / NSEC_PER_SEC,
		    redwood_battery_temperature());

		for (int d = 0; d < 2; d++) {
			if (domain[d] != 0) {
				uint32_t vote = rd(domain[d] + 0x704);

				n += snprintf(line + n, sizeof(line) - (size_t)n, " d%d vote %#x (%u MHz) state %u,", d, vote,
				    (vote & 0x3ff) * 192 / 10, rd(domain[d] + 0x920));
			}
		}
		n += snprintf(line + n, sizeof(line) - (size_t)n, " MHz:");
		if (domain[1] != 0) {
			char changes[600];
			int m = 0;

			for (int r = 0; r < 3; r++) {
				for (uint32_t off = ranges[r][0]; off < ranges[r][1]; off += 4) {
					uint32_t v = rd(domain[1] + off), i = (off - ranges[r][0]) / 4;

					if (!have_before) {
						m += snprintf(changes + m, sizeof(changes) - (size_t)m, "%s%x=%x", m ? " " : "", off, v);
					} else if (v != before[r][i] && m < (int)sizeof(changes) - 40 && !(off >= 0x9c0 && off < 0x9e0)) {
						m += snprintf(changes + m, sizeof(changes) - (size_t)m, " %x:%x>%x", off, before[r][i], v);
					}
					before[r][i] = v;
					if (!have_before && m > (int)sizeof(changes) - 40) {
						printf("mi8osm: %s\n", changes);
						m = 0;
					}
				}
			}
			printf("mi8osm: round %u%s %s\n", round, have_before ? " changed:" : " all:", changes);
			have_before = true;
		}

		for (int cpu = 0; cpu <= (int)ml_get_max_cpu_number(); cpu++) {
			processor_t processor = cpu_to_processor(cpu);
			uint64_t best = UINT64_MAX;
			int on = -1;

			if (processor == PROCESSOR_NULL) {
				continue;
			}
			thread_bind(processor);
			thread_block(THREAD_CONTINUE_NULL);
			for (int tries = 0; tries < 3; tries++) {
				uint64_t t0, t1, ns, x = 1;

				t0 = mach_absolute_time();
				for (uint64_t i = 0; i < 5000000ull / 8; i++) {
					__asm__ volatile ("add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\t"
					    "add %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1\n\tadd %0, %0, #1" : "+r" (x));
				}
				t1 = mach_absolute_time();
				absolutetime_to_nanoseconds(t1 - t0, &ns);
				best = ns < best ? ns : best;
			}
			on = cpu_number();
			n += snprintf(line + n, sizeof(line) - (size_t)n, " cpu%d%s %llu", cpu, on == cpu ? "" : "(?)",
			    best ? 5000000ull * 1000 / best : 0);
		}
		thread_bind(PROCESSOR_NULL);
		thread_block(THREAD_CONTINUE_NULL);
		printf("%s\n", line);
		delay_for_interval(period * 1000, NSEC_PER_MSEC);
	}
	thread_terminate(current_thread());
}
