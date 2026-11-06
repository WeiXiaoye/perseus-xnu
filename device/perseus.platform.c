/*
 * Xiaomi Mi MIX 3 (perseus, SDM845): the loader's description of the SoC.
 * Values from the sdm845 device tree and from the running Android firmware.
 *
 * Matched by the framebuffer the firmware reports (GOP at 0x9d400000), the same
 * way redwood is identified.
 */
static const struct platform platform_perseus = {
	/* io_base must be non-zero: XNU treats a zero SoC base as "no SoC". */
	.name = "perseus", .io_base = 0x00400000, .io_size = 0x1C000000,
	.gic_dist = 0x17A00000, .gic_redist = 0x17A60000, .uart = 0, .log_other_cpu = 1,
	.cores = 8, .core_shift = 8,
	/*
	 * SDM845: cores 0-3 are Cortex-A75 (2.8 GHz), 4-7 Cortex-A55 (1.8 GHz).
	 * XNU uses the small ones only when the big ones are busy
	 * (ml_choose_slow_core_partners()).
	 */
	.small_cores = 0xF0,
	.fb_base = 0x9d400000,
};
