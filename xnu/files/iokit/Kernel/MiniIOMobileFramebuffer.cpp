/*
 * MiniIOMobileFramebuffer: a stand-in for Apple's display driver family, as seen by
 * IOMobileFramebuffer.framework (and through it by CoreAnimation's window server).
 *
 * The display is the framebuffer the boot loader left us (the one the kernel's text console
 * draws on). There is no hardware behind it that we drive: a "swap" copies the pixels of the
 * surfaces of its layers into that memory, and "vsync" is a timer.
 *
 * The protocol (selectors and the layout of the swap request) was worked out from the
 * user-space side of iOS 16.5. Everything not implemented is logged once.
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>
#include <sys/proc.h>

extern "C" {
#include <console/video_console.h>
extern struct vc_info vinfo;            /* the boot framebuffer, as the text console knows it */
}

#include <IOKit/IOBufferMemoryDescriptor.h>

#include "MiniIOSurface.h"

extern "C" void redwood_tl(const char * format, ...) __printflike(1, 2);

/* IOConnectCallMethod selectors of the framebuffer user client (iOS 16.5) */
enum {
	kFBMethodGetLayerDefaultSurface = 3,
	kFBMethodSwapBegin              = 4,
	kFBMethodSwapEnd                = 5,
	kFBMethodSwapWait               = 6,
	kFBMethodGetID                  = 7,
	kFBMethodGetDisplaySize         = 8,
	kFBMethodRequestPowerChange     = 12,
	kFBMethodSwapSignal             = 20,
	kFBMethodGetDigitalOutState     = 25,
	kFBMethodGetDotPitch            = 28,
	kFBMethodGetDisplayArea         = 29,
	kFBMethodSwapCancel             = 52,
	kFBMethodGetColorRemapMode      = 57,
	kFBMethodNotifications          = 72,
	kFBMethodChangeFrameInfo        = 73,
	kFBMethodSupportedFrameInfo     = 74,
	kFBMethodGetCurrentAbsoluteTime = 77,
	kFBMethodGetBlock               = 79,
	kFBMethodSwapCancelAll          = 81,
	kFBMethodGetProtectionOptions   = 82,
};

/* The swap request (IOMobileFramebufferSwapEnd): 0x46c bytes, of which we read these. */
#define SWAP_SIZE               0x46c
#define SWAP_ID                 0x50
#define SWAP_SURFACE(layer)     (0x54 + 4 * (layer))    /* surface number, 0 = none */
#define SWAP_SOURCE(layer)      (0x64 + 16 * (layer))   /* x, y, width, height in the surface */
#define SWAP_ENABLED(layer)     (0xa4 + 4 * (layer))
#define SWAP_DEST(layer)        (0xc4 + 16 * (layer))   /* x, y, width, height on the display */
#define SWAP_LAYERS_SET         0x104                   /* bit per layer given in this swap */
#define SWAP_LAYERS_CLEARED     0x108
#define SWAP_LAYERS             4

#define kFBNotifyFrameInfo      3                       /* IOMobileFramebufferFrameInfo (EXP-0153/0155) */
#define kFBNotifyVSync          5
#define kFBNotifyTypes          8
#define kFBRefreshHz            60                      /* unless boot-arg fb_hz says otherwise */

#define FRAME_TIMES             64                      /* milliseconds told apart between two frames */
#define FRAME_PAUSE             90                      /* from so many on, nothing was moving: not a slow frame */
#define SWAP_GPU_MS             500                     /* an unfinished frame is discarded after this wait */
#define SWAP_HELD_MS            700                     /* a swap that cannot be taken by then is not shown at all */

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

static uint64_t gFBLogged[2];           /* selectors already reported */
static uint8_t  gFBCalls[128];          /* how many calls of each selector were traced */

/*
 * The display user space is told about. The system carries the identity of the iPhone X
 * (see the loader), and iOS knows what screen that phone has: with any other size
 * MobileGestalt reports an unknown screen class and the user interface has no layout for it.
 * So the display is always the iPhone's, and a swap scales the picture to the panel that is
 * really there (in QEMU the loader makes the panel exactly this size and nothing is scaled).
 * Boot-args fbwidth= and fbheight= change the size reported.
 */
#define kFBDefaultWidth         1125
#define kFBDefaultHeight        2436
#define kFBDotsPerInch          458

static TUNABLE(uint32_t, gFBWidth, "fbwidth", kFBDefaultWidth);
static TUNABLE(uint32_t, gFBHeight, "fbheight", kFBDefaultHeight);
/*
 * Boot-arg fbscale=2: the same 375 x 812 points drawn at two pixels a point instead of three.
 * Everything that is drawn is drawn by the processor, and this is 2.25 times fewer pixels to
 * draw; the picture is then enlarged to the panel and is that much less sharp.
 */
#define kFBPointsWide           375
#define kFBPointsHigh           812
#define kFBDefaultScale         3
static TUNABLE(uint32_t, gFBScale, "fbscale", 0);
/* Boot-arg fbtrace=N: log the first N calls of every selector (default 3). */
static TUNABLE(uint32_t, gFBTrace, "fbtrace", 3);

/*
 * A recording of the screen, for a machine one cannot look at while it runs (the phone is
 * reached only through the boot loader, before and after the kernel).
 * Boot-args rec_secs=N [rec_start=S] [rec_mb=M]: from S seconds after the first frame, for N
 * seconds, every frame that is shown is also kept -- one pixel of every REC_STEP in each
 * direction, three bytes a pixel -- in a piece of memory of M megabytes (default 480, which
 * is 520 frames) that is left in place when the kernel returns to the loader. The loader
 * hands it to the host ("xl recording"), where tools/recording.py makes a video of it.
 *
 *   struct rec_header, then per frame: struct rec_frame, width * height pixels (R, G, B)
 *
 * Boot-arg rec_ring=1: when the memory is full the recording does not stop but goes on over its
 * oldest frames, so that what it holds at the end is the last of the run: for something that
 * happens once in a long session, the run is ended right after it (five fingers) and the frames
 * before it are there. The header ("RWREC003") says where the oldest frame is (frames are all
 * the same size, one after the other) and when the recording began, in the kernel's time, for the
 * frames to be matched with what the window server's per-frame lines (glframes=1,
 * userland/src/eagl.c) say. rec_step=N (2 to 8, default 3) keeps one pixel of every N instead of 3:
 * N = 4 holds 1.8 times as many frames, 6 four times. The memory is taken from the system's for the
 * whole run: 640 MB of it under a long session of applications had iOS killing processes for
 * memory, SpringBoard and the window server among them (two respring in one run); 256 did not.
 */
static TUNABLE(uint32_t, gRecSeconds, "rec_secs", 0);
static TUNABLE(uint32_t, gRecStart, "rec_start", 0);
static TUNABLE(uint32_t, gRecMegabytes, "rec_mb", 480);
static TUNABLE(uint32_t, gRecRing, "rec_ring", 0);
static TUNABLE(uint32_t, gRecStep, "rec_step", 3);
static TUNABLE(uint32_t, gDiff, "fb_diff", 0);
/*
 * Boot-arg fb_filter=0: the picture is scaled to the panel nearest neighbour, as it once was. The
 * iPhone's 1125 x 2436 on this panel's 1080 x 2400 is 45 columns and 36 rows fewer: nearest
 * neighbour leaves out every 25th column and every 67th row, and where one of them goes through a
 * letter or a slanting line, that is a step in it or a stroke a pixel thinner -- text and lines a
 * little crooked, here and there and never in the same places twice. By default every pixel of the
 * panel is worked out from the four of the picture around where it falls (bilinear).
 */
static TUNABLE(uint32_t, gFilter, "fb_filter", 1);
static TUNABLE(uint32_t, gRefreshHz, "fb_hz", kFBRefreshHz);
static TUNABLE(uint32_t, gOnTheBeat, "fb_beat", 1);
/*
 * Boot-arg fb_threads=N (1 to 8): so many processors scale a frame onto the panel together (see
 * drawRows()). Three by default: a frame is in the panel's memory 3 ms after its turn instead of 9
 * to 10 (the phone, 15 October; fb_threads=1 is how it was).
 */
static TUNABLE(uint32_t, gThreads, "fb_threads", 3);
static TUNABLE(uint32_t, gProbe, "fb_probe", 0);   /* EXP-0125: time reading the surface and writing the panel, three times */
static TUNABLE(uint32_t, gDrop, "fb_drop", 0);   /* 1: the surface's rows are dropped from the cache before they are read (U-064: blocks of an older picture?) */
/*
 * Boot-arg fb_partial=0: every frame is scaled onto the whole panel. Otherwise a frame whose picture the
 * program says it changed in a rectangle only (MiniIODamage, from userland/src/eagl.c) is scaled onto that
 * part of the panel, which still has the frame before (EXP-0117); a whole frame every FULL_EVERY frames, and
 * after anything else was put on the panel.
 */
static TUNABLE(uint32_t, gPartial, "fb_partial", 1);
/*
 * fb_flip=1 (EXP-0130): the frames are not drawn into the boot framebuffer under the display hardware's reading but into one of
 * three buffers of our own (physically contiguous, below 4 GB, write-combining), and the DPU's pipe SSPP0 is pointed at the
 * finished one (SRC0_ADDR + CTL0 FLUSH bit 0: EXP-0129) -- the picture changes between two refreshes, so it cannot tear.
 * fb_flip=2: the same with cached buffers, cleaned before the flip. 0: off. Only the part of the panel a frame draws is
 * kept: the damage rectangles (fb_partial) are not used with it (gPartial is read-only by then: tested where it is used).
 */
static TUNABLE(uint32_t, gFlip, "fb_flip", 0);
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

/*
 * fb_flip with fb_telock=1 (default; EXP-0132): the "vsync" the window server is told about is tied to the panel's real
 * tearing-effect pulse instead of a timer of ours that drifts against it. The DPU's ping-pong block 0 counts lines on the
 * vsync clock (19.2 MHz / VSYNC_CONFIG count 142 = 7.396 us a line) from VSYNC_INIT (2248) at each pulse (EXP-0128), so
 * the count says how long ago the last pulse was. fb_teoff = microseconds after the pulse that the notification is due.
 */
static TUNABLE(uint32_t, gTeLock, "fb_telock", 1);
static TUNABLE(uint32_t, gTeOffUs, "fb_teoff", 1000);
/*
 * fb_mailbox=1 (default; EXP-0133): a finished frame whose predecessor the DPU has not taken yet (CTL0 FLUSH still set) takes
 * its place instead of waiting for the next pulse: the presenting thread never stops, the newest frame is the one shown.
 * Buffers: one the DPU reads (latched), at most one waiting (pending), the third is drawn into. 0: wait (EXP-0130).
 */
static TUNABLE(uint32_t, gMailbox, "fb_mailbox", 1);
/*
 * fb_swapwait=1 (EXP-0134): SwapWait (selector 6, which the window server calls for every swap it made) returns once the
 * DPU has taken that swap's frame (or a later one), at most 40 ms on, instead of at once: what a native framebuffer is
 * thought to do [待验证]. 0: at once.
 */
static TUNABLE(uint32_t, gSwapWait, "fb_swapwait", 0);
/*
 * fb_frameinfo=1 (default; EXP-0155): the one frame-info key "Presentation_time" is offered (selector 74) and, once asked for
 * (72 with type 3, 73 with the mask), every frame the DPU takes is reported with the tearing-effect pulse it was taken at.
 * QuartzCore's window server (A11: a timer of its own, not the vsync notifications -- CADeviceUseVBL) sets the phase of that
 * timer from these times only (IOMFBServer::frame_info_callback -> IOMFBDisplay::add_timing -> timing_reference, EXP-0153);
 * with no key offered it never set up frame info at all, and its timer ran at whatever phase it started with. 0: no keys.
 */
static TUNABLE(uint32_t, gFrameInfo, "fb_frameinfo", 1);
/*
 * fb_native_probe=1 (EXP-0165, docs/display-native-plan.md stage 0): with fb_flip, before the flip buffers are made, log
 * what the DPU's scaler, mixer and clock are left as (reads only), try for five pieces of contiguous memory below 4 GB the
 * size of a display surface (freed again at once), and write/read a pattern in the boot splash region past the boot
 * framebuffer (memory nothing else uses: EXP-0137 memory map, type 0, not XNU's); every 300 flips how many display-sized
 * surfaces are live. 0: none of it.
 */
static TUNABLE(uint32_t, gNativeProbe, "fb_native_probe", 0);
/*
 * mi8scale=N (EXP-0166, docs/display-native-plan.md stage 1): with fb_flip, before flipping starts, the DPU is pointed at a
 * 1125 x 2436 test picture in the splash region and its own scaler (SSPP0's QSEED3) puts it on the panel: bit 0 stretched to
 * the whole panel for 6 s, bit 1 the same scale both ways (1038 x 2248, 21 pixels black left and right) for 6 s; then every
 * register is put back as it was. 0: not done.
 */
static TUNABLE(uint32_t, gScaleTest, "mi8scale", 0);
/*
 * ... in a thread of its own: done inside start() (EXP-0166, first try) it held the driver back 12 s, backboardd found no
 * display ("FB Count = 0") and SpringBoard never came up. While it runs, frames are taken but not flipped to.
 */
static volatile bool gScaleBusy;
static uint32_t gProbeSids[8], gProbeFrames;   /* fb_native_probe: the bottom layer's surfaces since the last line (U-081) */
static volatile uint32_t * gTeCount;    /* PP0 INT_COUNT_VAL, or NULL */
extern "C" uint64_t kvtophys(vm_offset_t va);

/*
 * What the DPU must read again when this kernel is gone (EXP-0137): the boot framebuffer, which the loader and the next
 * kernel draw into; our buffers are freed memory then. Set by flipSetup(), used by MiniIOFlipQuiesce() on the way back.
 */
static volatile uint32_t * gFlipPipe, * gFlipCtl;
static uint32_t gFlipBoot;

/*
 * fb_native=1 (EXP-0167, docs/display-native-plan.md stage 2): the flip buffers are three 1125 x 2436 pictures (4608 bytes a
 * row) in the splash region -- the first where the boot framebuffer is -- and the DPU's own scaler (SSPP0's QSEED3, as EXP-0166
 * set it) shrinks them to the panel. The CPU only copies. The registers that were changed are kept here to be put back, all of
 * them in one flush, when the kernel goes back to the loader (MiniIOFlipQuiesce), and the boot framebuffer is cleared.
 */
static TUNABLE(uint32_t, gNative, "fb_native", 0);
static const uint32_t gScalePipeRegs[] = { 0x00, 0x08, 0x0c, 0x10, 0x14, 0x24, 0x100, 0x104, 0x108, 0x110, 0x114, 0x118,
	                                   0x120, 0x124, 0x128 };
static const uint32_t gScalerRegs[] = { 0x04, 0x10, 0x14, 0x18, 0x1c, 0x20, 0x40, 0x44, 0x48, 0x90, 0x94, 0x98, 0x9c };
static uint32_t gNativeSavedPipe[sizeof(gScalePipeRegs) / 4], gNativeSavedScaler[sizeof(gScalerRegs) / 4];
static volatile bool gNativeChanged;            /* the first flip has changed the registers above */
static uint8_t * gNativeBootVirt;               /* the boot framebuffer's memory, as the native buffer A maps it, and ... */
static size_t gNativeBootBytes;                 /* ... the size of the picture the loader's display has in it */

static void flipLog(const char * format, ...) __printflike(1, 2);
extern "C" void loader_log_putc(char c);

/* For the way back (interrupt-disabled: printf would panic, "Console I/O from interrupt-disabled context", EXP-0167): only the loader's log. */
static void
quiesceSay(const char * text)
{
	for (; *text != 0; text++) {
		loader_log_putc(*text);
	}
}

