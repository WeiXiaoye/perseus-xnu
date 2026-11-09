# GPU on the MIX 3 (perseus): the state of the work

This branch is the port as it stands: the Adreno 630 is woken, authenticated and drawing for
CoreAnimation, and the picture reaches this phone's panel. It is not yet fast or tear-free; what
follows is what is known to work, what was measured, and what is left.

## What works, and the evidence for it

| Step | Log line that proves it |
| --- | --- |
| CX half of the GPU block switched on | `mi8gpu: CX GDSCR ... hardware control ... (on)` |
| GMU's registers answer, 19.2 MHz counter runs | `the GMU's counter counts at 19200272 Hz: THE GPU BLOCK IS ALIVE` |
| GMU memory takes what is written | `the GMU's memory TAKES WHAT IS WRITTEN` |
| TrustZone authenticates the zap shader | `PAS_AUTH_AND_RESET: rc 0 -- ZAP AUTHENTICATED` |
| The 3D core leaves secure mode | `SECVID_TRUST_CNTL after: 0 (NON-SECURE MODE)` |
| The CP takes work | `CP CONSUMED the ring: RB_RPTR ... WPTR ...` |
| The GPU is kept for the system | `THE GPU STAYS ON for the system to draw with` |
| CoreAnimation draws through freedreno | `redwood-gl: frame 1: 136 GL calls, 4 draws, 8 triangles` |
| The work reaches the GPU | `mi8gpu: submission 1 (fence 1): 4 buffers ...` |

## The configuration that works on this phone

1. **The Mi 8 driver** (`bsd/dev/perseus_adreno.c`, with `perseus_a630_hw_init.inc`) behind the
   accelerator service, plus its hooks in `redwood_adreno.c`, `RedwoodGPU.cpp` and
   `MiniIOSurface.cpp`.
2. **The Mi 8 display driver** (`MiniIOMobileFramebuffer.cpp`). It is not optional: the accelerator's
   `RedwoodGPU.cpp` calls `MiniIOScan()` and `MiniIODamage()`, which live there.
3. **Start the driver from a kernel thread** (`A630_START_IN_THREAD` in `perseus_adreno.c`). On this
   phone TrustZone answers the zap's `PAS_INIT_IMAGE` with "interrupted" every time when the call is
   made inline from the boot thread during `bsd_init`, and with success when it is made from a kernel
   thread. `mi8gpu=21` proved the difference; `gpu=1` now starts the same way.
4. **Presentation by CPU copy.** The boot loader leaves the panel scanning a 1080x2340 framebuffer at
   `0x9d400000` with a stride of 4352 bytes, and nothing here may reprogram the display controller.
   A swap therefore copies the composed frame into that memory.

Boot arguments in use:

```
gpu=1 mi8gpu_delay=3 mi8gpu_gmuclk=1 mi8gpu_zap_keep=1 sb_fps_clamp=1
```

`mi8gpu_delay=3` is ours: it gives the loader's own SCM traffic time to finish before the steps run.

## Why not the Mi 8's own display settings

The Mi 8 runs `fb_flip=1 fb_native=1`, which allocates three 1125x2436 buffers and expects the
display controller to read them and scale. On this phone the controller keeps reading the boot
loader's 1080x2340 buffer whatever is written to those, so the screen shows a black border (the
picture is at the Mi 8's aspect) or, worse, nothing. Those two arguments must stay off here.

## What was measured, and what was fixed

The frame log (`redwood-gl: frame N: ... <t> us ... <u> us before it for the IOSurfaces`) showed the
GL work itself at ~2.3 ms a frame but **35-40 ms "for the IOSurfaces"**, which put the whole system
at 15-20 frames a second. That time was the per-pixel reorder in the userland's
`surface_to_texture()`: an iOS surface is `A,B,G,R` in memory and the texture wants `B,G,R,A`, and
the loop moved the four bytes one at a time, 2.5 million times a frame. It is one 32-bit rotation of
the pixel; see `userland/0001-eagl-word-rotate.patch`.

## The tearing, and the ways out

The frame is copied into the framebuffer with nothing tying the copy to the panel's scan. A copy
that begins in the middle of a frame is therefore seen half-old and half-new: the top of the screen
is one picture and the bottom is the one before it.

The Mi 8's own answer is `fb_flip=1 fb_telock=1`: the swap rides the panel's tearing-effect pulse.
That pulse is available here (the driver reads it at `0x0ae70000` and logs "the vsync follows the
tearing-effect pulse" when asked), but `fb_telock` only applies to the native flip path, and that
path cannot work on this phone -- the panel keeps reading the boot loader's buffer at `0x9d400000`.
So the copy has to be timed by hand. This phone does expose what is needed: the display controller's
line counter, `INTF_LINE_COUNT`, which the driver already reads when it reports the scan.

Three ways, in order of what they cost to try:

| | How | What it costs |
| --- | --- | --- |
| **A. copy during the vertical blank** | wait for the line counter to wrap (the frame's end) and then copy | simple, and the whole copy must fit in the blanking interval: 10 MB of frame in ~16 ms needs ~600 MB/s, which the copy may just manage; otherwise the first lines of the next frame get the tail |
| **B. chase the scan** | copy from the line the panel is reading *now*, so that every line is written before the panel reaches it, a frame's worth of lag behind | no extra delay in what is shown, but the writer has to keep up with the scanner line for line, and the check is the line counter against the row being written |
| **C. queue of frames** | three framebuffers, present the next one, drop the oldest when the producer outruns the panel | smoothest under load, most memory (2 x 10 MB) and a second thing that can go wrong |

A is the first thing to try: it is one condition at the start of the existing copy loop, it cannot
make the picture worse than it is now, and it says at once whether the panel's scan can be followed
at all -- which B and C both depend on too.

## What is left

* **Frame rate**: measure again after the reorder fix; if the upload is still the cost, the next step
  is to swizzle four pixels at a time (NEON `vrev32`), or to hand the pixels to the GPU untransformed
  and let the sampler read them wrapped.
* **Frame rate**: measure again after the reorder fix; if the upload is still the cost, the next step
  is to swizzle four pixels at a time (NEON `vrev32`), or to hand the pixels to the GPU untransformed
  and let the sampler read them wrapped.
* **Fences**: submissions are reported to the CP and the ring is consumed; whether every fence comes
  back is not yet measured over a long run.