extern "C" void
MiniIOFlipQuiesce(void)
{
	if (gFlipPipe != NULL && gFlipCtl != NULL && gFlipBoot != 0) {
		if (gNativeChanged) {
			quiesceSay("fb_native: giving the registers back\n");
			volatile uint32_t * scaler = gFlipPipe + 0xa00 / 4;

			for (unsigned i = 0; i < sizeof(gScalerRegs) / 4; i++) {
				scaler[gScalerRegs[i] / 4] = gNativeSavedScaler[i];
			}
			for (unsigned i = 0; i < sizeof(gScalePipeRegs) / 4; i++) {
				gFlipPipe[gScalePipeRegs[i] / 4] = gNativeSavedPipe[i];
			}
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		gFlipPipe[0x14 / 4] = gFlipBoot;
		__asm__ volatile ("dsb sy" ::: "memory");
		gFlipCtl[0x18 / 4] = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		if (gNativeChanged) {
			quiesceSay("fb_native: flushed, waiting for the DPU\n");
		}
		if (gNativeChanged && gNativeBootVirt != NULL) {
			/* what the loader shows is read from the same memory: once the DPU has taken the old layout back, clear it */
			uint64_t t0 = mach_absolute_time(), now, ns;

			do {
				now = mach_absolute_time();
				absolutetime_to_nanoseconds(now - t0, &ns);
			} while ((gFlipCtl[0x18 / 4] & 1) != 0 && ns < 40 * NSEC_PER_MSEC);
			do {
				absolutetime_to_nanoseconds(mach_absolute_time() - now, &ns);
			} while (ns < 20 * NSEC_PER_MSEC);
			quiesceSay("fb_native: clearing the boot framebuffer\n");
			bzero(gNativeBootVirt, gNativeBootBytes);
			quiesceSay("fb_native: registers and boot framebuffer put back\n");
			gNativeChanged = false;
		}
	}
}

static bool
teLast(uint64_t * last)
{
	uint32_t line;
	uint64_t now, back;

	if (gTeCount == NULL) {
		return false;
	}
	line = *gTeCount & 0xffff;
	now = mach_absolute_time();
	if (line < 2248 || line > 4700) {
		return false;
	}
	nanoseconds_to_absolutetime((uint64_t) (line - 2248) * 7396, &back);
	*last = now - back;
	return true;
}
/* Exercise the discard path once after N shown frames; no GPU fault is induced. */
static TUNABLE(uint32_t, gTestDrop, "fb_test_drop", 0);
#define FULL_EVERY 90
static IOLock * gDamageLock;
static bool gDamageGiven, gDamageFull;          /* since the last frame: said at all, and the whole of it */
static int32_t gDamage[4];                      /* x0, y0, x1, y1 */

void
MiniIODamage(const int32_t box[4], bool full)
{
	if (gDamageLock == NULL) {
		return;
	}
	IOLockLock(gDamageLock);
	gDamageGiven = true;
	if (full || box[2] <= 0 || box[3] <= 0) {
		gDamageFull = true;
	} else if (!gDamageFull) {
		int32_t x0 = box[0], y0 = box[1], x1 = box[0] + box[2], y1 = box[1] + box[3];

		if (gDamage[2] <= gDamage[0] || gDamage[3] <= gDamage[1]) {
			gDamage[0] = x0; gDamage[1] = y0; gDamage[2] = x1; gDamage[3] = y1;
		} else {
			gDamage[0] = x0 < gDamage[0] ? x0 : gDamage[0];
			gDamage[1] = y0 < gDamage[1] ? y0 : gDamage[1];
			gDamage[2] = x1 > gDamage[2] ? x1 : gDamage[2];
			gDamage[3] = y1 > gDamage[3] ? y1 : gDamage[3];
		}
	}
	IOLockUnlock(gDamageLock);
}

static TUNABLE(uint32_t, gScanOn, "fb_scan", 0);       /* 1: the GPU scales the frame and the CPU only copies it (EXP-0126: not faster than the CPU scaling, 17 vs 14 ms) */
static MiniIOSurface * gScan;                  /* the surface the GPU scales the frame into: the panel's size, rows the GPU can address */
static bool gScanFresh;                        /* ... and it holds the newest frame, not yet shown */
static uint32_t gScanPanelWidth, gScanPanelHeight;

uint32_t
MiniIOScan(uint32_t op, uint32_t surface, uint32_t * width, uint32_t * height)
{
	if (gDamageLock == NULL || !gScanOn) {
		return 1;
	}
	if (op == 0) {
		*width = gScanPanelWidth;
		*height = gScanPanelHeight;
		return gScanPanelWidth != 0 ? 0 : 1;
	}
	if (op == 1) {
		MiniIOSurface * s = MiniIOSurfaceCopy(surface), * old;

		if (s == NULL || s->info.bytesPerElement != 4 || s->info.planeCount != 0 || s->info.width != gScanPanelWidth ||
		    s->info.height != gScanPanelHeight) {
			if (s != NULL) {
				MiniIOSurfaceDrop(s);
			}
			return 1;
		}
		s->gpuOnly = true;
		IOLockLock(gDamageLock);
		old = gScan;
		gScan = s;
		gScanFresh = false;
		IOLockUnlock(gDamageLock);
		if (old != NULL) {
			old->gpuOnly = false;
			MiniIOSurfaceDrop(old);
		}
		return 0;
	}
	IOLockLock(gDamageLock);
	gScanFresh = gScan != NULL && gScan->info.width == gScanPanelWidth;
	IOLockUnlock(gDamageLock);
	return 0;
}

static void bindToCpu(int cpu);
static TUNABLE(uint32_t, gBind, "fb_bind", 1);   /* 1: the threads that scale a frame run on the first processors of the little cluster (1..3), which the window server leaves idle (EXP-0117) */
static TUNABLE(uint32_t, gNoCopy, "fb_nocopy", 0);   /* a measurement (EXP-0117): after the 40th frame nothing is put on the panel */
#define SWAP_QUEUE              2                       /* frames that may wait for the panel at most (fb_queue) */
static TUNABLE(uint32_t, gQueue, "fb_queue", 2);
static inline unsigned
swapQueue(void)
{
	return gQueue >= 1 && gQueue <= SWAP_QUEUE ? gQueue : 1;
}
/*
 * Boot-arg touch_lat=1: a line for every swap handed over ("tl: s ID at US") and for every frame
 * put on the panel ("tl: v ID ready US shown US done US": the GPU done with it, its beat come, the
 * pixels in the panel's memory), in microseconds of mach_absolute_time, for measuring how long a
 * touch takes to show (tools/latency.py; the touch's own lines: bsd/dev/redwood_input.c).
 */
static TUNABLE(uint32_t, gTouchLat, "touch_lat", 0);

static inline uint64_t
micros(uint64_t absolute)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(absolute, &ns);
	return ns / 1000;
}

static inline uint32_t
refreshHz(void)
{
	return gRefreshHz >= 1 && gRefreshHz <= 240 ? gRefreshHz : kFBRefreshHz;
}
static inline uint32_t
recStep(void)
{
	return gRecStep >= 2 && gRecStep <= 8 ? gRecStep : 3;
}
#define REC_STEP        recStep()

struct rec_header {
	char     magic[8];              /* "RWREC003" */
	uint32_t width, height;
	uint32_t frames;
	uint32_t oldest;                /* the place of the oldest frame (rec_ring; 0 until the memory has once been full) */
	uint64_t began;                 /* when the first frame was recorded, in nanoseconds of the kernel's time */
};
struct rec_frame {
	uint64_t nanoseconds;           /* since the first frame of the recording */
};

extern "C" uint64_t loader_recording[3];        /* { physical address, bytes, kernel address }: osfmk/arm64/loader_return.c */

static uint32_t
swapWord(const uint8_t * swap, unsigned offset)
{
	uint32_t value;

	memcpy(&value, swap + offset, sizeof(value));
	return value;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct RowJob;

class IOMobileFramebuffer : public IOService
{
	OSDeclareDefaultStructors(IOMobileFramebuffer);

public:
	IOLock *        fLock;
	uint8_t *       fBase;                  /* the boot framebuffer: the panel */
	bool            fFlipOn;                /* fb_flip: fBase is the buffer being drawn, not the one on the panel */
	int             fFlipShown;             /* ... the buffer the DPU was last told to read (-1: the boot framebuffer) */
	uint32_t        fFlipPhys[3];
	uint8_t *       fFlipVirt[3];
	IOBufferMemoryDescriptor * fFlipMemory[3];
	bool            fNativeOn;              /* fb_native: the flip buffers are the splash region's, 1125 x 2436 */
	bool            fNativeADone;           /* ... buffer 0 has been cleared once the DPU let go of it */
	size_t          fNativeBytes;           /* ... one buffer's size */
	IOMemoryDescriptor * fNativeRegion;
	IOMemoryMap *   fNativeMap;
	volatile uint32_t * fDpuPipe;           /* SSPP0 (VIG0) registers of the DPU */
	volatile uint32_t * fDpuCtl;            /* CTL0 */
	uint32_t        fFlipOriginal;          /* ... SRC0_ADDR the boot loaders left */
	uint32_t        fFlips;
	int             fFlipLatched, fFlipPending, fFlipDraw;  /* fb_mailbox */
	volatile uint32_t fSwapDrawing, fSwapPending, fSwapLatched;     /* the swap IDs of those buffers' frames */
	uint32_t        fFlipReplaced;
	uint64_t        fFlipWaited, fFlipWaitedMax;
	uint32_t        fPanelWidth, fPanelHeight, fRowBytes;
	uint32_t        fWidth, fHeight;        /* the display as user space sees it */
	MiniIOSurface * fDefaultSurface;
	uint32_t        fSwapID;
	uint32_t        fSwapsShown;
	uint32_t        fLayersShown;           /* which layers the last swap had a surface in */
	bool            fGraphics;              /* the text console has been told to stop */
	/*
	 * A frame goes to the panel when the GPU is done with it: a thread of ours waits for that
	 * and copies it there, and whoever made the frame is back at the next one meanwhile. One
	 * frame waits at a time: the swap after it is held, in SwapEnd, until this one is being
	 * copied to the panel. Whoever draws is so at most two frames ahead of the panel, and
	 * needs a third surface to be that: CoreAnimation makes itself a new one for every frame.
	 * By default two frames may wait (boot-arg fb_queue=1: one, as before): CoreAnimation draws on a
	 * clock of its own, and a SwapEnd held past its next tick makes that tick late, the clock going
	 * on from there (rc.jobs.fps: 28% of the frames over 16.7 ms with two, 33% with one).
	 */
	IOLock *        fSwapLock;              /* around the next five */
	uint8_t         fSwap[SWAP_QUEUE][SWAP_SIZE];   /* the swaps on their way to the panel (fb_queue), from fSwapFirst on */
	MiniIOSurface * fSwapSurfaces[SWAP_QUEUE][SWAP_LAYERS]; /* ... their surfaces, held from SwapEnd on: their maker may let go of them */
	unsigned        fSwapFirst, fSwapsWaiting;
	/*
	 * ... and not whenever the GPU happens to be done, but on a beat of the refresh rate
	 * (boot-arg fb_beat=0: at once): frames then follow each other at an even pace whatever
	 * each took to draw, as long as there is one for every beat. The beat is this driver's own.
	 * (CoreAnimation does not go by the refresh notifications of this driver -- it never asks
	 * for them -- but by a clock of its own at the rate it reads here; and where the panel's
	 * scan is nobody can read: the display block's register clock is off.) It starts with a
	 * frame that comes later than a beat and a half after the one before -- the first after a
	 * pause, or every one of a renderer that does not keep up, the software one: such a frame
	 * is shown at once, a beat would only make it later still -- and gives way by an eighth of
	 * what a frame is early or late, so that frames are ready some six milliseconds before
	 * their beat: neither a slow one nor a quick one then lands on the wrong side of it.
	 */
	uint64_t        fBeat;                  /* the beat the last frame was shown on */
	uint64_t        fReady;                 /* when the last frame was ready to be shown */
	uint32_t        fStatOnBeat;            /* frames that waited for a refresh, and for how long together */
	uint64_t        fStatBeatWait;
	uint32_t        fStatSurfaces[8];       /* the first different surfaces the bottom layer was given */
	uint32_t        fStatHeld, fStatLost;   /* swaps that waited for the one before; that gave up waiting */
	uint64_t        fSwapTime;              /* when the last swap was handed over */
	uint32_t        fStatSwapsLate;         /* swaps that came more than a refresh and a quarter after the one before (and no pause) */
	uint64_t        fStatWaiting;           /* how long frames waited for the GPU here */
	/* how the display is doing: frames and time spent drawing them here since fStatSince */
	uint32_t        fStatFrames;
	uint64_t        fStatSince, fStatDrawing;
	/* ... and how evenly: the time from each frame to the next, counted by the millisecond (the last count: longer) */
	uint64_t        fLastFrame;
	uint32_t        fFrameTimes[FRAME_TIMES + 1], fFramePauses, fFrameLongest;
	/* boot-arg fb_diff=1: which pixels of the panel each frame changed (it costs: the panel's memory is slow to read) */
	bool            fDiff;
	uint64_t        fDiffPixels;
	uint32_t        fDiffLeft, fDiffTop, fDiffRight, fDiffBottom;
	/* the screen recording, if one was asked for */
	uint32_t *      fColumn;                /* drawLayer(): source column for every panel column */
	uint16_t *      fColumnWeight;          /* ... with fb_filter: how much of the column after it, of 256 */
	uint32_t *      fBlendRow;              /* ... and a row of the picture blended from two (fBlendRowSize pixels) */
	uint32_t        fBlendRowSize;
	bool            fDamageUse;             /* drawLayer(): only the rectangle in fDamageBox */
	int32_t         fDamageBox[4];          /* ... x0, y0, x1, y1 in the surface */
	bool            fWasLost;
	bool            fPanelWhole;            /* the panel has the frame before, whole, put there by drawLayer() */
	uint32_t        fSinceFull;
	uint8_t *       fRecBase;
	uint64_t        fRecSize, fRecUsed;
	uint64_t        fFirstFrame, fRecFirst; /* when the first frame was shown, and the first recorded */
	bool            fRecDone;
	bool            fRecWrapped;            /* (rec_ring) the memory has been full once: frames go over the oldest */

	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	void publishModes(void);
	MiniIOSurface * defaultSurface(void);
	void present(const uint8_t * swap);
	void show(const uint8_t * swap, MiniIOSurface ** surfaces);
	void consumeSwap(void);
	void presenter(void);
	static void presenterMain(void * self, wait_result_t result);
	void reportFrameTimes(void);
	void drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst);
	void flipSetup();
	void flipPresent();
	bool shownSwap(uint32_t * id);
	void copyScan(MiniIOSurface * s);
	void drawRows(const struct RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow);
	static void helperMain(void * self, wait_result_t result);
	/* the scaling shared out (fb_threads): fThreads drawers, the presenter and fThreads - 1 helpers */
	unsigned        fThreads;
	IOLock *        fHelperLock;
	const struct RowJob * fHelperJob;
	uint64_t        fHelperGeneration;
	unsigned        fHelpersBusy, fHelpersStarted;
	uint32_t *      fHelperBlend[7];
	void record(MiniIOSurface * s);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

static void
setNumber(OSDictionary * dict, const char * key, uint64_t value)
{
	OSNumber * n = OSNumber::withNumber(value, 32);

	if (n != NULL) {
		dict->setObject(key, n);
		n->release();
	}
}

/*
 * The modes of the display, the way CoreAnimation reads them (the properties TimingElements
 * and ColorElements; without a mode it draws nothing). One timing, the panel's size at
 * 60 Hz (boot-arg fb_hz=N: at N), with one colour mode, 8-bit RGB. The identifiers are what "the current mode"
 * (block 0x40) answers with.
 */
#define kFBTimingID     1
#define kFBColorID      1

void
IOMobileFramebuffer::publishModes(void)
{
	OSDictionary * timing = OSDictionary::withCapacity(8);
	OSDictionary * horizontal = OSDictionary::withCapacity(4);
	OSDictionary * vertical = OSDictionary::withCapacity(4);
	OSDictionary * color = OSDictionary::withCapacity(8);
	OSArray * timings = OSArray::withCapacity(1);
	OSArray * colors = OSArray::withCapacity(1);
	const uint32_t rate = refreshHz() << 16;                /* 16.16 fixed point */

	if (timing == NULL || horizontal == NULL || vertical == NULL || color == NULL || timings == NULL || colors == NULL) {
		return;
	}
	setNumber(color, "ID", kFBColorID);
	setNumber(color, "PixelEncoding", 0);                   /* RGB 4:4:4 */
	setNumber(color, "Depth", 8);
	setNumber(color, "Colorimetry", 1);
	setNumber(color, "EOTF", 1);
	setNumber(color, "DynamicRange", 0);
	setNumber(color, "Score", 1);
	color->setObject("IsVirtual", kOSBooleanFalse);
	colors->setObject(color);

	setNumber(horizontal, "Active", fWidth);
	setNumber(horizontal, "Total", fWidth);
	setNumber(horizontal, "SyncRate", rate);
	setNumber(horizontal, "PreciseSyncRate", rate);
	setNumber(vertical, "Active", fHeight);
	setNumber(vertical, "Total", fHeight);
	setNumber(vertical, "SyncRate", rate);
	setNumber(vertical, "PreciseSyncRate", rate);
	setNumber(timing, "ID", kFBTimingID);
	setNumber(timing, "Score", 1);
	timing->setObject("IsVirtual", kOSBooleanFalse);
	timing->setObject("IsOverscanned", kOSBooleanFalse);
	timing->setObject("HorizontalAttributes", horizontal);
	timing->setObject("VerticalAttributes", vertical);
	timing->setObject("ColorModes", colors);
	timings->setObject(timing);

	setProperty("TimingElements", timings);
	setProperty("ColorElements", colors);
	OSSafeReleaseNULL(timings);
	OSSafeReleaseNULL(colors);
	OSSafeReleaseNULL(timing);
	OSSafeReleaseNULL(horizontal);
	OSSafeReleaseNULL(vertical);
	OSSafeReleaseNULL(color);
}

extern "C" void loader_log_putc(char c);

/* IOLog does not reach the loader's log; a line about the flip must (EXP-0130). */
static void __printflike(1, 2)
flipLog(const char * format, ...)
{
	char line[300];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char * c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	printf("%s", line);     /* and the kernel's own log, which the loader's live "oem log" carries in full */
}

/* fb_native_probe: so many registers from `at` on, eight to a line. */
static void
probeDump(const char * what, vm_offset_t base, uint32_t from, uint32_t to)
{
	for (uint32_t off = from; off < to; off += 32) {
		uint32_t v[8];

		for (unsigned i = 0; i < 8; i++) {
			v[i] = off + 4 * i < to ? *(volatile uint32_t *) (base + off + 4 * i) : 0;
		}
		flipLog("fb_native_probe: %s +%#05x: %08x %08x %08x %08x %08x %08x %08x %08x\n", what, off, v[0], v[1], v[2], v[3],
		    v[4], v[5], v[6], v[7]);
	}
}

/*
 * fb_native_probe=1 (EXP-0165). Registers (offsets from Linux dpu1 dpu_4_0_sdm845.h / dpu_hw_sspp.c / dpu_hw_util.c /
 * dpu_hw_lm.c and dispcc-sdm845.c): SSPP0 = VIG0 at MDSS 0x5000, its QSEED3 scaler at +0xa00, CTL0 at 0x2000, MDP top at
 * 0x1000, LM0 at 0x45000; DISP_CC: PLL0 (Fabia) at 0, the MDP clock's RCGR at 0x2088 (CMD, CFG, M, N, D), its CBCR 0x200c.
 */
static void
nativeProbe(vm_offset_t mdss, uint32_t rowBytes, uint32_t panelHeight)
{
	vm_offset_t lm = ml_io_map(0x0ae40000ull, 0x10000), dcc = ml_io_map(0x0af00000ull, 0x3000);
	uint64_t t0, t1, ns;

	probeDump("MDP top", mdss + 0x1000, 0x0, 0x40);
	probeDump("CTL0", mdss + 0x2000, 0x0, 0xe4);
	probeDump("SSPP0", mdss + 0x5000, 0x0, 0x160);
	probeDump("SSPP0 scaler", mdss + 0x5a00, 0x0, 0xa0);
	if (lm != 0) {
		probeDump("LM0", lm + 0x5000, 0x0, 0x100);
	}
	if (dcc != 0) {
		volatile uint32_t * r = (volatile uint32_t *) dcc;
		uint32_t cmd = r[0x2088 / 4], cfg = r[0x208c / 4], l = r[0x4 / 4] & 0xffff, frac = r[0x38 / 4] & 0xffff;
		uint32_t src = (cfg >> 8) & 7, half = cfg & 0x1f;
		uint64_t parent_khz = src == 0 ? 19200 : src == 4 ? 600000 : src == 5 ? 300000 :
		    src == 1 ? (uint64_t) 19200 * l + (uint64_t) 19200 * frac / 65536 : 0;

		probeDump("DISP_CC PLL0", dcc, 0x0, 0x40);
		probeDump("DISP_CC", dcc + 0x2000, 0x0, 0x100);
		flipLog("fb_native_probe: MDP clock: RCGR cmd %#x cfg %#x (source %u, divider %u.%u, mode %u) M %#x N %#x D %#x, "
		    "CBCR %#x; PLL0 L %u frac %#x -> about %llu kHz [parents from dispcc-sdm845.c]\n", cmd, cfg, src, (half + 1) / 2,
		    (half + 1) % 2 ? 5 : 0, (cfg >> 12) & 3, r[0x2090 / 4], r[0x2094 / 4], r[0x2098 / 4], r[0x200c / 4], l, frac,
		    half ? parent_khz * 2 / (half + 1) : parent_khz);
	}

	/* contiguous pieces the size of a display surface (a page of shared state, then 4608 x 2436 x 4), below 4 GB */
	vm_size_t piece = round_page((vm_size_t) 4608 * 2436 + PAGE_SIZE);
	IOBufferMemoryDescriptor * got[5] = { NULL };
	unsigned had = 0;

	for (unsigned i = 0; i < 5; i++) {
		t0 = mach_absolute_time();
		got[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
		    piece, 0xffffc000ull);
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (got[i] == NULL || got[i]->getPhysicalAddress() + piece > 0x100000000ull) {
			flipLog("fb_native_probe: contiguous piece %u of %lu bytes below 4 GB: none (%s, %llu us)\n", i, (unsigned long) piece,
			    got[i] == NULL ? "no memory" : "above 4 GB", ns / 1000);
			OSSafeReleaseNULL(got[i]);
			break;
		}
		had++;
		flipLog("fb_native_probe: contiguous piece %u of %lu bytes at %#llx (%llu us)\n", i, (unsigned long) piece,
		    (unsigned long long) got[i]->getPhysicalAddress(), ns / 1000);
	}
	for (unsigned i = 0; i < 5; i++) {
		OSSafeReleaseNULL(got[i]);
	}
	flipLog("fb_native_probe: %u contiguous pieces had at start, all given back\n", had);

	/* the boot splash region (0x9d400000, 36 MB) past the boot framebuffer: 10 MB on, one piece, write-combined */
	uint64_t phys = 0x9d400000ull + 0xa00000ull;
	IOMemoryDescriptor * region = (size_t) rowBytes * panelHeight <= 0xa00000u && phys + piece <= 0x9f800000ull ?
	    IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) phys, piece, kIODirectionInOut) : NULL;
	IOMemoryMap * map = region != NULL ? region->map(kIOMapAnywhere | kIOMapWriteCombineCache) : NULL;

	if (map == NULL) {
		flipLog("fb_native_probe: the splash region at %#llx cannot be mapped (%s)\n", phys, region ? "map failed" : "no descriptor");
	} else {
		volatile uint64_t * words = (volatile uint64_t *) map->getVirtualAddress();
		size_t count = piece / 8, wrong = 0;
		uint64_t wrote, read;

		t0 = mach_absolute_time();
		for (size_t i = 0; i < count; i++) {
			words[i] = (phys + i * 8) ^ 0x5a5a5a5a00000000ull;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		t1 = mach_absolute_time();
		absolutetime_to_nanoseconds(t1 - t0, &wrote);
		for (size_t i = 0; i < count; i++) {
			wrong += words[i] != ((phys + i * 8) ^ 0x5a5a5a5a00000000ull);
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - t1, &read);
		flipLog("fb_native_probe: splash region %#llx..%#llx mapped write-combined at %#lx: wrote %lu bytes in %llu us, read "
		    "back in %llu us, %lu words wrong\n", phys, phys + piece, (unsigned long) map->getVirtualAddress(),
		    (unsigned long) piece, wrote / 1000, read / 1000, (unsigned long) wrong);
	}
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(region);
}

/*
 * mi8scale (EXP-0166). SSPP0 (VIG0, MDSS 0x5000) registers from Linux dpu_hw_sspp.c, its QSEED3 v1.3 block (+0xa00) from
 * dpu_hw_util.c dpu_hw_setup_scaler3(), the values as _dpu_plane_setup_scaler3() works them out (phase steps in 1/2^21,
 * bilinear, no LUT). All of them are double-buffered: they take effect together at the frame after CTL0 FLUSH bit 0 (VIG0)
 * clears (EXP-0129). FORMAT/UNPACK/OP_MODE stay as UEFI left them (F-042).
 */
#define SCALE_SRC_W     1125
#define SCALE_SRC_H     2436
#define SCALE_STRIDE    4608

static uint64_t
scaleFlush(volatile uint32_t * ctl)
{
	uint64_t t0 = mach_absolute_time(), ns = 0;

	__asm__ volatile ("dsb sy" ::: "memory");
	ctl[0x18 / 4] = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	while ((ctl[0x18 / 4] & 1) != 0) {
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ns > 40 * NSEC_PER_MSEC) {
			return UINT64_MAX;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	return ns / 1000;
}

static void
scaleSet(volatile uint32_t * pipe, uint32_t phys, uint32_t outW, uint32_t outX, uint32_t outH)
{
	volatile uint32_t * scaler = pipe + 0xa00 / 4;
	uint32_t src = (SCALE_SRC_H << 16) | SCALE_SRC_W, out = (outH << 16) | outW;
	uint32_t stepX = (uint32_t) (((uint64_t) SCALE_SRC_W << 21) / outW), stepY = (uint32_t) (((uint64_t) SCALE_SRC_H << 21) / outH);

	pipe[0x00 / 4] = src;                   /* SRC_SIZE */
	pipe[0x08 / 4] = 0;                     /* SRC_XY */
	pipe[0x0c / 4] = out;                   /* OUT_SIZE */
	pipe[0x10 / 4] = outX;                  /* OUT_XY */
	pipe[0x14 / 4] = phys;                  /* SRC0_ADDR */
	pipe[0x24 / 4] = SCALE_STRIDE;          /* SRC_YSTRIDE0 */
	for (uint32_t off = 0x100; off <= 0x120; off += 0x10) {
		pipe[off / 4] = 0;              /* SW_PIX_EXT LR */
		pipe[off / 4 + 1] = 0;          /* ... TB */
		pipe[off / 4 + 2] = src;        /* ... REQ_PIXELS: the whole source, nothing extended */
	}
	scaler[0x10 / 4] = stepX;               /* PHASE_STEP_Y_H */
	scaler[0x14 / 4] = stepY;               /* PHASE_STEP_Y_V */
	scaler[0x18 / 4] = stepX;               /* PHASE_STEP_UV_H */
	scaler[0x1c / 4] = stepY;               /* PHASE_STEP_UV_V */
	scaler[0x20 / 4] = 0x03040304;          /* PRELOAD: x 4, y 3 (Linux's QSEED3 defaults) */
	scaler[0x40 / 4] = src;                 /* SRC_SIZE_Y_RGB_A */
	scaler[0x44 / 4] = src;                 /* SRC_SIZE_UV */
	scaler[0x48 / 4] = out;                 /* DST_SIZE */
	scaler[0x90 / 4] = scaler[0x94 / 4] = scaler[0x98 / 4] = scaler[0x9c / 4] = 0;  /* PHASE_INIT_* */
	/* OP_MODE: bit 0 on, [17:16] 3 bilinear, bit 14 8-bit, bit 10 + [30:29] 1 alpha present and bilinear, bit 31 blend */
	scaler[0x04 / 4] = 0xa0034401u;
}

static void
scaleTest(vm_offset_t mdss)
{
	volatile uint32_t * pipe = (volatile uint32_t *) (mdss + 0x5000), * ctl = (volatile uint32_t *) (mdss + 0x2000);
	uint32_t savedPipe[sizeof(gScalePipeRegs) / 4], savedScaler[sizeof(gScalerRegs) / 4];
	const uint64_t phys = 0x9de00000ull;    /* the splash region past the boot framebuffer (F-042) */
	vm_size_t bytes = round_page((vm_size_t) SCALE_STRIDE * SCALE_SRC_H);
	IOMemoryDescriptor * region = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) phys, bytes, kIODirectionInOut);
	IOMemoryMap * map = region != NULL ? region->map(kIOMapAnywhere | kIOMapWriteCombineCache) : NULL;

	if (map == NULL || pipe[0xa00 / 4] != 0x1003 || (ctl[0x18 / 4] & 1) != 0) {
		flipLog("mi8scale: not done (map %p, scaler version %#x, flush %#x)\n", map, pipe[0xa00 / 4], ctl[0x18 / 4]);
		OSSafeReleaseNULL(map);
		OSSafeReleaseNULL(region);
		return;
	}
	/* the test picture (B, G, R, A in memory: 0xAARRGGBB): see experiments/EXP-0166/README.md */
	uint8_t * base = (uint8_t *) map->getVirtualAddress();
	const int64_t cx = SCALE_SRC_W / 2, cy = SCALE_SRC_H / 2, r0 = 447 * 447, r1 = 453 * 453;

	for (int64_t y = 0; y < SCALE_SRC_H; y++) {
		volatile uint32_t * row = (volatile uint32_t *) (base + y * SCALE_STRIDE);

		for (int64_t x = 0; x < SCALE_STRIDE / 4; x++) {
			int64_t d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
			uint32_t v = 0xff000000u;

			if (x >= SCALE_SRC_W) {
				v = 0;
			} else if (x < 120 && y < 120) {
				v = 0xffff0000u;        /* top left red */
			} else if (x >= SCALE_SRC_W - 120 && y < 120) {
				v = 0xff00ff00u;        /* top right green */
			} else if (x < 120 && y >= SCALE_SRC_H - 120) {
				v = 0xff0000ffu;        /* bottom left blue */
			} else if (x >= SCALE_SRC_W - 120 && y >= SCALE_SRC_H - 120) {
				v = 0xffffff00u;        /* bottom right yellow */
			} else if (x < 3 || y < 3 || x >= SCALE_SRC_W - 3 || y >= SCALE_SRC_H - 3 || x % 125 < 3 || y % 203 < 3 ||
			    (d >= r0 && d <= r1)) {
				v = 0xffffffffu;        /* frame, grid, ring */
			}
			row[x] = v;
		}
	}
	__asm__ volatile ("dsb sy" ::: "memory");

	for (unsigned i = 0; i < sizeof(gScalePipeRegs) / 4; i++) {
		savedPipe[i] = pipe[gScalePipeRegs[i] / 4];
	}
	for (unsigned i = 0; i < sizeof(gScalerRegs) / 4; i++) {
		savedScaler[i] = pipe[(0xa00 + gScalerRegs[i]) / 4];
	}
	for (unsigned part = 0; part < 2; part++) {
		uint32_t outW = part == 0 ? 1080 : 1038, outX = part == 0 ? 0 : 21;
		uint64_t us;

		if ((gScaleTest & (1u << part)) == 0) {
			continue;
		}
		scaleSet(pipe, (uint32_t) phys, outW, outX, 2248);
		us = scaleFlush(ctl);
		flipLog("mi8scale: %s %ux2248 at x %u: flush %s %llu us; read back SRC %#x OUT %#x XY %#x ADDR %#x STRIDE %#x "
		    "REQ %#x, scaler op %#x step %#x/%#x preload %#x src %#x dst %#x\n", part == 0 ? "stretched" : "same scale", outW,
		    outX, us == UINT64_MAX ? "NOT TAKEN after" : "taken in", us == UINT64_MAX ? 40000ull : us, pipe[0], pipe[0xc / 4],
		    pipe[0x10 / 4], pipe[0x14 / 4], pipe[0x24 / 4], pipe[0x108 / 4], pipe[0xa04 / 4], pipe[0xa10 / 4], pipe[0xa14 / 4],
		    pipe[0xa20 / 4], pipe[0xa40 / 4], pipe[0xa48 / 4]);
		for (unsigned second = 0; second < 6; second++) {
			IOSleep(1000);
			flipLog("mi8scale: %u s: flush %#x, PP0 INT_COUNT_VAL %#x\n", second + 1, ctl[0x18 / 4], gTeCount ? *gTeCount : 0);
		}
	}
	for (unsigned i = 0; i < sizeof(gScalerRegs) / 4; i++) {
		pipe[(0xa00 + gScalerRegs[i]) / 4] = savedScaler[i];
	}
	for (unsigned i = 0; i < sizeof(gScalePipeRegs) / 4; i++) {
		pipe[gScalePipeRegs[i] / 4] = savedPipe[i];
	}
	uint64_t us = scaleFlush(ctl);
	flipLog("mi8scale: put back: flush %s %llu us; SRC %#x OUT %#x ADDR %#x STRIDE %#x scaler op %#x\n",
	    us == UINT64_MAX ? "NOT TAKEN after" : "taken in", us == UINT64_MAX ? 40000ull : us, pipe[0], pipe[0xc / 4],
	    pipe[0x14 / 4], pipe[0x24 / 4], pipe[0xa04 / 4]);
	OSSafeReleaseNULL(map);
	OSSafeReleaseNULL(region);
}

static void
scaleTestThread(void * parameter, __unused wait_result_t result)
{
	scaleTest((vm_offset_t) parameter);
	__asm__ volatile ("dmb sy" ::: "memory");
	gScaleBusy = false;
	thread_terminate(current_thread());
}

/* The DPU as EXP-0128 found it: HW_VERSION 0x40000001, SSPP0 reading the panel's size from the boot framebuffer. */
void
IOMobileFramebuffer::flipSetup()
{
	vm_offset_t mdss = ml_io_map(0x0ae00000ull, 0x10000);
	volatile uint32_t * top = (volatile uint32_t *) mdss;

	if (mdss == 0 || top[0] != 0x40000001u) {
		flipLog("MiniIOMobileFramebuffer: fb_flip: not the DPU 4.0 (HW_VERSION %#x): not flipping\n", mdss ? top[0] : 0);
		return;
	}
	fDpuPipe = (volatile uint32_t *) (mdss + 0x5000);
	fDpuCtl = (volatile uint32_t *) (mdss + 0x2000);
	if (fDpuPipe[0] != ((fPanelHeight << 16) | fPanelWidth) || fDpuPipe[0x24 / 4] != fRowBytes || fDpuPipe[0x14 / 4] == 0 ||
	    (fDpuCtl[0x18 / 4] & 1)) {
		flipLog("MiniIOMobileFramebuffer: fb_flip: SSPP0 size %#x stride %#x source %#x, CTL0 flush %#x: not what was seen, "
		    "not flipping\n", fDpuPipe[0], fDpuPipe[0x24 / 4], fDpuPipe[0x14 / 4], fDpuCtl[0x18 / 4]);
		return;
	}
	/* buffer 0 is the boot framebuffer at its own physical address: what SRC0_ADDR holds may be a buffer of a kernel before
	 * this one that did not get round to pointing the DPU back (EXP-0137) */
	fFlipOriginal = (uint32_t) kvtophys((vm_offset_t) fBase);
	if (fFlipOriginal == 0 || kvtophys((vm_offset_t) fBase) > 0xffffffffull) {
		flipLog("MiniIOMobileFramebuffer: fb_flip: no physical address for the boot framebuffer: not flipping\n");
		return;
	}
	if (fDpuPipe[0x14 / 4] != fFlipOriginal) {
		flipLog("MiniIOMobileFramebuffer: fb_flip: the DPU was left reading %#x (a run before this one): back to the boot "
		    "framebuffer %#x\n", fDpuPipe[0x14 / 4], fFlipOriginal);
		fDpuPipe[0x14 / 4] = fFlipOriginal;
		__asm__ volatile ("dsb sy" ::: "memory");
		fDpuCtl[0x18 / 4] = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	gFlipPipe = fDpuPipe;
	gFlipCtl = fDpuCtl;
	gFlipBoot = fFlipOriginal;
	if (gTeLock != 0) {
		vm_offset_t pp = ml_io_map(0x0ae70000ull, 0x10000);

		if (pp != 0 && (*(volatile uint32_t *) (pp + 0x1000) & 1) && (*(volatile uint32_t *) (pp + 0x1030) & 0x80000000u)) {
			gTeCount = (volatile uint32_t *) (pp + 0x1014);
			flipLog("MiniIOMobileFramebuffer: fb_flip: the vsync follows the tearing-effect pulse (%u us after it)\n", gTeOffUs);
		}
	}
	if (gNativeProbe == 1) {
		nativeProbe(mdss, fRowBytes, fPanelHeight);
	}
	/* buffer 0 is the boot framebuffer itself (a third contiguous piece below 4 GB is not to be had once the system is up,
	 * EXP-0130); 1 and 2 are ours */
	fFlipMemory[0] = NULL;
	fFlipVirt[0] = fBase;
	fFlipPhys[0] = fFlipOriginal;
	if (gNative != 0 && gFlip == 1 && gMailbox != 0 && fFlipOriginal == 0x9d400000u) {
		/* three pictures of 1125 x 2436 from the boot framebuffer's address on, in the splash region (36 MiB, not XNU's: EXP-0165) */
		const size_t piece = round_page((size_t) 4608 * 2436);

		fNativeBytes = piece;
		fNativeRegion = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress) 0x9d400000ull, 3 * piece, kIODirectionInOut);
		fNativeMap = fNativeRegion != NULL ? fNativeRegion->map(kIOMapAnywhere | kIOMapWriteCombineCache) : NULL;
		if (fNativeMap == NULL) {
			flipLog("MiniIOMobileFramebuffer: fb_native: the splash region cannot be mapped: not native\n");
			OSSafeReleaseNULL(fNativeRegion);
		} else {
			uint8_t * base = (uint8_t *) fNativeMap->getVirtualAddress();

			/* what is to be put back */
			for (unsigned i = 0; i < sizeof(gScalePipeRegs) / 4; i++) {
				gNativeSavedPipe[i] = fDpuPipe[gScalePipeRegs[i] / 4];
			}
			for (unsigned i = 0; i < sizeof(gScalerRegs) / 4; i++) {
				gNativeSavedScaler[i] = fDpuPipe[(0xa00 + gScalerRegs[i]) / 4];
			}
			gNativeBootVirt = base;
			gNativeBootBytes = (size_t) fRowBytes * fPanelHeight;
			for (unsigned i = 0; i < 3; i++) {
				fFlipMemory[i] = NULL;
				fFlipVirt[i] = base + i * piece;
				fFlipPhys[i] = 0x9d400000u + (uint32_t) (i * piece);
			}
			bzero(fFlipVirt[1], 2 * piece);         /* (buffer 0 is what is on the panel until the first flip) */
			fPanelWidth = 1125;
			fPanelHeight = 2436;
			fRowBytes = 4608;
			fNativeOn = true;
			fFlipShown = 0;
			fFlipOn = true;
			fBase = fFlipVirt[1];
			fFlipLatched = 0;
			fFlipPending = -1;
			fFlipDraw = 1;
			flipLog("MiniIOMobileFramebuffer: fb_native=1: three buffers of 1125 x 2436 at %#x %#x %#x (%lu bytes each), the DPU "
			    "reads %#x until the first flip and then scales\n", fFlipPhys[0], fFlipPhys[1], fFlipPhys[2], (unsigned long) piece,
			    fFlipOriginal);
			return;
		}
	}
	for (unsigned i = 1; i < 3; i++) {
		IOOptionBits options = kIODirectionInOut | kIOMemoryPhysicallyContiguous | (gFlip == 1 ? kIOMapWriteCombineCache : 0);
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options,
		    (mach_vm_size_t) fRowBytes * fPanelHeight, 0xffffc000ull);

		if (memory == NULL || memory->getPhysicalAddress() > 0xffffc000ull) {
			flipLog("MiniIOMobileFramebuffer: fb_flip: no buffer %u (below 4 GB, contiguous): not flipping\n", i);
			for (unsigned j = 1; j < i; j++) {
				OSSafeReleaseNULL(fFlipMemory[j]);
			}
			OSSafeReleaseNULL(memory);
			return;
		}
		fFlipMemory[i] = memory;
		fFlipVirt[i] = (uint8_t *) memory->getBytesNoCopy();
		fFlipPhys[i] = (uint32_t) memory->getPhysicalAddress();
		bzero(fFlipVirt[i], (size_t) fRowBytes * fPanelHeight);
		if (gFlip == 2) {
			memory->performOperation(kIOMemoryIncoherentIOFlush, 0, (size_t) fRowBytes * fPanelHeight);
		}
	}
	if (gScaleTest != 0) {
		thread_t thread;

		gScaleBusy = true;
		if (kernel_thread_start(scaleTestThread, (void *) mdss, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			gScaleBusy = false;
		}
	}
	fFlipShown = 0;
	fFlipOn = true;
	fBase = fFlipVirt[1];
	fFlipLatched = 0;
	fFlipPending = -1;
	fFlipDraw = 1;
	flipLog("MiniIOMobileFramebuffer: fb_flip=%u: three buffers at %#x %#x %#x, the DPU read %#x\n", gFlip, fFlipPhys[0],
	    fFlipPhys[1], fFlipPhys[2], fFlipOriginal);
}

/* The frame in fBase is finished: show it. The buffer before has been latched by now unless frames come faster than the panel. */
void
IOMobileFramebuffer::flipPresent()
{
	int index = (fFlipShown + 1) % 3;
	uint64_t t0 = mach_absolute_time(), waited, ns;

	if (gScaleBusy) {
		/* mi8scale has the DPU (EXP-0166): this frame stays in the buffer being drawn, the next one is drawn over it */
		return;
	}

	if (gMailbox != 0) {
		int drawn = fFlipDraw;
		uint64_t pulse, period, guard, next;

		if (gFlip == 2 && fFlipMemory[drawn] != NULL) {
			fFlipMemory[drawn]->performOperation(kIOMemoryIncoherentIOFlush, 0, (size_t) fRowBytes * fPanelHeight);
		}
		/* not in the last 300 us before a pulse: the waiting frame could be taken between reading FLUSH and writing it */
		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		nanoseconds_to_absolutetime(300 * NSEC_PER_USEC, &guard);
		if (teLast(&pulse)) {
			next = pulse + period;
			if (next > t0 && next - t0 < guard) {
				while (mach_absolute_time() < next + guard / 6) {
				}
			}
		}
		if (fFlipPending >= 0 && (fDpuCtl[0x18 / 4] & 1) == 0) {
			fFlipLatched = fFlipPending;
			fFlipPending = -1;
			fSwapLatched = fSwapPending;
		}
		if (fFlipPending >= 0) {
			fFlipReplaced++;
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		if (fNativeOn && !gNativeChanged) {
			scaleSet(fDpuPipe, fFlipPhys[drawn], 1080, 0, 2248);    /* (the size, the row, the scaler and the address: one flush) */
			gNativeChanged = true;
		} else {
			fDpuPipe[0x14 / 4] = fFlipPhys[drawn];
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		fDpuCtl[0x18 / 4] = 1;
		__asm__ volatile ("dsb sy" ::: "memory");
		fSwapPending = fSwapDrawing;
		fFlipPending = drawn;
		fFlipDraw = 3 - fFlipLatched - drawn;   /* the one neither read nor waiting (0 + 1 + 2 = 3) */
		if (fNativeOn && fFlipDraw == 0 && !fNativeADone) {
			bzero(fFlipVirt[0], fNativeBytes);      /* the boot framebuffer's memory, the DPU no longer reads it */
			fNativeADone = true;
		}
		fBase = fFlipVirt[fFlipDraw];
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		fFlipWaited += ns / 1000;
		fFlipWaitedMax = (ns / 1000) > fFlipWaitedMax ? (ns / 1000) : fFlipWaitedMax;
		if (++fFlips % 300 == 0) {
			flipLog("MiniIOMobileFramebuffer: fb_flip: %u flips (mailbox), %u replaced a frame not yet taken, %llu us spent in all, "
			    "%llu us at most\n", fFlips, fFlipReplaced, fFlipWaited, fFlipWaitedMax);
			if (gNativeProbe != 0) {
				uint32_t live, peak, peakEver, made;

				MiniIOSurfaceDisplayStats(&live, &peak, &peakEver, &made);
				flipLog("fb_native_probe: display-sized surfaces: %u live, at most %u since the last line (%u ever), %u made; "
				    "the bottom layer's surfaces in the last %u frames: %u %u %u %u %u %u %u %u\n", live, peak, peakEver, made,
				    gProbeFrames, gProbeSids[0], gProbeSids[1], gProbeSids[2], gProbeSids[3], gProbeSids[4], gProbeSids[5],
				    gProbeSids[6], gProbeSids[7]);
				bzero(gProbeSids, sizeof(gProbeSids));
				gProbeFrames = 0;
			}
		}
		return;
	}

	if (gFlip == 2 && fFlipMemory[index] != NULL) {
		fFlipMemory[index]->performOperation(kIOMemoryIncoherentIOFlush, 0, (size_t) fRowBytes * fPanelHeight);
	}
	while ((fDpuCtl[0x18 / 4] & 1) != 0) {
		absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
		if (ns > 40 * NSEC_PER_MSEC) {
			flipLog("MiniIOMobileFramebuffer: fb_flip: the last flush was never taken: going on\n");
			break;
		}
	}
	waited = mach_absolute_time() - t0;
	absolutetime_to_nanoseconds(waited, &ns);
	fFlipWaited += ns / 1000;
	fFlipWaitedMax = (ns / 1000) > fFlipWaitedMax ? (ns / 1000) : fFlipWaitedMax;
	__asm__ volatile ("dsb sy" ::: "memory");
	fDpuPipe[0x14 / 4] = fFlipPhys[index];
	__asm__ volatile ("dsb sy" ::: "memory");
	fDpuCtl[0x18 / 4] = 1;
	__asm__ volatile ("dsb sy" ::: "memory");
	fFlipShown = index;
	fBase = fFlipVirt[(index + 1) % 3];
	if (++fFlips % 300 == 0) {
		flipLog("MiniIOMobileFramebuffer: fb_flip: %u flips, waited for the last flush %llu us in all, %llu us at most\n", fFlips,
		    fFlipWaited, fFlipWaitedMax);
	}
}

/*
 * The swap whose frame is on the panel (fb_flip with fb_mailbox): the waiting one if the DPU has taken it since (CTL0 FLUSH
 * clear), else the one taken before. Read without the presenter's cooperation: a flip just being made can make this the
 * frame before for one refresh, which only delays its report by that.
 */
bool
IOMobileFramebuffer::shownSwap(uint32_t * id)
{
	if (!fFlipOn || gMailbox == 0) {
		return false;
	}
	if (fFlipPending >= 0 && (fDpuCtl[0x18 / 4] & 1) == 0) {
		*id = fSwapPending;
	} else {
		*id = fSwapLatched;
	}
	return *id != 0;
}

bool
IOMobileFramebuffer::start(IOService * provider)
{
	thread_t thread;

	if (!IOService::start(provider)) {
		return false;
	}
	if (vinfo.v_baseaddr == 0 || vinfo.v_width == 0 || vinfo.v_height == 0 || vinfo.v_depth != 32) {
		IOLog("MiniIOMobileFramebuffer: no usable boot framebuffer (%ux%u, depth %u)\n", vinfo.v_width, vinfo.v_height,
		    vinfo.v_depth);
		return false;
	}
	fLock = IOLockAlloc();
	gDamageLock = IOLockAlloc();
	fSwapLock = IOLockAlloc();
	if (kernel_thread_start(&IOMobileFramebuffer::presenterMain, this, &thread) != KERN_SUCCESS) {
		IOLog("MiniIOMobileFramebuffer: no thread to put frames on the panel with\n");
		return false;
	}
	thread_set_thread_name(thread, "frames to the panel");
	thread_deallocate(thread);
	fThreads = gThreads >= 1 && gThreads <= 8 ? gThreads : 1;
	fHelperLock = IOLockAlloc();
	for (unsigned i = 0; i + 1 < fThreads; i++) {
		if (kernel_thread_start(&IOMobileFramebuffer::helperMain, this, &thread) != KERN_SUCCESS) {
			fThreads = i + 1;
			break;
		}
		thread_set_thread_name(thread, "frames to the panel: a helper");
		thread_deallocate(thread);
	}
	fBase = (uint8_t *) vinfo.v_baseaddr;
	fPanelWidth = vinfo.v_width;
	fPanelHeight = vinfo.v_height;
	fRowBytes = vinfo.v_rowbytes;
	gScanPanelWidth = vinfo.v_width;
	gScanPanelHeight = vinfo.v_height;
	fWidth = gFBWidth ? gFBWidth : fPanelWidth;
	fHeight = gFBHeight ? gFBHeight : fPanelHeight;
	if (gFBScale != 0 && gFBScale != kFBDefaultScale) {
		static const char * const places[][2] = { { "/chosen", "display-scale" }, { "/product", "artwork-scale-factor" } };
		uint32_t scale = gFBScale;

		fWidth = kFBPointsWide * scale;
		fHeight = kFBPointsHigh * scale;
		/* what the loader's device tree says about the scale is where MobileGestalt has it from */
		for (unsigned i = 0; i < sizeof(places) / sizeof(places[0]); i++) {
			IORegistryEntry * node = IORegistryEntry::fromPath(places[i][0], gIODTPlane);

			if (node != NULL) {
				node->setProperty(places[i][1], &scale, sizeof(scale));
				node->release();
			}
		}
	}
	fColumn = IONewZero(uint32_t, fPanelWidth > 1125 ? fPanelWidth : 1125);              /* (1125: fb_native's panel) */
	fColumnWeight = IONewZero(uint16_t, fPanelWidth > 1125 ? fPanelWidth : 1125);
	fDiff = gDiff != 0;
	fDiffLeft = fDiffTop = ~0u;
	IOLog("MiniIOMobileFramebuffer: display %ux%u on a panel of %ux%u (%u bytes per row)\n", fWidth, fHeight,
	    fPanelWidth, fPanelHeight, fRowBytes);
	if (gFlip != 0) {
		flipSetup();
	}

	if (gRecSeconds != 0) {
		/* one piece of physical memory: the loader will send it over USB as it lies */
		IOBufferMemoryDescriptor * memory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		    kIODirectionInOut | kIOMemoryPhysicallyContiguous, (mach_vm_size_t) gRecMegabytes << 20, ~0ULL);

		if (memory != NULL) {
			struct rec_header * header = (struct rec_header *) memory->getBytesNoCopy();

			fRecBase = (uint8_t *) header;
			fRecSize = (uint64_t) gRecMegabytes << 20;
			fRecUsed = sizeof(*header);
			bzero(header, sizeof(*header));
			memcpy(header->magic, "RWREC003", 8);
			header->width = fWidth / REC_STEP;
			header->height = fHeight / REC_STEP;
			loader_recording[0] = memory->getPhysicalSegment(0, NULL);
			loader_recording[1] = fRecUsed;
			loader_recording[2] = (uint64_t) (uintptr_t) fRecBase;
			IOLog("MiniIOMobileFramebuffer: recording %u s of the screen at %ux%u from second %u, into %u MB at %#llx%s\n",
			    gRecSeconds, header->width, header->height, gRecStart, gRecMegabytes, loader_recording[0],
			    gRecRing ? ", the last of it kept (rec_ring)" : "");
		} else {
			IOLog("MiniIOMobileFramebuffer: no memory for a recording of %u MB\n", gRecMegabytes);
		}
	}

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	/* for who draws with the GPU (userland/src/eagl.c): a frame need not be waited for before it is handed over */
	setProperty("RedwoodWaitsForGPU", kOSBooleanTrue);
	publishModes();
	setName("disp0");
	registerService();
	return true;
}

/* Keep a small copy of the frame that is about to be shown (the surface is the whole screen). */
void
IOMobileFramebuffer::record(MiniIOSurface * s)
{
	struct rec_header * header = (struct rec_header *) fRecBase;
	uint64_t now = mach_absolute_time(), since_first, since_start = 0, frame_size;
	struct rec_frame * frame;
	uint8_t * to;

	if (fFirstFrame == 0) {
		fFirstFrame = now;
	}
	if (fRecBase == NULL || fRecDone || s->info.width < fWidth || s->info.height < fHeight) {
		return;
	}
	absolutetime_to_nanoseconds(now - fFirstFrame, &since_first);
	if (since_first < (uint64_t) gRecStart * NSEC_PER_SEC) {
		return;
	}
	if (fRecFirst == 0) {
		uint64_t began;

		fRecFirst = now;
		absolutetime_to_nanoseconds(now, &began);
		header->began = began;
		IOLog("MiniIOMobileFramebuffer: the recording begins at %llu.%03llu s of the kernel's time (mach_absolute_time)\n",
		    began / NSEC_PER_SEC, began / NSEC_PER_MSEC % 1000);
	}
	absolutetime_to_nanoseconds(now - fRecFirst, &since_start);
	frame_size = sizeof(*frame) + (uint64_t) header->width * header->height * 3;
	if (since_start > (uint64_t) gRecSeconds * NSEC_PER_SEC || (fRecUsed + frame_size > fRecSize && !gRecRing)) {
		fRecDone = true;
		IOLog("MiniIOMobileFramebuffer: recording finished: %u frames, %llu MB\n", header->frames, fRecUsed >> 20);
		return;
	}
	if (fRecUsed + frame_size > fRecSize) {
		/* (rec_ring) round again: over the oldest, which is the next one */
		fRecUsed = sizeof(*header);
		fRecWrapped = true;
	}
	frame = (struct rec_frame *) (fRecBase + fRecUsed);
	frame->nanoseconds = since_start;
	to = (uint8_t *) (frame + 1);
	for (uint32_t y = 0; y < header->height; y++) {
		const uint32_t * from = (const uint32_t *) (s->pixels() + (size_t) (y * REC_STEP + 1) * s->info.bytesPerRow);

		for (uint32_t x = 0; x < header->width; x++) {
			uint32_t pixel = from[x * REC_STEP + 1];        /* B G R A in memory */

			*to++ = (uint8_t) (pixel >> 16);
			*to++ = (uint8_t) (pixel >> 8);
			*to++ = (uint8_t) pixel;
		}
	}
	fRecUsed += frame_size;
	if (!fRecWrapped) {
		header->frames++;
		loader_recording[1] = fRecUsed;
	} else {
		/* the memory full of frames: all of them are handed over, from the one after this */
		header->oldest = (uint32_t) ((fRecUsed - sizeof(*header)) / frame_size);
		if (header->oldest >= header->frames) {
			header->oldest = 0;
		}
		loader_recording[1] = sizeof(*header) + (uint64_t) header->frames * frame_size;
	}
}

MiniIOSurface *
IOMobileFramebuffer::defaultSurface(void)
{
	IOLockLock(fLock);
	if (fDefaultSurface == NULL) {
		fDefaultSurface = MiniIOSurfaceCreateSimple(fWidth, fHeight, FOURCC('B', 'G', 'R', 'A'));
	}
	IOLockUnlock(fLock);
	return fDefaultSurface;
}

/*
 * Two pixels mixed: `w` of 256 of the second. Two of the four channels at a time, in one word.
 */
static inline uint32_t
blend(uint32_t a, uint32_t b, uint32_t w)
{
	uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8 & 0xff00ff;
	uint32_t ga = ((a >> 8 & 0xff00ff) * (256 - w) + (b >> 8 & 0xff00ff) * w) & 0xff00ff00;

	return rb | ga;
}

/*
 * Where the middle of the `i`th of `out` pixels falls among `in` of them, in 256ths of a pixel,
 * kept within the first and the last.
 */
static inline int64_t
sample_at(int64_t i, int64_t in, int64_t out)
{
	int64_t at = ((2 * i + 1) * in - out) * 128 / out;

	return at < 0 ? 0 : at > (in - 1) * 256 ? (in - 1) * 256 : at;
}

/*
 * The rows of one layer, from `y0` to `y1` (panel rows), as drawLayer() has worked them out:
 * scaling a whole frame takes one processor some 9 ms, so with boot-arg fb_threads=N it is shared
 * out between N of them, each with rows of its own and a row of blending of its own.
 */
struct RowJob {
	const uint8_t * pixels;
	uint32_t        stride;
	int32_t         sx, sy, sw, sh, px0, py0, px1, py1, cx0, cx1, ry0, ry1;
	bool            filter, unscaled, copyOnly;     /* copyOnly: rows of a frame the GPU has scaled already: dropped from the cache, sent as they are */
};

/*
 * The GPU wrote the surface's memory behind this processor's cache: whatever lines of it the cache took in
 * meanwhile (a prefetch, the frame before in the same surface) are old. Read through them, they are the
 * blocks of an older picture in the new one (EXP-0116, U-064). So the rows about to be read are dropped
 * from the cache first -- by whoever reads them, which spreads the work over the scaling's threads.
 */
static TUNABLE(uint32_t, gScanDrop, "fb_scandrop", 1);   /* 0 nothing, 1 dc civac, 2 dc ivac before the scaled frame is read (EXP-0126) */

static void
dropFromCache(const void * start, size_t bytes)
{
	uintptr_t at = (uintptr_t) start & ~(uintptr_t) 63, end = (uintptr_t) start + bytes;

	if (gScanDrop == 2) {
		for (; at < end; at += 64) {
			__asm__ volatile ("dc ivac, %0" : : "r" (at) : "memory");
		}
	} else if (gScanDrop == 1) {
		for (; at < end; at += 64) {
			__asm__ volatile ("dc civac, %0" : : "r" (at) : "memory");
		}
	}
	__asm__ volatile ("dsb sy" : : : "memory");
}

void
IOMobileFramebuffer::drawRows(const RowJob * job, int32_t y0, int32_t y1, uint32_t * blendRow)
{
	const uint8_t * pixels = job->pixels;
	uint32_t stride = job->stride;
	int32_t sx = job->sx, sy = job->sy, sw = job->sw, sh = job->sh;
	int32_t px0 = job->px0, py0 = job->py0, px1 = job->px1, py1 = job->py1, cx0 = job->cx0, cx1 = job->cx1;
	bool filter = job->filter;
	if (job->copyOnly) {
		if (gProbe && y0 == 0 && (fSwapsShown % 40) == 0) {
			uint64_t a = mach_absolute_time(), b, c;
			uint32_t sum = 0;

			for (int32_t y = y0; y < y1; y++) {
				const uint32_t * in = (const uint32_t *) (job->pixels + (size_t) y * job->stride);

				for (uint32_t x = 0; x < fPanelWidth; x++) {
					sum += in[x];
				}
			}
			b = mach_absolute_time();
			for (int32_t y = y0; y < y1; y++) {
				memcpy(fBase + (size_t) y * fRowBytes, job->pixels + (size_t) y * job->stride, (size_t) fPanelWidth * 4);
			}
			c = mach_absolute_time();
			redwood_tl("tl: c share %d rows: read only %llu us, then copy (cached source now) %llu us (%u)\n", y1 - y0, micros(b) - micros(a),
			    micros(c) - micros(b), sum);
		}
		for (int32_t y = y0; y < y1; y++) {
			const uint8_t * from = job->pixels + (size_t) y * job->stride;

			if (y < 0 || (uint32_t) y >= fPanelHeight) {
				continue;
			}
			dropFromCache(from, (size_t) fPanelWidth * 4);
			memcpy(fBase + (size_t) y * fRowBytes, from, (size_t) fPanelWidth * 4);
		}
		return;
	}
	uint32_t * line = blendRow + fBlendRowSize / 2;         /* (the second half of the row buffer: a panel row made before it is sent) */

	if (y1 > y0 && gDrop) {
		int32_t first, last;

		if (job->unscaled) {
			first = sy + y0 - py0;
			last = sy + y1 - 1 - py0;
		} else if (filter) {
			first = (int32_t) (sample_at(y0 - py0, sh, py1 - py0) >> 8);
			last = (int32_t) (sample_at(y1 - 1 - py0, sh, py1 - py0) >> 8) + 1;
			first += sy;
			last += sy;
		} else {
			first = sy + (int32_t) ((int64_t) (y0 - py0) * sh / (py1 - py0));
			last = sy + (int32_t) ((int64_t) (y1 - 1 - py0) * sh / (py1 - py0));
		}
		if (first < 0) {
			first = 0;
		}
		if (last >= first) {
			dropFromCache(pixels + (size_t) first * stride, (size_t) (last - first + 1) * stride);
		}
	}
	uint64_t tv = 0, th = 0, tc = 0, tm;

	for (int32_t y = y0; y < y1; y++) {
		const uint32_t * from;
		uint32_t * to;

		if (y < 0 || (uint32_t) y >= fPanelHeight) {
			continue;
		}
		tm = gProbe ? mach_absolute_time() : 0;
		to = (uint32_t *) (fBase + (size_t) y * fRowBytes);
		if (job->unscaled) {
			/* not scaled at all */
			memcpy(to + px0, pixels + (size_t) (sy + y - py0) * stride + (size_t) sx * 4, (size_t) sw * 4);
			continue;
		}
		if (filter) {
			int64_t at = sample_at(y - py0, sh, py1 - py0);
			uint32_t row = (uint32_t) (at >> 8), weight = (uint32_t) (at & 255);
			const uint32_t * upper = (const uint32_t *) (pixels + (size_t) (sy + row) * stride) + sx;

			if (weight == 0 || row + 1 >= (uint32_t) sh) {
				from = upper;
			} else {
				const uint32_t * lower = (const uint32_t *) (pixels + (size_t) (sy + row + 1) * stride) + sx;
				uint32_t first = fColumn[cx0], last = fColumn[cx1 - 1] + 1 < (uint32_t) sw ? fColumn[cx1 - 1] + 1 : fColumn[cx1 - 1];

				uint32_t * __restrict mixed = blendRow;

				/* two pixels to a 64-bit word, no branch (EXP-0125: the branchy one cost 11 us a row, most of the frame) */
				const uint64_t M = 0x00ff00ff00ff00ffull;
				uint32_t c = first;

				for (; c + 1 <= last; c += 2) {
					uint64_t a, b, rb, ga;

					memcpy(&a, upper + c, 8);
					memcpy(&b, lower + c, 8);
					rb = ((a & M) * (256 - weight) + (b & M) * weight) >> 8 & M;
					ga = ((a >> 8 & M) * (256 - weight) + (b >> 8 & M) * weight) & ~M;
					rb |= ga;
					memcpy(mixed + c, &rb, 8);
				}
				for (; c <= last; c++) {
					mixed[c] = blend(upper[c], lower[c], weight);
				}
				from = blendRow;
			}
			if (gProbe) {
				uint64_t now = mach_absolute_time();

				tv += now - tm;
				tm = now;
			}
		} else {
			from = (const uint32_t *) (pixels + (size_t) (sy + (int64_t) (y - py0) * sh / (py1 - py0)) * stride);
		}
		if (fDiff) {
			for (int32_t x = cx0; x < cx1; x++) {
				uint32_t value = filter ? blend(from[fColumn[x]], from[fColumn[x] + (fColumnWeight[x] != 0)], fColumnWeight[x]) :
				    from[fColumn[x]];

				if (to[x] != value) {
					fDiffPixels++;
					fDiffLeft = (uint32_t) x < fDiffLeft ? (uint32_t) x : fDiffLeft;
					fDiffRight = (uint32_t) x > fDiffRight ? (uint32_t) x : fDiffRight;
					fDiffTop = (uint32_t) y < fDiffTop ? (uint32_t) y : fDiffTop;
					fDiffBottom = (uint32_t) y > fDiffBottom ? (uint32_t) y : fDiffBottom;
				}
			}
		}
		if (filter) {
			const uint32_t * __restrict column = fColumn + cx0;
			const uint16_t * __restrict weights = fColumnWeight + cx0;
			uint32_t * __restrict out = line + cx0;

			for (uint32_t i = 0, n = (uint32_t) (cx1 - cx0); i < n; i++) {
				uint32_t c = column[i], w = weights[i], a = from[c];

				out[i] = w == 0 || a == from[c + 1] ? a : blend(a, from[c + 1], w);
			}
		} else {
			for (int32_t x = cx0; x < cx1; x++) {
				line[x] = from[fColumn[x]];
			}
		}
		/*
		 * The row is made in memory that is cached and goes to the panel in one piece: the panel's memory takes a
		 * word at a time in 13 ms for 1000 rows and memcpy()'s wide stores in 4 ms (EXP-0125, fb_probe).
		 */
		if (gProbe) {
			uint64_t now = mach_absolute_time();

			th += now - tm;
			tm = now;
		}
		memcpy(to + cx0, line + cx0, (size_t) (cx1 - cx0) * 4);
		if (gProbe) {
			tc += mach_absolute_time() - tm;
		}
	}
	if (gProbe && y1 - y0 > 100) {
		redwood_tl("tl: r %d rows: vertical %llu us, horizontal %llu us, copy to panel %llu us\n", y1 - y0, micros(tv) , micros(th), micros(tc));
	}
}

/* A helper of the scaling (fb_threads): waits for rows to do, does them, says so. */
void
IOMobileFramebuffer::helperMain(void * self, wait_result_t result __unused)
{
	IOMobileFramebuffer * fb = (IOMobileFramebuffer *) self;
	unsigned index;
	uint64_t done = 0;

	IOLockLock(fb->fHelperLock);
	index = fb->fHelpersStarted++;
	IOLockUnlock(fb->fHelperLock);
	bindToCpu(1 + (int) (index % 2));
	for (;;) {
		const RowJob * job;
		int32_t rows, y0, y1;
		uint32_t * blend;

		IOLockLock(fb->fHelperLock);
		while (fb->fHelperGeneration == done) {
			IOLockSleep(fb->fHelperLock, &fb->fHelperGeneration, THREAD_UNINT);
		}
		done = fb->fHelperGeneration;
		job = fb->fHelperJob;
		IOLockUnlock(fb->fHelperLock);
		/* share index + 1 of fThreads (the presenter takes share 0) */
		rows = job->ry1 - job->ry0;
		y0 = job->ry0 + (int32_t) ((int64_t) rows * (index + 1) / fb->fThreads);
		y1 = job->ry0 + (int32_t) ((int64_t) rows * (index + 2) / fb->fThreads);
		blend = fb->fHelperBlend[index];
		fb->drawRows(job, y0, y1, blend);
		IOLockLock(fb->fHelperLock);
		fb->fHelpersBusy--;
		if (fb->fHelpersBusy == 0) {
			IOLockWakeup(fb->fHelperLock, &fb->fHelpersBusy, true);
		}
		IOLockUnlock(fb->fHelperLock);
	}
}

/* A frame the GPU has scaled to the panel's size: only copied, by all the threads (no reading in two rows, no blending). */
void
IOMobileFramebuffer::copyScan(MiniIOSurface * s)
{
	RowJob job;

	bzero(&job, sizeof(job));
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.ry0 = 0;
	job.ry1 = (int32_t) fPanelHeight;
	job.cx0 = 0;
	job.cx1 = (int32_t) fPanelWidth;
	job.copyOnly = true;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads) {
		drawRows(&job, job.ry0, job.ry1, fBlendRow);
		return;
	}
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	drawRows(&job, job.ry0, job.ry0 + (int32_t) ((int64_t) (job.ry1 - job.ry0) / fThreads), fBlendRow);
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
}

/*
 * Put a rectangle of a 32-bit surface on the screen. `dst` is in display coordinates; the
 * panel may be of another size, and the layer itself may be scaled: both are done in one
 * step, bilinear (or nearest neighbour: fb_filter=0).
 */
void
IOMobileFramebuffer::drawLayer(MiniIOSurface * s, const int32_t * src, const int32_t * dst)
{
	int32_t sx = src[0], sy = src[1], sw = src[2], sh = src[3];
	int32_t dx = dst[0], dy = dst[1], dw = dst[2], dh = dst[3];
	int32_t px0, py0, px1, py1, cx0, cx1;
	bool filter = gFilter != 0;
	RowJob job;

	if (sw <= 0 || sh <= 0) {
		sx = sy = 0;
		sw = (int32_t) s->info.width;
		sh = (int32_t) s->info.height;
	}
	if (dw <= 0 || dh <= 0) {
		dx = dy = 0;
		dw = (int32_t) fWidth;
		dh = (int32_t) fHeight;
	}
	if (sx < 0 || sy < 0 || (uint32_t) (sx + sw) > s->info.width || (uint32_t) (sy + sh) > s->info.height) {
		return;
	}
	if (gProbe && (fSwapsShown == 60 || fSwapsShown == 120 || fSwapsShown == 180)) {
		/* EXP-0125: what is slow, reading the surface or writing the panel? */
		const uint32_t * in = (const uint32_t *) s->pixels();
		uint32_t rows = s->info.height < 1000 ? s->info.height : 1000, words = s->info.bytesPerRow / 4, sum = 0;
		uint64_t a = mach_absolute_time(), b, c, d;

		for (uint32_t y = 0; y < rows; y++) {
			for (uint32_t x = 0; x < words; x += 1) {
				sum += in[(size_t) y * words + x];
			}
		}
		b = mach_absolute_time();
		for (uint32_t y = 0; y < fPanelHeight && y < 1000; y++) {
			uint32_t * to = (uint32_t *) (fBase + (size_t) y * fRowBytes);

			for (uint32_t x = 0; x < fPanelWidth; x++) {
				to[x] = sum;
			}
		}
		c = mach_absolute_time();
		for (uint32_t y = 0; y < fPanelHeight && y < 1000; y++) {
			memcpy(fBase + (size_t) y * fRowBytes, (const uint8_t *) in + (size_t) y * s->info.bytesPerRow, fPanelWidth * 4);
		}
		d = mach_absolute_time();
		redwood_tl("tl: p read1000rows %llu us, write1000rows %llu us, memcpy1000rows %llu us (sum %u)\n", micros(b) - micros(a),
		    micros(c) - micros(b), micros(d) - micros(c), sum);
	}
	/* the rectangle on the panel */
	px0 = (int32_t) ((int64_t) dx * fPanelWidth / fWidth);
	py0 = (int32_t) ((int64_t) dy * fPanelHeight / fHeight);
	px1 = (int32_t) ((int64_t) (dx + dw) * fPanelWidth / fWidth);
	py1 = (int32_t) ((int64_t) (dy + dh) * fPanelHeight / fHeight);
	if (px1 <= px0 || py1 <= py0) {
		return;
	}
	/* (blending, a row of the picture mixed from two: room for it, for every one who draws rows) */
	if (fBlendRowSize < 2 * (uint32_t) sw) {
		if (fBlendRow != NULL) {
			IODelete(fBlendRow, uint32_t, fBlendRowSize);
		}
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			if (fHelperBlend[i] != NULL) {
				IODelete(fHelperBlend[i], uint32_t, fBlendRowSize);
			}
		}
		fBlendRowSize = 2 * (uint32_t) sw;
		fBlendRow = IONew(uint32_t, fBlendRowSize);
		for (unsigned i = 0; i + 1 < fThreads; i++) {
			fHelperBlend[i] = IONew(uint32_t, fBlendRowSize);
			if (fHelperBlend[i] == NULL && fBlendRow != NULL) {
				IODelete(fBlendRow, uint32_t, fBlendRowSize);
				fBlendRow = NULL;
			}
		}
		if (fBlendRow == NULL) {
			fBlendRowSize = 0;
		}
	}
	filter = filter && fBlendRow != NULL;
	/* which pixel of a source row each pixel of a panel row takes: worked out once, not 2.6 million times */
	cx0 = px0 < 0 ? 0 : px0;
	cx1 = (uint32_t) px1 > fPanelWidth ? (int32_t) fPanelWidth : px1;
	int32_t ry0 = py0, ry1 = py1;

	if (fDamageUse) {
		/* only what changed, and a margin for the filter's neighbours (source pixels -> panel pixels, rounded outwards) */
		int64_t pw = px1 - px0, ph = py1 - py0;
		int32_t x0 = px0 + (int32_t) ((int64_t) (fDamageBox[0] - sx - 2) * pw / sw) - 2;
		int32_t x1 = px0 + (int32_t) ((int64_t) (fDamageBox[2] - sx + 2) * pw / sw) + 3;
		int32_t y0 = py0 + (int32_t) ((int64_t) (fDamageBox[1] - sy - 2) * ph / sh) - 2;
		int32_t y1 = py0 + (int32_t) ((int64_t) (fDamageBox[3] - sy + 2) * ph / sh) + 3;

		cx0 = cx0 > x0 ? cx0 : x0;
		cx1 = cx1 < x1 ? cx1 : x1;
		ry0 = ry0 > y0 ? ry0 : y0;
		ry1 = ry1 < y1 ? ry1 : y1;
		if (cx1 <= cx0 || ry1 <= ry0) {
			return;
		}
	}
	for (int32_t x = cx0; x < cx1; x++) {
		if (filter) {
			int64_t at = sample_at(x - px0, sw, px1 - px0);

			fColumn[x] = (uint32_t) (at >> 8);              /* (in the blended row: from sx on) */
			fColumnWeight[x] = (uint16_t) (fColumn[x] + 1 < (uint32_t) sw ? at & 255 : 0);
		} else {
			fColumn[x] = (uint32_t) (sx + (int64_t) (x - px0) * sw / (px1 - px0));
		}
	}
	job.pixels = s->pixels();
	job.stride = s->info.bytesPerRow;
	job.sx = sx;
	job.sy = sy;
	job.sw = sw;
	job.sh = sh;
	job.px0 = px0;
	job.py0 = py0;
	job.px1 = px1;
	job.py1 = py1;
	job.cx0 = cx0;
	job.cx1 = cx1;
	job.ry0 = ry0;
	job.ry1 = ry1;
	job.filter = filter;
	job.unscaled = px1 - px0 == sw && py1 - py0 == sh && px0 >= 0 && (uint32_t) px1 <= fPanelWidth;
	if (fThreads <= 1 || fDiff || fHelpersStarted + 1 < fThreads || ry1 - ry0 < 64) {
		drawRows(&job, ry0, ry1, fBlendRow);
		return;
	}
	/* shared out: the helpers take their rows, this thread the first share, then waits for them */
	uint64_t t0 = mach_absolute_time(), t1, t2;
	IOLockLock(fHelperLock);
	fHelperJob = &job;
	fHelpersBusy = fThreads - 1;
	fHelperGeneration++;
	IOLockWakeup(fHelperLock, &fHelperGeneration, false);
	IOLockUnlock(fHelperLock);
	t1 = mach_absolute_time();
	drawRows(&job, ry0, ry0 + (int32_t) ((int64_t) (ry1 - ry0) / fThreads), fBlendRow);
	t2 = mach_absolute_time();
	IOLockLock(fHelperLock);
	while (fHelpersBusy != 0) {
		IOLockSleep(fHelperLock, &fHelpersBusy, THREAD_UNINT);
	}
	IOLockUnlock(fHelperLock);
	if (gTouchLat) {
		redwood_tl("tl: e wake %llu own %llu helpers_after %llu rows %d\n", micros(t1) - micros(t0), micros(t2) - micros(t1),
		    micros(mach_absolute_time()) - micros(t2), ry1 - ry0);
	}
}

/*
 * How evenly the frames of the last seconds came: how many followed the one before within the
 * time of one refresh at 120, 60, 40 and 30 a second, and the times half and 95 in 100 of them
 * stayed under. With the lock held.
 */
void
IOMobileFramebuffer::reportFrameTimes(void)
{
	static const uint32_t limits[] = { 10, 18, 26, 35, FRAME_TIMES + 1 };
	uint32_t within[sizeof(limits) / sizeof(limits[0])] = { 0 }, total = 0, sum = 0, half = 0, most = 0;

	for (uint32_t ms = 0; ms <= FRAME_TIMES; ms++) {
		for (uint32_t i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
			if (ms < limits[i]) {
				within[i] += fFrameTimes[ms];
				break;
			}
		}
		total += fFrameTimes[ms];
	}
	for (uint32_t ms = 0; ms <= FRAME_TIMES && total != 0; ms++) {
		sum += fFrameTimes[ms];
		if (half == 0 && 2 * sum >= total) {
			half = ms + 1;
		}
		if (most == 0 && 100 * sum >= 95 * total) {
			most = ms + 1;
		}
	}
	if (total != 0) {
		IOLog("MiniIOMobileFramebuffer: from one frame to the next: %u times under 10 ms, %u under 18, %u under 26, %u under 35, "
		    "%u longer (the longest %u ms); half under %u ms, 95 in 100 under %u ms; %u pauses\n", within[0], within[1],
		    within[2], within[3], within[4], fFrameLongest, half, most, fFramePauses);
	}
	bzero(fFrameTimes, sizeof(fFrameTimes));
	fFramePauses = fFrameLongest = 0;
}

/* SwapEnd: the swap is handed to the thread that puts frames on the panel, as soon as that has no other waiting. */
void
IOMobileFramebuffer::present(const uint8_t * swap)
{
	AbsoluteTime deadline;

	uint64_t now = mach_absolute_time(), period, pause;

	/* (a frame that is late on the panel was late here already, or it was not: for telling whose it is) */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz() * 5 / 4, &period);
	nanoseconds_to_absolutetime(FRAME_PAUSE * NSEC_PER_MSEC, &pause);
	clock_interval_to_deadline(SWAP_HELD_MS, kMillisecondScale, &deadline);
	if (gTouchLat) {
		redwood_tl("tl: s %u at %llu\n", swapWord(swap, SWAP_ID), micros(now));
	}
	IOLockLock(fSwapLock);
	fStatSwapsLate += fSwapTime != 0 && now - fSwapTime > period && now - fSwapTime < pause;
	fSwapTime = now;
	fStatHeld += fSwapsWaiting >= swapQueue();
	while (fSwapsWaiting >= swapQueue()) {
		if (IOLockSleepDeadline(fSwapLock, &fSwapsWaiting, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
		    fSwapsWaiting >= swapQueue()) {
			fStatLost++;                    /* (the GPU has stopped answering: the frame before is waiting for it) */
			fWasLost = true;
			IOLockUnlock(fSwapLock);
			return;
		}
	}
	unsigned slot = (fSwapFirst + fSwapsWaiting) % SWAP_QUEUE;

	memcpy(fSwap[slot], swap, SWAP_SIZE);
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));

		fSwapSurfaces[slot][layer] = sid != 0 ? MiniIOSurfaceCopy(sid) : NULL;
	}
	fSwapsWaiting++;
	IOLockWakeup(fSwapLock, fSwap, true);
	IOLockUnlock(fSwapLock);
}

/* Run the calling thread on one processor for good (fb_bind); mi8_bind_thread() is in bsd/dev/mi8_adreno.c. */
extern "C" void mi8_bind_thread(int cpu);

static void
bindToCpu(int cpu)
{
	if (gBind) {
		mi8_bind_thread(gBind == 2 ? cpu + 3 : cpu);          /* fb_bind=2: the big cluster, 4..6 */
	}
}

void
IOMobileFramebuffer::presenterMain(void * self, wait_result_t result __unused)
{
	bindToCpu(3);
	((IOMobileFramebuffer *) self)->presenter();
}

void
IOMobileFramebuffer::presenter(void)
{
	static uint8_t swap[SWAP_SIZE];         /* (one thread) */
	MiniIOSurface * surfaces[SWAP_LAYERS];

	for (;;) {
		IOLockLock(fSwapLock);
		while (fSwapsWaiting == 0) {
			IOLockSleep(fSwapLock, fSwap, THREAD_UNINT);
		}
		memcpy(swap, fSwap[fSwapFirst], SWAP_SIZE);
		memcpy(surfaces, fSwapSurfaces[fSwapFirst], sizeof(surfaces));
		IOLockUnlock(fSwapLock);
		show(swap, surfaces);
		for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
			if (surfaces[layer] != NULL) {
				MiniIOSurfaceDrop(surfaces[layer]);
			}
		}
		MiniIOSurfaceClearKept();               /* (the next frame is some milliseconds away) */
	}
}

/* Both shown and rejected swaps must release their queue slot exactly once. */
void
IOMobileFramebuffer::consumeSwap(void)
{
	IOLockLock(fSwapLock);
	fSwapFirst = (fSwapFirst + 1) % SWAP_QUEUE;
	fSwapsWaiting--;
	IOLockWakeup(fSwapLock, &fSwapsWaiting, true);
	IOLockUnlock(fSwapLock);
}

/* A swap's surfaces onto the panel, once the GPU is done with them. */
void
IOMobileFramebuffer::show(const uint8_t * swap, MiniIOSurface ** surfaces)
{
	uint32_t set = swapWord(swap, SWAP_LAYERS_SET);
	uint64_t began = mach_absolute_time(), nanos, ready, shown, drawn;
	uint32_t layers = 0;
	MiniIOSurface * recorded = NULL;
	bool blanked = false;
	bool report;

	bool complete = true;
	static bool injected;
	bool testDrop = gTestDrop && !injected && fSwapsShown >= gTestDrop;
	if (gNativeProbe != 0) {
		/* (U-081: one surface for every frame, or a few in turn? the first eight different ones) */
		uint32_t sid0 = swapWord(swap, SWAP_SURFACE(0));

		gProbeFrames++;
		for (unsigned i = 0; sid0 != 0 && i < 8; i++) {
			if (gProbeSids[i] == 0 || gProbeSids[i] == sid0) {
				gProbeSids[i] = sid0;
				break;
			}
		}
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		if (sid != 0 && surfaces[layer] == NULL) {
			IOLog("MiniIOMobileFramebuffer: swap %u surface %u unavailable or unfinished; discard entire frame\n",
			    swapWord(swap, SWAP_ID), sid);
			complete = false;
			break;
		}
	}
	bool held = complete && MiniIOSurfaceBeginDisplay(surfaces, SWAP_LAYERS, SWAP_GPU_MS);
	complete &= held;
	fStatWaiting += mach_absolute_time() - began;
	if (!complete || testDrop) {
		if (held) {
			MiniIOSurfaceEndDisplay(surfaces, SWAP_LAYERS);
		}
		injected |= testDrop;
		/* The next partial update cannot rely on a frame we never displayed. */
		fPanelWhole = false;
		fWasLost = true;
		IOLockLock(fSwapLock);
		fStatLost++;
		IOLockUnlock(fSwapLock);
		redwood_tl("tl: x %u at %llu reason %s\n", swapWord(swap, SWAP_ID), micros(mach_absolute_time()),
		    testDrop ? "test" : "not-ready");
		consumeSwap();
		return; /* presenter() still drops every retained surface reference. */
	}
	began = ready = mach_absolute_time();
	if (gOnTheBeat && !fFlipOn) {
		uint64_t period, aim, next;

		nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
		aim = period * 3 / 8;
		next = fReady;
		fReady = began;
		if (fBeat == 0 || began - next > period * 3 / 2) {
			fBeat = began;
		} else {
			for (next = fBeat + period; next < began; next += period) {
			}
			if (next - began > aim) {
				next -= (next - began - aim) / 8;
			} else {
				next += (aim - (next - began)) / 8;
			}
			clock_delay_until(next);
			fBeat = next;
			fStatOnBeat++;
			fStatBeatWait += mach_absolute_time() - began;
			began = mach_absolute_time();
		}
	}
	shown = began;
	/* its turn has come: the swap after it may be handed over */
	consumeSwap();
	IOLockLock(fLock);
	if (fLastFrame != 0) {
		absolutetime_to_nanoseconds(began - fLastFrame, &nanos);
		nanos /= NSEC_PER_MSEC;
		if (nanos >= FRAME_PAUSE) {
			fFramePauses++;
		} else {
			fFrameTimes[nanos < FRAME_TIMES ? nanos : FRAME_TIMES]++;
			fFrameLongest = nanos > fFrameLongest ? (uint32_t) nanos : fFrameLongest;
		}
	}
	fLastFrame = began;
	report = fSwapsShown < 8;
	if (report) {
		IOLog("MiniIOMobileFramebuffer: swap %u (#%u): surfaces %u %u %u %u, layers set 0x%x cleared 0x%x\n",
		    swapWord(swap, SWAP_ID), fSwapsShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)),
		    swapWord(swap, SWAP_SURFACE(2)), swapWord(swap, SWAP_SURFACE(3)), set, swapWord(swap, SWAP_LAYERS_CLEARED));
	}
	for (unsigned layer = 0; layer < SWAP_LAYERS; layer++) {
		uint32_t sid = swapWord(swap, SWAP_SURFACE(layer));
		int32_t src[4], dst[4];
		MiniIOSurface * s;

		layers |= sid != 0 ? 1u << layer : 0;
		/* the frames' surfaces: is it a few, in turn? */
		for (unsigned i = 0; layer == 0 && sid != 0 && i < 8; i++) {
			if (fStatSurfaces[i] == 0 || fStatSurfaces[i] == sid) {
				fStatSurfaces[i] = sid;
				break;
			}
		}
		if (sid == 0) {
			/* the bottom layer taken away: nothing is shown, the screen is black */
			if (layer == 0 && (set & 1) && fGraphics) {
				fPanelWhole = false;
				for (uint32_t y = 0; y < fPanelHeight; y++) {
					bzero(fBase + (size_t) y * fRowBytes, (size_t) fPanelWidth * 4);
				}
				blanked = true;
			}
			continue;
		}
		memcpy(src, swap + SWAP_SOURCE(layer), sizeof(src));
		memcpy(dst, swap + SWAP_DEST(layer), sizeof(dst));
		s = surfaces[layer];
		if (report) {
			IOLog("MiniIOMobileFramebuffer: swap %u layer %u: surface %u%s (%ux%u '%c%c%c%c') "
			    "from %d,%d %dx%d to %d,%d %dx%d, layers set 0x%x\n", swapWord(swap, SWAP_ID), layer, sid,
			    s ? "" : " MISSING", s ? s->info.width : 0, s ? s->info.height : 0,
			    s ? (int) (s->info.pixelFormat >> 24 & 0xff) : '?', s ? (int) (s->info.pixelFormat >> 16 & 0xff) : '?',
			    s ? (int) (s->info.pixelFormat >> 8 & 0xff) : '?', s ? (int) (s->info.pixelFormat & 0xff) : '?',
			    src[0], src[1], src[2], src[3], dst[0], dst[1], dst[2], dst[3], set);
		}
		if (s == NULL) {
			continue;
		}
		if (s->info.bytesPerElement == 4 && s->info.planeCount == 0) {
			if (!fGraphics) {
				/* the first picture: the kernel's text console must stop drawing over it */
				fGraphics = true;
				PE_initialize_console(NULL, kPEGraphicsMode);
			}
			if (!(gNoCopy && fSwapsShown > 40)) {
				MiniIOSurface * scan = NULL;

				fDamageUse = false;
				if (layer == 0) {
					bool only = true, given, whole;
					int32_t box[4];

					for (unsigned other = 1; other < SWAP_LAYERS; other++) {
						only = only && swapWord(swap, SWAP_SURFACE(other)) == 0;
					}
					IOLockLock(gDamageLock);
					given = gDamageGiven;
					whole = gDamageFull;
					memcpy(box, gDamage, sizeof(box));
					gDamageGiven = gDamageFull = false;
					bzero(gDamage, sizeof(gDamage));
					IOLockUnlock(gDamageLock);
					if (gPartial && !fFlipOn && only && given && !whole && fPanelWhole && fSinceFull < FULL_EVERY && fSwapsShown > 8 &&
					    !fWasLost && box[2] > box[0] && box[3] > box[1]) {
						box[0] = box[0] < 0 ? 0 : box[0];
						box[1] = box[1] < 0 ? 0 : box[1];
						box[2] = box[2] > (int32_t) s->info.width ? (int32_t) s->info.width : box[2];
						box[3] = box[3] > (int32_t) s->info.height ? (int32_t) s->info.height : box[3];
						memcpy(fDamageBox, box, sizeof(box));
						fDamageUse = box[2] > box[0] && box[3] > box[1];
					}
					fSinceFull = fDamageUse ? fSinceFull + 1 : 0;
					fPanelWhole = only;
					fWasLost = false;
				} else {
					fPanelWhole = false;    /* (more than the one layer: the panel is not the frame alone) */
				}
				if (layer == 0 && gScanOn) {
					bool only = true;

					for (unsigned other = 1; other < SWAP_LAYERS; other++) {
						only = only && swapWord(swap, SWAP_SURFACE(other)) == 0;
					}
					IOLockLock(gDamageLock);
					if (only && gScanFresh && gScan != NULL &&
					    (src[2] <= 0 || (src[0] == 0 && src[1] == 0 && src[2] == (int32_t) s->info.width && src[3] == (int32_t) s->info.height)) &&
					    (dst[2] <= 0 || (dst[0] == 0 && dst[1] == 0 && dst[2] == (int32_t) fWidth && dst[3] == (int32_t) fHeight))) {
						scan = gScan;
						gScanFresh = false;
					}
					IOLockUnlock(gDamageLock);
				}
				if (layer == 0 && gTouchLat) {
					redwood_tl("tl: d %u partial %u box %d,%d,%d,%d whole %u since %u\n", swapWord(swap, SWAP_ID), fDamageUse ? 1u : 0u,
					    fDamageBox[0], fDamageBox[1], fDamageBox[2], fDamageBox[3], fPanelWhole ? 1u : 0u, fSinceFull);
				}
				if (scan != NULL) {
					MiniIOSurface * one[1] = { scan };

					uint64_t w0 = mach_absolute_time(), w1, w2;
					bool got = MiniIOSurfaceBeginDisplay(one, 1, SWAP_GPU_MS);

					w1 = mach_absolute_time();
					if (got) {
						copyScan(scan);
						w2 = mach_absolute_time();
						MiniIOSurfaceEndDisplay(one, 1);
						if (gTouchLat) {
							redwood_tl("tl: s2 wait_for_gpu %llu copy %llu\n", micros(w1) - micros(w0), micros(w2) - micros(w1));
						}
						fPanelWhole = true;
						fSinceFull = 0;
					} else {
						drawLayer(s, src, dst);
					}
				} else {
					drawLayer(s, src, dst);
				}
				fDamageUse = false;
			}
			if (layer == 0) {
				recorded = s;   /* (recorded once the frame is in the panel's memory: see below) */
			}
		}
	}
	drawn = mach_absolute_time();
	if (recorded != NULL) {
		record(recorded);
	}
	if (fFlipOn && (recorded != NULL || blanked)) {
		if (fFlips == 0) {
			flipLog("MiniIOMobileFramebuffer: fb_flip: the first flip\n");
		}
		fSwapDrawing = swapWord(swap, SWAP_ID);
		flipPresent();
	}
	MiniIOSurfaceEndDisplay(surfaces, SWAP_LAYERS);
	fSwapsShown++;
	if (gTouchLat) {
		redwood_tl("tl: v %u ready %llu shown %llu done %llu\n", swapWord(swap, SWAP_ID), micros(ready), micros(shown),
		    micros(drawn));
	}
	if (layers != fLayersShown) {
		IOLog("MiniIOMobileFramebuffer: swap #%u: surfaces in layers 0x%x now (0x%x before): %u %u %u %u\n", fSwapsShown, layers,
		    fLayersShown, swapWord(swap, SWAP_SURFACE(0)), swapWord(swap, SWAP_SURFACE(1)), swapWord(swap, SWAP_SURFACE(2)),
		    swapWord(swap, SWAP_SURFACE(3)));
		fLayersShown = layers;
	}

	/* every five seconds in which something was drawn: how many frames, and what each cost here */
	fStatFrames++;
	fStatDrawing += mach_absolute_time() - began;
	if (fStatSince == 0) {
		fStatSince = began;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - fStatSince, &nanos);
	if (nanos >= 5 * NSEC_PER_SEC) {
		uint64_t drawing, waiting, beat;
		unsigned different;

		absolutetime_to_nanoseconds(fStatDrawing, &drawing);
		absolutetime_to_nanoseconds(fStatWaiting, &waiting);
		absolutetime_to_nanoseconds(fStatBeatWait, &beat);
		(gNativeProbe != 0 ? flipLog : IOLog)("MiniIOMobileFramebuffer: %u frames in %llu ms (%llu a second), %llu us each in the driver, after %llu us "
		    "waiting for the GPU and, %u of them, %llu us for the next refresh; %u swaps waited for the frame before, "
		    "%u gave up; %u swaps came a refresh late themselves\n", fStatFrames, nanos / NSEC_PER_MSEC,
		    fStatFrames * NSEC_PER_SEC / nanos, drawing / fStatFrames / NSEC_PER_USEC,
		    waiting / fStatFrames / NSEC_PER_USEC, fStatOnBeat, fStatOnBeat ? beat / fStatOnBeat / NSEC_PER_USEC : 0,
		    fStatHeld, fStatLost, fStatSwapsLate);
		fStatSwapsLate = 0;
		for (different = 0; different < 8 && fStatSurfaces[different] != 0; different++) {
		}
		if (different == 8 && fStatFrames > 8) {
			uint32_t made, freed, again;
			uint64_t making, freeing;

			/* (CoreAnimation has made itself a new surface for every frame: see "the frames' surfaces" above) */
			MiniIOSurfaceCost(&made, &making, &freed, &freeing, &again);
			IOLog("MiniIOMobileFramebuffer: the frames came in surfaces %u, %u, %u, %u, %u and on, none twice; in that time "
			    "%u surfaces of all kinds were made, in %llu us each (%u of them of memory kept from one before), and %u "
			    "freed, in %llu us each\n", fStatSurfaces[0], fStatSurfaces[1], fStatSurfaces[2], fStatSurfaces[3],
			    fStatSurfaces[4], made, making, again, freed, freeing);
		}
		bzero(fStatSurfaces, sizeof(fStatSurfaces));
		fStatHeld = fStatLost = fStatOnBeat = 0;
		fStatWaiting = fStatBeatWait = 0;
		reportFrameTimes();
		if (fDiff) {
			IOLog("MiniIOMobileFramebuffer: %llu pixels of the panel changed a frame, all within %u,%u to %u,%u\n",
			    fDiffPixels / fStatFrames, fDiffLeft, fDiffTop, fDiffRight, fDiffBottom);
			fDiffPixels = 0;
			fDiffLeft = fDiffTop = ~0u;
			fDiffRight = fDiffBottom = 0;
		}
		fStatFrames = 0;
		fStatDrawing = 0;
		fStatSince = 0;
	}
	IOLockUnlock(fLock);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

class IOMobileFramebufferUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

	task_t                  fTask;
	IOMobileFramebuffer *   fOwner;
	IOLock *                fLock;
	mach_port_t             fPorts[kFBNotifyTypes];
	mach_vm_address_t       fCallbacks[kFBNotifyTypes];
	io_user_reference_t     fRefcons[kFBNotifyTypes];
	thread_call_t           fVSyncCall;
	uint64_t                fVSyncCount;
	uint64_t                fVSyncTime;             /* when the refresh being waited for is due */
	bool                    fVSyncRunning;
	bool                    fClosed;
	uint32_t                fFrameInfoMask;         /* the frame-info keys asked for (73): bit 0 Presentation_time */
	uint32_t                fFrameInfoShown;        /* the last swap reported as presented */
	uint32_t                fFrameInfoSent;

	bool wantsTicks(void) const;

	static void vsyncFired(thread_call_param_t self, thread_call_param_t unused);
	void vsync(void);
	void armVSync(void);

public:
	virtual bool initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService * provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments * args,
	    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void * securityID, UInt32 type, OSDictionary * properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService * provider)
{
	fOwner = OSDynamicCast(IOMobileFramebuffer, provider);
	if (fOwner == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fVSyncCall = thread_call_allocate(&IOMobileFramebufferUserClient::vsyncFired, this);
	return fLock != NULL && fVSyncCall != NULL;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fLock != NULL) {
		IOLockLock(fLock);
		fClosed = true;
		IOLockUnlock(fLock);
	}
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
	}
	if (!isInactive()) {
		terminate();
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::free(void)
{
	if (fVSyncCall != NULL) {
		thread_call_cancel_wait(fVSyncCall);
		thread_call_free(fVSyncCall);
		fVSyncCall = NULL;
	}
	for (unsigned i = 0; i < kFBNotifyTypes; i++) {
		if (fPorts[i] != NULL) {
			releaseNotificationPort(fPorts[i]);
			fPorts[i] = NULL;
		}
	}
	if (fLock != NULL) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOUserClient::free();
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, UInt32 type, io_user_reference_t refCon)
{
	mach_port_t old;

	IOLog("MiniIOMobileFramebuffer: notification port for type %u\n", (unsigned) type);
	if (type >= kFBNotifyTypes) {
		return kIOReturnBadArgument;
	}
	IOLockLock(fLock);
	old = fPorts[type];
	fPorts[type] = port;
	IOLockUnlock(fLock);
	if (old != NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

void
IOMobileFramebufferUserClient::vsyncFired(thread_call_param_t self, thread_call_param_t unused)
{
	((IOMobileFramebufferUserClient *) self)->vsync();
}

/* With the lock held. */
void
IOMobileFramebufferUserClient::armVSync(void)
{
	uint64_t period, now = mach_absolute_time();

	/* one refresh after the last, not after whenever we got round to this: the rate does not drift */
	nanoseconds_to_absolutetime(NSEC_PER_SEC / refreshHz(), &period);
	{
		uint64_t pulse, offset;

		if (teLast(&pulse)) {
			/* the first moment after now that is `offset` after a pulse (the pulses come every `period`) */
			nanoseconds_to_absolutetime((uint64_t) gTeOffUs * 1000, &offset);
			fVSyncTime = pulse + offset;
			while (fVSyncTime <= now + period / 8) {
				fVSyncTime += period;
			}
			thread_call_enter_delayed(fVSyncCall, fVSyncTime);
			return;
		}
	}
	fVSyncTime += period;
	if (fVSyncTime <= now || fVSyncTime > now + 2 * period) {
		fVSyncTime = now + period;
	}
	thread_call_enter_delayed(fVSyncCall, fVSyncTime);
}

/* With the lock held: the timer has a client for its ticks -- vsync notifications, or frame info with a key asked for. */
bool
IOMobileFramebufferUserClient::wantsTicks(void) const
{
	if (fClosed) {
		return false;
	}
	return (fCallbacks[kFBNotifyVSync] != 0 && fPorts[kFBNotifyVSync] != NULL) ||
	       (fCallbacks[kFBNotifyFrameInfo] != 0 && fPorts[kFBNotifyFrameInfo] != NULL && (fFrameInfoMask & 1) != 0);
}

/*
 * One refresh. Vsync: the client gets three 64-bit numbers as six 32-bit halves -- the time of this
 * refresh, the same again (the time it was presented), and a counter. Frame info, when a frame the
 * client has not been told of is on the panel: [the mask of the values, top bit = each value is two
 * 32-bit halves; the swap ID; Presentation_time low, high] (IOMobileFramebuffer's FrameInfoNotifyFunc,
 * EXP-0153) -- the time is the tearing-effect pulse the DPU took it at, the latest before now.
 */
void
IOMobileFramebufferUserClient::vsync(void)
{
	OSAsyncReference64 ref, infoRef;
	io_user_reference_t args[6], info[4];
	uint64_t now, pulse;
	uint32_t shown;
	bool send = false, sendInfo = false;

	IOLockLock(fLock);
	now = fVSyncTime;
	if (!wantsTicks()) {
		fVSyncRunning = false;
	} else {
		if (fCallbacks[kFBNotifyVSync] != 0 && fPorts[kFBNotifyVSync] != NULL) {
			fVSyncCount++;
			setAsyncReference64(ref, fPorts[kFBNotifyVSync], fCallbacks[kFBNotifyVSync], fRefcons[kFBNotifyVSync], fTask);
			args[0] = (uint32_t) now;
			args[1] = now >> 32;
			args[2] = (uint32_t) now;
			args[3] = now >> 32;
			args[4] = (uint32_t) fVSyncCount;
			args[5] = fVSyncCount >> 32;
			send = true;
		}
		if (fCallbacks[kFBNotifyFrameInfo] != 0 && fPorts[kFBNotifyFrameInfo] != NULL && (fFrameInfoMask & 1) != 0 &&
		    fOwner->shownSwap(&shown) && shown != fFrameInfoShown && teLast(&pulse)) {
			fFrameInfoShown = shown;
			setAsyncReference64(infoRef, fPorts[kFBNotifyFrameInfo], fCallbacks[kFBNotifyFrameInfo],
			    fRefcons[kFBNotifyFrameInfo], fTask);
			info[0] = 0x80000001u;
			info[1] = shown;
			info[2] = (uint32_t) pulse;
			info[3] = pulse >> 32;
			sendInfo = true;
			fFrameInfoSent++;
		}
		armVSync();
	}
	IOLockUnlock(fLock);
	if (send) {
		sendAsyncResult64WithOptions(ref, kIOReturnSuccess, args, 6, kIOUserNotifyOptionCanDrop);
	}
	if (sendInfo) {
		sendAsyncResult64WithOptions(infoRef, kIOReturnSuccess, info, 4, kIOUserNotifyOptionCanDrop);
		if (fFrameInfoSent == 1 || fFrameInfoSent % 600 == 0) {
			flipLog("MiniIOMobileFramebuffer: frame info: %u frames reported (swap %u at %llu)\n", fFrameInfoSent, shown,
			    pulse);
		}
	}
}

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments * args,
    IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	const uint64_t * in = args->scalarInput;
	uint64_t * out = args->scalarOutput;
	MiniIOSurface * s;

	if (selector < 128 && gFBCalls[selector] < gFBTrace) {
		char procname[32] = "?";

		gFBCalls[selector]++;
		proc_selfname(procname, sizeof(procname));
		char head[60] = "";

		if (args->structureInput != NULL && args->structureInputSize != 0) {
			const uint8_t * b = (const uint8_t *) args->structureInput;
			unsigned n = args->structureInputSize < 16 ? args->structureInputSize : 16, at = 0;

			at += snprintf(head + at, sizeof(head) - at, ":");
			for (unsigned k = 0; k < n && at + 4 < sizeof(head); k++) {
				at += snprintf(head + at, sizeof(head) - at, " %02x", b[k]);
			}
		}
		flipLog("MiniIOMobileFramebuffer: %s[%d] selector %u (scalars %u in [0x%llx 0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)%s\n", procname, proc_selfpid(), selector, args->scalarInputCount,
		    args->scalarInputCount > 0 ? in[0] : 0, args->scalarInputCount > 1 ? in[1] : 0,
		    args->scalarInputCount > 2 ? in[2] : 0, args->scalarInputCount > 3 ? in[3] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize, head);
	}

	switch (selector) {
	case kFBMethodGetDisplaySize:                   /* out: width, height */
		if (args->scalarOutputCount < 2) {
			return kIOReturnBadArgument;
		}
		out[0] = fOwner->fWidth;
		out[1] = fOwner->fHeight;
		args->scalarOutputCount = 2;
		return kIOReturnSuccess;

	case kFBMethodGetID:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetDotPitch:                      /* out: pixels per inch */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = kFBDotsPerInch * fOwner->fWidth / kFBDefaultWidth;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodGetLayerDefaultSurface:           /* in: size wanted (or 0, 0); out: a send right for the surface */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		s = fOwner->defaultSurface();
		if (s == NULL) {
			return kIOReturnNoMemory;
		}
		out[0] = MiniIOSurfaceMakeSendRight(fTask, s);
		args->scalarOutputCount = 1;
		return out[0] != 0 ? kIOReturnSuccess : kIOReturnNoMemory;

	case kFBMethodSwapBegin:                        /* out: the number of the new swap */
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = OSIncrementAtomic(&fOwner->fSwapID) + 1;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	case kFBMethodSwapEnd:
		if (args->structureInput == NULL || args->structureInputSize < SWAP_SIZE) {
			IOLog("MiniIOMobileFramebuffer: swap request of %u bytes, expected %u\n", args->structureInputSize, SWAP_SIZE);
			return kIOReturnBadArgument;
		}
		fOwner->present((const uint8_t *) args->structureInput);
		return kIOReturnSuccess;

	case kFBMethodSwapWait:                         /* a swap is on the screen as soon as it is made */
		if (gSwapWait != 0 && fOwner->fFlipOn && args->scalarInputCount > 0) {
			uint32_t id = (uint32_t) in[0];

			/* taken: its frame (or a later one, which took the place of a frame not yet taken) latched by the DPU */
			for (int i = 0; i < 40; i++) {
				IOMobileFramebuffer * o = fOwner;

				if ((int32_t) (o->fSwapLatched - id) >= 0 ||
				    ((int32_t) (o->fSwapPending - id) >= 0 && (o->fDpuCtl[0x18 / 4] & 1) == 0)) {
					break;
				}
				IOSleep(1);
			}
		}
		if (gTouchLat) {
			redwood_tl("tl: w %llu %#llx %#llx at %llu\n", args->scalarInputCount > 0 ? in[0] : 0,
			    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, micros(mach_absolute_time()));
		}
		return kIOReturnSuccess;
	case kFBMethodSwapSignal:
	case kFBMethodSwapCancel:
	case kFBMethodSwapCancelAll:
	case kFBMethodRequestPowerChange:
		return kIOReturnSuccess;

	case kFBMethodChangeFrameInfo:                  /* in: notification type (3), mask of the keys wanted */
		if (args->scalarInputCount >= 2 && in[0] == kFBNotifyFrameInfo) {
			IOLockLock(fLock);
			fFrameInfoMask = gFrameInfo != 0 ? (uint32_t) in[1] & 1 : 0;
			if (wantsTicks() && !fVSyncRunning) {
				fVSyncRunning = true;
				armVSync();
			}
			IOLockUnlock(fLock);
			flipLog("MiniIOMobileFramebuffer: frame info: keys %#llx asked for\n", in[1]);
		}
		return kIOReturnSuccess;

	case kFBMethodNotifications:                    /* in: callback (0 = off), its argument, type, 0 */
		if (args->scalarInputCount < 3 || in[2] >= kFBNotifyTypes) {
			return kIOReturnBadArgument;
		}
		IOLockLock(fLock);
		fCallbacks[in[2]] = in[0];
		fRefcons[in[2]] = in[1];
		if (wantsTicks() && !fVSyncRunning) {
			fVSyncRunning = true;
			armVSync();
		}
		IOLockUnlock(fLock);
		return kIOReturnSuccess;

	case kFBMethodSupportedFrameInfo:               /* in: which key; out: how many keys, and that one's name (64 bytes) */
		if (args->scalarOutputCount >= 1) {
			out[0] = gFrameInfo != 0 ? 1 : 0;
			args->scalarOutputCount = 1;
		}
		if (args->structureOutput != NULL) {
			bzero(args->structureOutput, args->structureOutputSize);
			if (gFrameInfo != 0 && args->scalarInputCount >= 1 && in[0] == 0) {
				strlcpy((char *) args->structureOutput, "Presentation_time", args->structureOutputSize);
			}
		}
		return gFrameInfo == 0 || (args->scalarInputCount >= 1 && in[0] == 0) ? kIOReturnSuccess : kIOReturnBadArgument;

	case kFBMethodGetBlock:                         /* in: which block; out: its bytes */
		if (args->structureOutput == NULL) {
			return kIOReturnBadArgument;
		}
		bzero(args->structureOutput, args->structureOutputSize);
		if (args->scalarInputCount >= 1 && in[0] == 0x40 && args->structureOutputSize >= 12) {
			/* the current mode: its colour and timing identifiers */
			uint32_t ids[2] = { kFBColorID, kFBTimingID };

			memcpy((uint8_t *) args->structureOutput + 4, ids, sizeof(ids));
		}
		return kIOReturnSuccess;

	case kFBMethodGetCurrentAbsoluteTime:
		if (args->scalarOutputCount < 1) {
			return kIOReturnBadArgument;
		}
		out[0] = mach_absolute_time();
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;

	default:
		break;
	}

	if (selector < 128 && !(gFBLogged[selector / 64] & (1ULL << (selector % 64)))) {
		gFBLogged[selector / 64] |= 1ULL << (selector % 64);
		flipLog("MiniIOMobileFramebuffer: selector %u not implemented (scalars %u in [0x%llx 0x%llx 0x%llx], %u out; "
		    "structure %u in, %u out)\n", selector, args->scalarInputCount, args->scalarInputCount > 0 ? in[0] : 0,
		    args->scalarInputCount > 1 ? in[1] : 0, args->scalarInputCount > 2 ? in[2] : 0, args->scalarOutputCount,
		    args->structureInputSize, args->structureOutputSize);
	}
	/* a request that only sets something is accepted; one that wants an answer gets zeros */
	if (args->scalarOutputCount != 0) {
		bzero(out, args->scalarOutputCount * sizeof(out[0]));
	}
	if (args->structureOutput != NULL && args->structureOutputSize != 0) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	return kIOReturnSuccess;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * IOMobileFramebuffer.framework counts the displays it should wait for by looking for
 * children of /arm-io in the device tree whose device_type is "display-subsystem".
 */
static void
publishDisplayNode(void)
{
	IORegistryEntry * armio = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
	IORegistryEntry * node;
	OSDictionary * props;
	OSData * data;

	if (armio == NULL) {
		IOLog("MiniIOMobileFramebuffer: no /arm-io in the device tree\n");
		return;
	}
	node = OSTypeAlloc(IORegistryEntry);
	props = OSDictionary::withCapacity(4);
	if (node != NULL && props != NULL) {
		data = OSData::withBytes("display-subsystem", sizeof("display-subsystem"));
		props->setObject("device_type", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("redwood,framebuffer", sizeof("redwood,framebuffer"));
		props->setObject("compatible", data);
		OSSafeReleaseNULL(data);
		data = OSData::withBytes("disp0", sizeof("disp0"));
		props->setObject("name", data);
		OSSafeReleaseNULL(data);
		if (node->init(props)) {
			node->setName("disp0");
			node->attachToParent(armio, gIODTPlane);
		}
	}
	OSSafeReleaseNULL(props);
	OSSafeReleaseNULL(node);
	armio->release();
}

/*
 * The graphics accelerator. There is none, but CoreAnimation waits half a minute for a
 * service of this class to appear before it looks for a GPU at all (once per process). An
 * accelerator without the properties that name its Metal and OpenGL ES drivers is passed
 * over by both, which leaves CoreAnimation with its software renderer, as before.
 *
 * With boot-arg metal=1 a user process may put those two properties here, which is the whole
 * of what keeps Metal from finding our driver; see setProperties below, and docs/metal.md 10.31
 * (10.14 for the measurement that made it a boot-arg).
 */

/* Metal's step 2 reads exactly these two, in this order, off this service (docs/metal.md 1). */
#define kAcceleratorPluginNameKey      "MetalPluginName"
#define kAcceleratorPluginClassNameKey "MetalPluginClassName"

static bool gAcceleratorTakesProperties;        /* boot-arg metal=1 */

class IOAcceleratorES : public IOService
{
	OSDeclareDefaultStructors(IOAcceleratorES);

public:
	virtual IOReturn setProperties(OSObject * properties) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

/*
 * Switch Metal's own device discovery on, from user space: IORegistryEntrySetCFProperties() with
 * MetalPluginName and MetalPluginClassName. Metal reads the first (getMetalPluginClassForService,
 * 0x181a39ba4), builds "/System/Library/Extensions/" + it + ".bundle", opens that with NSBundle
 * and takes the class the second names; with the first missing it returns nil before it even
 * reads the second, and that single `cbz` is where the chain breaks on this port today.
 *
 * This method is the only thing that was in the way. IORegistryEntry::setProperties is
 * "return kIOReturnUnsupported" (IORegistryEntry.cpp) and IOService does not override it; the
 * MAC check in front of the call has no policy to ask, because this kernel registers no iokit
 * hook (bsd/kern/kern_minimac.c); and this service carries no property whitelist.
 *
 * Why it is gated on a boot-arg, which is a measured risk and not caution in general
 * (docs/metal.md 10.14): the conditions Metal puts on a device object are asserts, not soft
 * refusals (10.2-10.3). A process that asks Metal and is handed a half-made device does not
 * degrade to another renderer -- it aborts ("Assertion failed: ([self
 * conformsToProtocol:@protocol(MTLDeviceSPI)])", "failed assertion _limits.maxColorAttachments
 * > 0"), and an aborting daemon is restarted by launchd, which is a loop. Published instead at
 * registerService() time, the properties would exist before launchd, backboardd and SpringBoard
 * are born, so each of the three would take the Metal road on its first call, with nobody left
 * to read the log. Behind a boot-arg, a machine that has them is a machine booted on purpose to
 * have them, and the first such boot is meant to be RC=rc.ios, where launchd is never born.
 *
 * An empty string removes a property rather than setting it. That is the way out of the loop
 * above without a reboot: the properties live only in the registry, so processes started after
 * they are gone go back to the software road.
 *
 * Everything else is refused, and refused whole, before anything is set: every process on the
 * machine can reach this method and the only thing it exists for is this one switch.
 */
IOReturn
IOAcceleratorES::setProperties(OSObject * properties)
{
	OSDictionary * dict = OSDynamicCast(OSDictionary, properties);
	OSCollectionIterator * keys;
	OSObject * next;
	char who[32] = "?";

	if (!gAcceleratorTakesProperties) {
		static unsigned said;

		if (said++ < 4) {
			proc_selfname(who, sizeof(who));
			IOLog("IOAcceleratorES: %s asks to set properties on it; boot-arg metal=1 was not given\n", who);
		}
		return kIOReturnUnsupported;
	}
	if (dict == NULL || dict->getCount() == 0) {
		return kIOReturnBadArgument;
	}
	keys = OSCollectionIterator::withCollection(dict);
	if (keys == NULL) {
		return kIOReturnNoMemory;
	}
	proc_selfname(who, sizeof(who));
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);

		if (key == NULL || (!key->isEqualTo(kAcceleratorPluginNameKey) &&
		    !key->isEqualTo(kAcceleratorPluginClassNameKey))) {
			IOLog("IOAcceleratorES: refusing what %s sent: only " kAcceleratorPluginNameKey " and "
			    kAcceleratorPluginClassNameKey " are taken here\n", who);
			keys->release();
			return kIOReturnUnsupported;
		}
		if (OSDynamicCast(OSString, dict->getObject(key)) == NULL) {
			IOLog("IOAcceleratorES: refusing \"%s\" from %s: Metal reads it as a CFString\n",
			    key->getCStringNoCopy(), who);
			keys->release();
			return kIOReturnBadArgument;
		}
	}
	keys->reset();
	while ((next = keys->getNextObject()) != NULL) {
		const OSSymbol * key = OSDynamicCast(OSSymbol, next);
		OSString * value = OSDynamicCast(OSString, dict->getObject(key));

		if (value->getLength() == 0) {
			removeProperty(key);
			IOLog("IOAcceleratorES: %s removed by %s\n", key->getCStringNoCopy(), who);
		} else if (setProperty(key, value)) {
			IOLog("IOAcceleratorES: %s = %s, set by %s\n", key->getCStringNoCopy(),
			    value->getCStringNoCopy(), who);
		} else {
			IOLog("IOAcceleratorES: %s could not be set\n", key->getCStringNoCopy());
			keys->release();
			return kIOReturnNoMemory;
		}
	}
	keys->release();
	return kIOReturnSuccess;
}

static void
publishAccelerator(IOService * provider)
{
	IOAcceleratorES * accelerator = OSTypeAlloc(IOAcceleratorES);
	uint32_t metal = 0;

	gAcceleratorTakesProperties = PE_parse_boot_argn("metal", &metal, sizeof(metal)) && metal != 0;
	if (accelerator != NULL && accelerator->init() && accelerator->attach(provider)) {
		accelerator->setName("sgx");
		accelerator->registerService();
		/* the first line to look for in the log of such a boot: the gate is open, nothing is set yet */
		if (gAcceleratorTakesProperties) {
			IOLog("IOAcceleratorES: metal=%u: this service takes " kAcceleratorPluginNameKey
			    " and " kAcceleratorPluginClassNameKey " from user space\n", metal);
		}
	}
	OSSafeReleaseNULL(accelerator);
}

/*
 * A picture from inside the kernel (the GPU driver's first ones): rows of 32-bit pixels, put at
 * the top left corner of the panel as they are and cut to its size.
 */
extern "C" void
MiniIOMobileFramebufferShow(const uint8_t * pixels, uint32_t width, uint32_t height, uint32_t rowBytes)
{
	uint32_t columns = width < vinfo.v_width ? width : (uint32_t) vinfo.v_width;
	uint32_t rows = height < vinfo.v_height ? height : (uint32_t) vinfo.v_height;

	if (vinfo.v_baseaddr == 0 || vinfo.v_depth != 32) {
		return;
	}
	for (uint32_t y = 0; y < rows; y++) {
		memcpy((void *) (vinfo.v_baseaddr + (size_t) y * vinfo.v_rowbytes), pixels + (size_t) y * rowBytes,
		    (size_t) columns * 4);
	}
}

void
MiniIOMobileFramebufferStart(IOService * provider)
{
	IOMobileFramebuffer * fb = OSTypeAlloc(IOMobileFramebuffer);

	if (fb == NULL || !fb->init() || !fb->attach(provider)) {
		OSSafeReleaseNULL(fb);
		return;
	}
	if (fb->start(provider)) {
		publishDisplayNode();
		publishAccelerator(provider);
	} else {
		fb->detach(provider);
	}
	fb->release();
}
