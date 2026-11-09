/* SPDX-License-Identifier: GPL-2.0-only
 * Linux-derived A630 initialization data and ordering.
 * Sources: Linux v7.2.10 drivers/gpu/drm/msm/adreno/a6xx_gpu.c
 *          and drivers/gpu/drm/msm/adreno/a6xx_catalog.c.
 * Copyright (c) 2017-2019 The Linux Foundation. All rights reserved.
 * Copyright (C) 2013-2014 Red Hat
 * Copyright (c) 2014,2017 The Linux Foundation. All rights reserved.
 * Adapted for dipper by yanei, 2026; source notices added 2026-10-30.
 * See LICENSES/GPL-2.0-only.txt and docs/GPU_LICENSE.md in dipper-xnu.
 * This notice does not resolve licensing of a combination with APSL code.
 */

/*
 * TASK-002, Linux 7.2.10 a630 (0x06030001), SDM845 UBWC profile.
 * reg is a WORD offset from the GPU mapping; only the writer multiplies by 4.
 * Caller must already own GX power/clocks, the GMU OOB vote, and security setup.
 * This is the cold/full HWCG path: RBBM_CLOCK_CNTL != 0x8aa8aa02, CP halted.
 * 推测: that cold-state precondition and the Linux SDM845 profile match this boot.
 * No register read or wait belongs here; true means writes issued, not GPU ready.
 *
 * Required Linux ordering, with table half-open ranges below:
 * 1. Acquire GMU lock/liveness, GX power and OOB_GPU_SET ACK (hw_init:1182-1187).
 * 2. Caller handles SECVID TSB control/base/size (1204,1214-1215), then ADDRESS.
 * 3. Caller handles SECVID address mode (1230), then GMU HWCG mode/delay/hyst
 *    (a6xx_set_hwcg:671-676), reads RBBM_CLOCK_CNTL (697-701).
 *    If it already equals 0x8aa8aa02, skip HWCG and HWCG_COMMIT.
 * 4. Otherwise caller RMW-clears GMU GX_SPTPRAC_CLOCK_CONTROL bit 0 (705),
 *    writes HWCG, RMW-sets that bit (712), then writes HWCG_COMMIT (714).
 * 5. Write CORE; caller then handles GMU busy/power counters (1369-1373).
 * 6. Write PROTECT; install/mask the GPU IRQ path before writing IRQ (1428).
 * 7. Caller does adreno_hw_init (ucode/mappings must already exist), SQE IOVA, ring IOVA/control,
 *    optional RPTR shadow and preemption setup (1431-1469), in that order.
 * 8. Caller writes CP_SQE_CNTL=1, submits a6xx_cp_init and waits a6xx_idle
 *    (1473-1482), then authenticates zap and switches secure mode via CP,
 *    flushing and waiting a6xx_idle (1491-1498). Never guess SECVID_TRUST_CNTL.
 * 9. Caller releases OOB_GPU_SET and legacy BOOT_SLUMBER (1525-1529).
 * a6xx_gmu_wait_for_idle belongs to the caller's GMU/power sequence; it is not
 * called inside this version's hw_init. Do not invent an extra wait in this table.
 *
 * a630_hw_init() replays the static projection only. To preserve the GMU
 * interleavings above, integration must use a630_hw_init_range(), not flatten
 * the nine caller steps around a single a630_hw_init() call. See NOTES.md.
 */

static const struct {
	uint32_t reg;
	uint32_t value;
	const char *name;
} a630_hw_init_table[] = {
	/* ADDRESS: Linux hw_init:1217-1231; SECVID_ADDR_MODE remains caller-owned. */
	{ 0x00842u, 0x00000001u, "REG_A6XX_CP_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1219 hw_init(); a6xx.xml:270 */
	{ 0x00c01u, 0x00000001u, "REG_A6XX_VSC_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1220 hw_init(); a6xx.xml:549 */
	{ 0x08601u, 0x00000001u, "REG_A6XX_GRAS_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1221 hw_init(); a6xx.xml:2215 */
	{ 0x08e05u, 0x00000001u, "REG_A6XX_RB_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1222 hw_init(); a6xx.xml:2823 */
	{ 0x09e01u, 0x00000001u, "REG_A6XX_PC_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1223 hw_init(); a6xx.xml:3430 */
	{ 0x0be05u, 0x00000001u, "REG_A6XX_HLSQ_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1224 hw_init(); a6xx.xml:4963 */
	{ 0x0a601u, 0x00000001u, "REG_A6XX_VFD_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1225 hw_init(); a6xx.xml:3579 */
	{ 0x09601u, 0x00000001u, "REG_A6XX_VPC_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1226 hw_init(); a6xx.xml:3247 */
	{ 0x00e00u, 0x00000001u, "REG_A6XX_UCHE_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1227 hw_init(); a6xx.xml:1201 */
	{ 0x0ae01u, 0x00000001u, "REG_A6XX_SP_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1228 hw_init(); a6xx.xml:4330 */
	{ 0x0b601u, 0x00000001u, "REG_A6XX_TPL1_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1229 hw_init(); a6xx.xml:4541 */
	/* HWCG: Linux a6xx_set_hwcg:705-714; SP clock must already be gated. */
	{ 0x000b0u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:186; a6xx.xml:816 */
	{ 0x000b1u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:187; a6xx.xml:817 */
	{ 0x000b2u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:188; a6xx.xml:818 */
	{ 0x000b3u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:189; a6xx.xml:819 */
	{ 0x000b4u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:190; a6xx.xml:820 */
	{ 0x000b5u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:191; a6xx.xml:821 */
	{ 0x000b6u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:192; a6xx.xml:822 */
	{ 0x000b7u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:193; a6xx.xml:823 */
	{ 0x000b8u, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:194; a6xx.xml:824 */
	{ 0x000b9u, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:195; a6xx.xml:825 */
	{ 0x000bau, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:196; a6xx.xml:826 */
	{ 0x000bbu, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:197; a6xx.xml:827 */
	{ 0x000bcu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:198; a6xx.xml:828 */
	{ 0x000bdu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:199; a6xx.xml:829 */
	{ 0x000beu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:200; a6xx.xml:830 */
	{ 0x000bfu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:201; a6xx.xml:831 */
	{ 0x000c0u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:202; a6xx.xml:832 */
	{ 0x000c1u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:203; a6xx.xml:833 */
	{ 0x000c2u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:204; a6xx.xml:834 */
	{ 0x000c3u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:205; a6xx.xml:835 */
	{ 0x000c4u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:206; a6xx.xml:836 */
	{ 0x000c5u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:207; a6xx.xml:837 */
	{ 0x000c6u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:208; a6xx.xml:838 */
	{ 0x000c7u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:209; a6xx.xml:839 */
	{ 0x000c8u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:210; a6xx.xml:840 */
	{ 0x000c9u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:211; a6xx.xml:841 */
	{ 0x000cau, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:212; a6xx.xml:842 */
	{ 0x000cbu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:213; a6xx.xml:843 */
	{ 0x000ccu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:214; a6xx.xml:844 */
	{ 0x000cdu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:215; a6xx.xml:845 */
	{ 0x000ceu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:216; a6xx.xml:846 */
	{ 0x000cfu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:217; a6xx.xml:847 */
	{ 0x000e0u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:218; a6xx.xml:864 */
	{ 0x000e1u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:219; a6xx.xml:865 */
	{ 0x000e2u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:220; a6xx.xml:866 */
	{ 0x000e3u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:221; a6xx.xml:867 */
	{ 0x000e4u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:222; a6xx.xml:868 */
	{ 0x000e5u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:223; a6xx.xml:869 */
	{ 0x000e6u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:224; a6xx.xml:870 */
	{ 0x000e7u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:225; a6xx.xml:871 */
	{ 0x000e8u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:226; a6xx.xml:872 */
	{ 0x000e9u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:227; a6xx.xml:873 */
	{ 0x000eau, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:228; a6xx.xml:874 */
	{ 0x000ebu, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:229; a6xx.xml:875 */
	{ 0x000ecu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:230; a6xx.xml:876 */
	{ 0x000edu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:231; a6xx.xml:877 */
	{ 0x000eeu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:232; a6xx.xml:878 */
	{ 0x000efu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:233; a6xx.xml:879 */
	{ 0x000d0u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:234; a6xx.xml:848 */
	{ 0x000d1u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:235; a6xx.xml:849 */
	{ 0x000d2u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:236; a6xx.xml:850 */
	{ 0x000d3u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:237; a6xx.xml:851 */
	{ 0x000d4u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:238; a6xx.xml:852 */
	{ 0x000d5u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:239; a6xx.xml:853 */
	{ 0x000d6u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:240; a6xx.xml:854 */
	{ 0x000d7u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:241; a6xx.xml:855 */
	{ 0x000d8u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:242; a6xx.xml:856 */
	{ 0x000d9u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:243; a6xx.xml:857 */
	{ 0x000dau, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:244; a6xx.xml:858 */
	{ 0x000dbu, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:245; a6xx.xml:859 */
	{ 0x000dcu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:246; a6xx.xml:860 */
	{ 0x000ddu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:247; a6xx.xml:861 */
	{ 0x000deu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:248; a6xx.xml:862 */
	{ 0x000dfu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:249; a6xx.xml:863 */
	{ 0x0010bu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_UCHE" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:250; a6xx.xml:903 */
	{ 0x0010cu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_UCHE" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:251; a6xx.xml:904 */
	{ 0x0010du, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_UCHE" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:252; a6xx.xml:905 */
	{ 0x0010eu, 0x00222222u, "REG_A6XX_RBBM_CLOCK_CNTL4_UCHE" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:253; a6xx.xml:906 */
	{ 0x00110u, 0x00000004u, "REG_A6XX_RBBM_CLOCK_HYST_UCHE" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:254; a6xx.xml:908 */
	{ 0x0010fu, 0x00000002u, "REG_A6XX_RBBM_CLOCK_DELAY_UCHE" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:255; a6xx.xml:907 */
	{ 0x000f0u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:256; a6xx.xml:880 */
	{ 0x000f1u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:257; a6xx.xml:881 */
	{ 0x000f2u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:258; a6xx.xml:882 */
	{ 0x000f3u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:259; a6xx.xml:883 */
	{ 0x000f4u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:260; a6xx.xml:884 */
	{ 0x000f5u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:261; a6xx.xml:885 */
	{ 0x000f6u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:262; a6xx.xml:886 */
	{ 0x000f7u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:263; a6xx.xml:887 */
	{ 0x000f8u, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:264; a6xx.xml:888 */
	{ 0x000f9u, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:265; a6xx.xml:889 */
	{ 0x000fau, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:266; a6xx.xml:890 */
	{ 0x000fbu, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:267; a6xx.xml:891 */
	{ 0x00100u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU0" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:268; a6xx.xml:892 */
	{ 0x00101u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU1" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:269; a6xx.xml:893 */
	{ 0x00102u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:270; a6xx.xml:894 */
	{ 0x00103u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU3" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:271; a6xx.xml:895 */
	{ 0x00104u, 0x05022022u, "REG_A6XX_RBBM_CLOCK_CNTL_RAC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:272; a6xx.xml:896 */
	{ 0x00105u, 0x00005555u, "REG_A6XX_RBBM_CLOCK_CNTL2_RAC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:273; a6xx.xml:897 */
	{ 0x00106u, 0x00000011u, "REG_A6XX_RBBM_CLOCK_DELAY_RAC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:274; a6xx.xml:898 */
	{ 0x00107u, 0x00445044u, "REG_A6XX_RBBM_CLOCK_HYST_RAC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:275; a6xx.xml:899 */
	{ 0x00108u, 0x04222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TSE_RAS_RBBM" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:276; a6xx.xml:900 */
	{ 0x00114u, 0x00222222u, "REG_A6XX_RBBM_CLOCK_MODE_GPC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:277; a6xx.xml:912 */
	{ 0x00111u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_MODE_VFD" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:278; a6xx.xml:909 */
	{ 0x0010au, 0x00000000u, "REG_A6XX_RBBM_CLOCK_HYST_TSE_RAS_RBBM" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:279; a6xx.xml:902 */
	{ 0x00116u, 0x04104004u, "REG_A6XX_RBBM_CLOCK_HYST_GPC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:280; a6xx.xml:914 */
	{ 0x00113u, 0x00000000u, "REG_A6XX_RBBM_CLOCK_HYST_VFD" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:281; a6xx.xml:911 */
	{ 0x0011cu, 0x00000000u, "REG_A6XX_RBBM_CLOCK_DELAY_HLSQ" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:282; a6xx.xml:920 */
	{ 0x00109u, 0x00004000u, "REG_A6XX_RBBM_CLOCK_DELAY_TSE_RAS_RBBM" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:283; a6xx.xml:901 */
	{ 0x00115u, 0x00000200u, "REG_A6XX_RBBM_CLOCK_DELAY_GPC" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:284; a6xx.xml:913 */
	{ 0x00112u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_DELAY_VFD" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:285; a6xx.xml:910 */
	{ 0x00117u, 0x00000002u, "REG_A6XX_RBBM_CLOCK_DELAY_HLSQ_2" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:286; a6xx.xml:915 */
	{ 0x0011bu, 0x00002222u, "REG_A6XX_RBBM_CLOCK_MODE_HLSQ" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:287; a6xx.xml:919 */
	{ 0x00118u, 0x00000222u, "REG_A6XX_RBBM_CLOCK_CNTL_GMU_GX" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:288; a6xx.xml:916 */
	{ 0x00119u, 0x00000111u, "REG_A6XX_RBBM_CLOCK_DELAY_GMU_GX" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:289; a6xx.xml:917 */
	{ 0x0011au, 0x00000555u, "REG_A6XX_RBBM_CLOCK_HYST_GMU_GX" }, /* a6xx_gpu.c:708 a6xx_set_hwcg(); a6xx_catalog.c:290; a6xx.xml:918 */
	/* HWCG_COMMIT: Linux a6xx_set_hwcg:711-714; SP clock must already be re-enabled. */
	{ 0x000aeu, 0x8aa8aa02u, "REG_A6XX_RBBM_CLOCK_CNTL" }, /* a6xx_gpu.c:714 a6xx_set_hwcg(); a6xx.xml:815 */
	/* CORE: Linux hw_init:1244-1353; UBWC uses sdm845_data, not an a660 profile. */
	{ 0x00010u, 0x00000003u, "REG_A6XX_RBBM_VBIF_CLIENT_QOS_CNTL" }, /* a6xx_gpu.c:1251 hw_init(); a6xx.xml:780 */
	{ 0x0302au, 0x00000009u, "REG_A6XX_VBIF_GATE_OFF_WRREQ_EN" }, /* a6xx_gpu.c:1255 hw_init(); a6xx.xml:1236 */
	{ 0x0050bu, 0xffffffffu, "REG_A6XX_RBBM_PERFCTR_GPU_BUSY_MASKED" }, /* a6xx_gpu.c:1261 hw_init(); a6xx.xml:719 */
	{ 0x00e05u, 0xffffffc0u, "REG_A6XX_UCHE_WRITE_RANGE_MAX_LO" }, /* a6xx_gpu.c:1268 hw_init(); a6xx_gpu.c:2769 a6xx_gpu_init(); a6xx.xml:1203 */
	{ 0x00e06u, 0x0001ffffu, "REG_A6XX_UCHE_WRITE_RANGE_MAX_HI" }, /* a6xx_gpu.c:1268 hw_init(); a6xx_gpu.c:2769 a6xx_gpu_init(); a6xx.xml:1203 */
	{ 0x00e09u, 0xfffff000u, "REG_A6XX_UCHE_TRAP_BASE_LO" }, /* a6xx_gpu.c:1269 hw_init(); a6xx_gpu.c:2769 a6xx_gpu_init(); a6xx.xml:1206 */
	{ 0x00e0au, 0x0001ffffu, "REG_A6XX_UCHE_TRAP_BASE_HI" }, /* a6xx_gpu.c:1269 hw_init(); a6xx_gpu.c:2769 a6xx_gpu_init(); a6xx.xml:1206 */
	{ 0x00e07u, 0xfffff000u, "REG_A6XX_UCHE_WRITE_THRU_BASE_LO" }, /* a6xx_gpu.c:1270 hw_init(); a6xx_gpu.c:2769 a6xx_gpu_init(); a6xx.xml:1204 */
	{ 0x00e08u, 0x0001ffffu, "REG_A6XX_UCHE_WRITE_THRU_BASE_HI" }, /* a6xx_gpu.c:1270 hw_init(); a6xx_gpu.c:2769 a6xx_gpu_init(); a6xx.xml:1204 */
	{ 0x00e0bu, 0x00100000u, "REG_A6XX_UCHE_GMEM_RANGE_MIN_LO" }, /* a6xx_gpu.c:1279 hw_init(); a6xx.xml:1208 */
	{ 0x00e0cu, 0x00000000u, "REG_A6XX_UCHE_GMEM_RANGE_MIN_HI" }, /* a6xx_gpu.c:1279 hw_init(); a6xx.xml:1208 */
	{ 0x00e0du, 0x001fffffu, "REG_A6XX_UCHE_GMEM_RANGE_MAX_LO" }, /* a6xx_gpu.c:1281 hw_init(); a6xx.xml:1209 */
	{ 0x00e0eu, 0x00000000u, "REG_A6XX_UCHE_GMEM_RANGE_MAX_HI" }, /* a6xx_gpu.c:1281 hw_init(); a6xx.xml:1209 */
	{ 0x00e18u, 0x00000804u, "REG_A6XX_UCHE_FILTER_CNTL" }, /* a6xx_gpu.c:1288 hw_init(); a6xx.xml:1212 */
	{ 0x00e17u, 0x00000004u, "REG_A6XX_UCHE_CACHE_WAYS" }, /* a6xx_gpu.c:1289 hw_init(); a6xx.xml:1210 */
	{ 0x008c2u, 0x010000c0u, "REG_A6XX_CP_ROQ_THRESHOLDS_2" }, /* a6xx_gpu.c:1299 hw_init(); a6xx.xml:251 */
	{ 0x008c1u, 0x8040362cu, "REG_A6XX_CP_ROQ_THRESHOLDS_1" }, /* a6xx_gpu.c:1300 hw_init(); a6xx.xml:239 */
	{ 0x008c3u, 0x00000080u, "REG_A6XX_CP_MEM_POOL_SIZE" }, /* a6xx_gpu.c:1314 hw_init(); a6xx.xml:267 */
	{ 0x09e00u, 0x00180000u, "REG_A6XX_PC_DBG_ECO_CNTL" }, /* a6xx_gpu.c:1319 hw_init(); a6xx.xml:3428 */
	{ 0x0098du, 0x00000001u, "REG_A6XX_CP_AHB_CNTL" }, /* a6xx_gpu.c:1323 hw_init(); a6xx.xml:455 */
	{ 0x00500u, 0x00000001u, "REG_A6XX_RBBM_PERFCTR_CNTL" }, /* a6xx_gpu.c:1326 hw_init(); a6xx.xml:710 */
	{ 0x008d0u, 0x00000000u, "REG_A6XX_CP_PERFCTR_CP_SEL(0)" }, /* a6xx_gpu.c:1335 hw_init(); a6xx_perfcntrs.xml:10; a6xx.xml:325 */
	{ 0x08e08u, 0x00000004u, "REG_A6XX_RB_NC_MODE_CNTL" }, /* a6xx_gpu.c:767 a6xx_set_ubwc_config(); ubwc_config.c:128-134 sdm845_data; a6xx.xml:2862 */
	{ 0x0b604u, 0x00000004u, "REG_A6XX_TPL1_NC_MODE_CNTL" }, /* a6xx_gpu.c:774 a6xx_set_ubwc_config(); ubwc_config.c:128-134 sdm845_data; a6xx.xml:4548 */
	{ 0x0ae02u, 0x00000004u, "REG_A6XX_SP_NC_MODE_CNTL" }, /* a6xx_gpu.c:779 a6xx_set_ubwc_config(); ubwc_config.c:128-134 sdm845_data; a6xx.xml:4332 */
	{ 0x00e01u, 0x00400000u, "REG_A6XX_UCHE_MODE_CNTL" }, /* a6xx_gpu.c:796 a6xx_set_ubwc_config(); ubwc_config.c:128-134 sdm845_data; a6xx.xml:1202 */
	{ 0x00534u, 0x00000000u, "REG_A6XX_RBBM_NC_MODE_CNTL" }, /* a6xx_gpu.c:799 a6xx_set_ubwc_config(); ubwc_config.c:128-134 sdm845_data; a6xx.xml:729 */
	{ 0x0001fu, 0x401fffffu, "REG_A6XX_RBBM_INTERFACE_HANG_INT_CNTL" }, /* a6xx_gpu.c:1351 hw_init(); a6xx.xml:796 */
	{ 0x00e19u, 0x00000081u, "REG_A6XX_UCHE_CLIENT_PF" }, /* a6xx_gpu.c:1353 hw_init(); a6xx.xml:1213 */
	/* PROTECT: Linux hw_init:1368-1376; caller GMU counters precede CP protection. */
	{ 0x0084fu, 0x0000000bu, "REG_A6XX_CP_PROTECT_CNTL" }, /* a6xx_gpu.c:728 a6xx_set_cp_protect(); a6xx.xml:280 */
	{ 0x00850u, 0x13fc0000u, "REG_A6XX_CP_PROTECT(0)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:509; a6xx.xml:293 */
	{ 0x00851u, 0x00140501u, "REG_A6XX_CP_PROTECT(1)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:510; a6xx.xml:293 */
	{ 0x00852u, 0x0bd0050bu, "REG_A6XX_CP_PROTECT(2)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:511; a6xx.xml:293 */
	{ 0x00853u, 0x8000050eu, "REG_A6XX_CP_PROTECT(3)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:512; a6xx.xml:293 */
	{ 0x00854u, 0x80000510u, "REG_A6XX_CP_PROTECT(4)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:513; a6xx.xml:293 */
	{ 0x00855u, 0x80000534u, "REG_A6XX_CP_PROTECT(5)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:514; a6xx.xml:293 */
	{ 0x00856u, 0x82080800u, "REG_A6XX_CP_PROTECT(6)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:515; a6xx.xml:293 */
	{ 0x00857u, 0x802008a0u, "REG_A6XX_CP_PROTECT(7)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:516; a6xx.xml:293 */
	{ 0x00858u, 0x809008abu, "REG_A6XX_CP_PROTECT(8)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:517; a6xx.xml:293 */
	{ 0x00859u, 0x02b808deu, "REG_A6XX_CP_PROTECT(9)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:518; a6xx.xml:293 */
	{ 0x0085au, 0x81340900u, "REG_A6XX_CP_PROTECT(10)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:519; a6xx.xml:293 */
	{ 0x0085bu, 0x89c8098du, "REG_A6XX_CP_PROTECT(11)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:520; a6xx.xml:293 */
	{ 0x0085cu, 0x80040e00u, "REG_A6XX_CP_PROTECT(12)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:521; a6xx.xml:293 */
	{ 0x0085du, 0x80300e03u, "REG_A6XX_CP_PROTECT(13)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:522; a6xx.xml:293 */
	{ 0x0085eu, 0x830c3c00u, "REG_A6XX_CP_PROTECT(14)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:523; a6xx.xml:293 */
	{ 0x0085fu, 0x7ffc3cc4u, "REG_A6XX_CP_PROTECT(15)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:524; a6xx.xml:293 */
	{ 0x00860u, 0x873c8630u, "REG_A6XX_CP_PROTECT(16)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:525; a6xx.xml:293 */
	{ 0x00861u, 0x80008e00u, "REG_A6XX_CP_PROTECT(17)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:526; a6xx.xml:293 */
	{ 0x00862u, 0x80008e08u, "REG_A6XX_CP_PROTECT(18)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:527; a6xx.xml:293 */
	{ 0x00863u, 0x807c8e50u, "REG_A6XX_CP_PROTECT(19)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:528; a6xx.xml:293 */
	{ 0x00864u, 0x876c9624u, "REG_A6XX_CP_PROTECT(20)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:529; a6xx.xml:293 */
	{ 0x00865u, 0x80049e70u, "REG_A6XX_CP_PROTECT(21)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:530; a6xx.xml:293 */
	{ 0x00866u, 0x861c9e78u, "REG_A6XX_CP_PROTECT(22)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:531; a6xx.xml:293 */
	{ 0x00867u, 0x873ca630u, "REG_A6XX_CP_PROTECT(23)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:532; a6xx.xml:293 */
	{ 0x00868u, 0x8000ae02u, "REG_A6XX_CP_PROTECT(24)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:533; a6xx.xml:293 */
	{ 0x00869u, 0x8cbcae50u, "REG_A6XX_CP_PROTECT(25)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:534; a6xx.xml:293 */
	{ 0x0086au, 0x8000b604u, "REG_A6XX_CP_PROTECT(26)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:535; a6xx.xml:293 */
	{ 0x0086bu, 0x8004be02u, "REG_A6XX_CP_PROTECT(27)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:536; a6xx.xml:293 */
	{ 0x0086cu, 0xdf7cbe20u, "REG_A6XX_CP_PROTECT(28)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:537; a6xx.xml:293 */
	{ 0x0086du, 0xaffcf000u, "REG_A6XX_CP_PROTECT(29)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:538; a6xx.xml:293 */
	{ 0x0086eu, 0x7ffcfc00u, "REG_A6XX_CP_PROTECT(30)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:539; a6xx.xml:293 */
	{ 0x0086fu, 0x80011c00u, "REG_A6XX_CP_PROTECT(31)" }, /* a6xx_gpu.c:739 a6xx_set_cp_protect(); a6xx_catalog.c:540; a6xx.xml:293 */
	/* IRQ: Linux hw_init:1427-1429; caller IRQ handler/gating must be ready. */
	{ 0x00038u, 0x03d0e242u, "REG_A6XX_RBBM_INT_0_MASK" }, /* a6xx_gpu.c:1428 hw_init(); a6xx.xml:800 */
};

/* Audit only: vendor secvid-set-once policy belongs to the caller, not the static replay. */
static const struct {
	uint32_t reg;
	uint32_t value;
	const char *name;
} a630_caller_security_table[] = {
	{ 0x0f803u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_CNTL" }, /* a6xx_gpu.c:1204 hw_init(); a6xx.xml:777 */
	{ 0x0f800u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE_LO" }, /* a6xx_gpu.c:1214 hw_init(); a6xx.xml:775 */
	{ 0x0f801u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_TRUSTED_BASE_HI" }, /* a6xx_gpu.c:1214 hw_init(); a6xx.xml:775 */
	{ 0x0f802u, 0x00000000u, "REG_A6XX_RBBM_SECVID_TSB_TRUSTED_SIZE" }, /* a6xx_gpu.c:1215 hw_init(); a6xx.xml:776 */
	{ 0x0f810u, 0x00000001u, "REG_A6XX_RBBM_SECVID_TSB_ADDR_MODE_CNTL" }, /* a6xx_gpu.c:1230 hw_init(); a6xx.xml:778 */
};

/* The split prevents GMU read-modify-writes from being moved across HWCG writes. */
enum {
	A630_INIT_ADDRESS_BEGIN = 0,
	A630_INIT_ADDRESS_END = 11,
	A630_INIT_HWCG_BEGIN = 11,
	A630_INIT_HWCG_END = 116,
	A630_INIT_HWCG_COMMIT_BEGIN = 116,
	A630_INIT_HWCG_COMMIT_END = 117,
	A630_INIT_CORE_BEGIN = 117,
	A630_INIT_CORE_END = 146,
	A630_INIT_PROTECT_BEGIN = 146,
	A630_INIT_PROTECT_END = 179,
	A630_INIT_IRQ_BEGIN = 179,
	A630_INIT_IRQ_END = 180,
	A630_INIT_COUNT = 180
};

/* Catalog reglist, without its zero sentinel: zero is not a hardware write. */
static const struct {
	uint32_t reg;
	uint32_t value;
	const char *name;
} a630_hwcg_table[] = {
	{ 0x000b0u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP0" }, /* a6xx_catalog.c:186 a630_hwcg[]; a6xx.xml:816 */
	{ 0x000b1u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP1" }, /* a6xx_catalog.c:187 a630_hwcg[]; a6xx.xml:817 */
	{ 0x000b2u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP2" }, /* a6xx_catalog.c:188 a630_hwcg[]; a6xx.xml:818 */
	{ 0x000b3u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_SP3" }, /* a6xx_catalog.c:189 a630_hwcg[]; a6xx.xml:819 */
	{ 0x000b4u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP0" }, /* a6xx_catalog.c:190 a630_hwcg[]; a6xx.xml:820 */
	{ 0x000b5u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP1" }, /* a6xx_catalog.c:191 a630_hwcg[]; a6xx.xml:821 */
	{ 0x000b6u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP2" }, /* a6xx_catalog.c:192 a630_hwcg[]; a6xx.xml:822 */
	{ 0x000b7u, 0x02022220u, "REG_A6XX_RBBM_CLOCK_CNTL2_SP3" }, /* a6xx_catalog.c:193 a630_hwcg[]; a6xx.xml:823 */
	{ 0x000b8u, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP0" }, /* a6xx_catalog.c:194 a630_hwcg[]; a6xx.xml:824 */
	{ 0x000b9u, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP1" }, /* a6xx_catalog.c:195 a630_hwcg[]; a6xx.xml:825 */
	{ 0x000bau, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP2" }, /* a6xx_catalog.c:196 a630_hwcg[]; a6xx.xml:826 */
	{ 0x000bbu, 0x00000080u, "REG_A6XX_RBBM_CLOCK_DELAY_SP3" }, /* a6xx_catalog.c:197 a630_hwcg[]; a6xx.xml:827 */
	{ 0x000bcu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP0" }, /* a6xx_catalog.c:198 a630_hwcg[]; a6xx.xml:828 */
	{ 0x000bdu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP1" }, /* a6xx_catalog.c:199 a630_hwcg[]; a6xx.xml:829 */
	{ 0x000beu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP2" }, /* a6xx_catalog.c:200 a630_hwcg[]; a6xx.xml:830 */
	{ 0x000bfu, 0x0000f3cfu, "REG_A6XX_RBBM_CLOCK_HYST_SP3" }, /* a6xx_catalog.c:201 a630_hwcg[]; a6xx.xml:831 */
	{ 0x000c0u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP0" }, /* a6xx_catalog.c:202 a630_hwcg[]; a6xx.xml:832 */
	{ 0x000c1u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP1" }, /* a6xx_catalog.c:203 a630_hwcg[]; a6xx.xml:833 */
	{ 0x000c2u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP2" }, /* a6xx_catalog.c:204 a630_hwcg[]; a6xx.xml:834 */
	{ 0x000c3u, 0x02222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TP3" }, /* a6xx_catalog.c:205 a630_hwcg[]; a6xx.xml:835 */
	{ 0x000c4u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP0" }, /* a6xx_catalog.c:206 a630_hwcg[]; a6xx.xml:836 */
	{ 0x000c5u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP1" }, /* a6xx_catalog.c:207 a630_hwcg[]; a6xx.xml:837 */
	{ 0x000c6u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP2" }, /* a6xx_catalog.c:208 a630_hwcg[]; a6xx.xml:838 */
	{ 0x000c7u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_TP3" }, /* a6xx_catalog.c:209 a630_hwcg[]; a6xx.xml:839 */
	{ 0x000c8u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP0" }, /* a6xx_catalog.c:210 a630_hwcg[]; a6xx.xml:840 */
	{ 0x000c9u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP1" }, /* a6xx_catalog.c:211 a630_hwcg[]; a6xx.xml:841 */
	{ 0x000cau, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP2" }, /* a6xx_catalog.c:212 a630_hwcg[]; a6xx.xml:842 */
	{ 0x000cbu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_TP3" }, /* a6xx_catalog.c:213 a630_hwcg[]; a6xx.xml:843 */
	{ 0x000ccu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP0" }, /* a6xx_catalog.c:214 a630_hwcg[]; a6xx.xml:844 */
	{ 0x000cdu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP1" }, /* a6xx_catalog.c:215 a630_hwcg[]; a6xx.xml:845 */
	{ 0x000ceu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP2" }, /* a6xx_catalog.c:216 a630_hwcg[]; a6xx.xml:846 */
	{ 0x000cfu, 0x00022222u, "REG_A6XX_RBBM_CLOCK_CNTL4_TP3" }, /* a6xx_catalog.c:217 a630_hwcg[]; a6xx.xml:847 */
	{ 0x000e0u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP0" }, /* a6xx_catalog.c:218 a630_hwcg[]; a6xx.xml:864 */
	{ 0x000e1u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP1" }, /* a6xx_catalog.c:219 a630_hwcg[]; a6xx.xml:865 */
	{ 0x000e2u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP2" }, /* a6xx_catalog.c:220 a630_hwcg[]; a6xx.xml:866 */
	{ 0x000e3u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST_TP3" }, /* a6xx_catalog.c:221 a630_hwcg[]; a6xx.xml:867 */
	{ 0x000e4u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP0" }, /* a6xx_catalog.c:222 a630_hwcg[]; a6xx.xml:868 */
	{ 0x000e5u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP1" }, /* a6xx_catalog.c:223 a630_hwcg[]; a6xx.xml:869 */
	{ 0x000e6u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP2" }, /* a6xx_catalog.c:224 a630_hwcg[]; a6xx.xml:870 */
	{ 0x000e7u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST2_TP3" }, /* a6xx_catalog.c:225 a630_hwcg[]; a6xx.xml:871 */
	{ 0x000e8u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP0" }, /* a6xx_catalog.c:226 a630_hwcg[]; a6xx.xml:872 */
	{ 0x000e9u, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP1" }, /* a6xx_catalog.c:227 a630_hwcg[]; a6xx.xml:873 */
	{ 0x000eau, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP2" }, /* a6xx_catalog.c:228 a630_hwcg[]; a6xx.xml:874 */
	{ 0x000ebu, 0x77777777u, "REG_A6XX_RBBM_CLOCK_HYST3_TP3" }, /* a6xx_catalog.c:229 a630_hwcg[]; a6xx.xml:875 */
	{ 0x000ecu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP0" }, /* a6xx_catalog.c:230 a630_hwcg[]; a6xx.xml:876 */
	{ 0x000edu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP1" }, /* a6xx_catalog.c:231 a630_hwcg[]; a6xx.xml:877 */
	{ 0x000eeu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP2" }, /* a6xx_catalog.c:232 a630_hwcg[]; a6xx.xml:878 */
	{ 0x000efu, 0x00077777u, "REG_A6XX_RBBM_CLOCK_HYST4_TP3" }, /* a6xx_catalog.c:233 a630_hwcg[]; a6xx.xml:879 */
	{ 0x000d0u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP0" }, /* a6xx_catalog.c:234 a630_hwcg[]; a6xx.xml:848 */
	{ 0x000d1u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP1" }, /* a6xx_catalog.c:235 a630_hwcg[]; a6xx.xml:849 */
	{ 0x000d2u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP2" }, /* a6xx_catalog.c:236 a630_hwcg[]; a6xx.xml:850 */
	{ 0x000d3u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY_TP3" }, /* a6xx_catalog.c:237 a630_hwcg[]; a6xx.xml:851 */
	{ 0x000d4u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP0" }, /* a6xx_catalog.c:238 a630_hwcg[]; a6xx.xml:852 */
	{ 0x000d5u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP1" }, /* a6xx_catalog.c:239 a630_hwcg[]; a6xx.xml:853 */
	{ 0x000d6u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP2" }, /* a6xx_catalog.c:240 a630_hwcg[]; a6xx.xml:854 */
	{ 0x000d7u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY2_TP3" }, /* a6xx_catalog.c:241 a630_hwcg[]; a6xx.xml:855 */
	{ 0x000d8u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP0" }, /* a6xx_catalog.c:242 a630_hwcg[]; a6xx.xml:856 */
	{ 0x000d9u, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP1" }, /* a6xx_catalog.c:243 a630_hwcg[]; a6xx.xml:857 */
	{ 0x000dau, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP2" }, /* a6xx_catalog.c:244 a630_hwcg[]; a6xx.xml:858 */
	{ 0x000dbu, 0x11111111u, "REG_A6XX_RBBM_CLOCK_DELAY3_TP3" }, /* a6xx_catalog.c:245 a630_hwcg[]; a6xx.xml:859 */
	{ 0x000dcu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP0" }, /* a6xx_catalog.c:246 a630_hwcg[]; a6xx.xml:860 */
	{ 0x000ddu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP1" }, /* a6xx_catalog.c:247 a630_hwcg[]; a6xx.xml:861 */
	{ 0x000deu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP2" }, /* a6xx_catalog.c:248 a630_hwcg[]; a6xx.xml:862 */
	{ 0x000dfu, 0x00011111u, "REG_A6XX_RBBM_CLOCK_DELAY4_TP3" }, /* a6xx_catalog.c:249 a630_hwcg[]; a6xx.xml:863 */
	{ 0x0010bu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_UCHE" }, /* a6xx_catalog.c:250 a630_hwcg[]; a6xx.xml:903 */
	{ 0x0010cu, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL2_UCHE" }, /* a6xx_catalog.c:251 a630_hwcg[]; a6xx.xml:904 */
	{ 0x0010du, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL3_UCHE" }, /* a6xx_catalog.c:252 a630_hwcg[]; a6xx.xml:905 */
	{ 0x0010eu, 0x00222222u, "REG_A6XX_RBBM_CLOCK_CNTL4_UCHE" }, /* a6xx_catalog.c:253 a630_hwcg[]; a6xx.xml:906 */
	{ 0x00110u, 0x00000004u, "REG_A6XX_RBBM_CLOCK_HYST_UCHE" }, /* a6xx_catalog.c:254 a630_hwcg[]; a6xx.xml:908 */
	{ 0x0010fu, 0x00000002u, "REG_A6XX_RBBM_CLOCK_DELAY_UCHE" }, /* a6xx_catalog.c:255 a630_hwcg[]; a6xx.xml:907 */
	{ 0x000f0u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB0" }, /* a6xx_catalog.c:256 a630_hwcg[]; a6xx.xml:880 */
	{ 0x000f1u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB1" }, /* a6xx_catalog.c:257 a630_hwcg[]; a6xx.xml:881 */
	{ 0x000f2u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB2" }, /* a6xx_catalog.c:258 a630_hwcg[]; a6xx.xml:882 */
	{ 0x000f3u, 0x22222222u, "REG_A6XX_RBBM_CLOCK_CNTL_RB3" }, /* a6xx_catalog.c:259 a630_hwcg[]; a6xx.xml:883 */
	{ 0x000f4u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB0" }, /* a6xx_catalog.c:260 a630_hwcg[]; a6xx.xml:884 */
	{ 0x000f5u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB1" }, /* a6xx_catalog.c:261 a630_hwcg[]; a6xx.xml:885 */
	{ 0x000f6u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB2" }, /* a6xx_catalog.c:262 a630_hwcg[]; a6xx.xml:886 */
	{ 0x000f7u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_CNTL2_RB3" }, /* a6xx_catalog.c:263 a630_hwcg[]; a6xx.xml:887 */
	{ 0x000f8u, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU0" }, /* a6xx_catalog.c:264 a630_hwcg[]; a6xx.xml:888 */
	{ 0x000f9u, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU1" }, /* a6xx_catalog.c:265 a630_hwcg[]; a6xx.xml:889 */
	{ 0x000fau, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU2" }, /* a6xx_catalog.c:266 a630_hwcg[]; a6xx.xml:890 */
	{ 0x000fbu, 0x00002220u, "REG_A6XX_RBBM_CLOCK_CNTL_CCU3" }, /* a6xx_catalog.c:267 a630_hwcg[]; a6xx.xml:891 */
	{ 0x00100u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU0" }, /* a6xx_catalog.c:268 a630_hwcg[]; a6xx.xml:892 */
	{ 0x00101u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU1" }, /* a6xx_catalog.c:269 a630_hwcg[]; a6xx.xml:893 */
	{ 0x00102u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU2" }, /* a6xx_catalog.c:270 a630_hwcg[]; a6xx.xml:894 */
	{ 0x00103u, 0x00040f00u, "REG_A6XX_RBBM_CLOCK_HYST_RB_CCU3" }, /* a6xx_catalog.c:271 a630_hwcg[]; a6xx.xml:895 */
	{ 0x00104u, 0x05022022u, "REG_A6XX_RBBM_CLOCK_CNTL_RAC" }, /* a6xx_catalog.c:272 a630_hwcg[]; a6xx.xml:896 */
	{ 0x00105u, 0x00005555u, "REG_A6XX_RBBM_CLOCK_CNTL2_RAC" }, /* a6xx_catalog.c:273 a630_hwcg[]; a6xx.xml:897 */
	{ 0x00106u, 0x00000011u, "REG_A6XX_RBBM_CLOCK_DELAY_RAC" }, /* a6xx_catalog.c:274 a630_hwcg[]; a6xx.xml:898 */
	{ 0x00107u, 0x00445044u, "REG_A6XX_RBBM_CLOCK_HYST_RAC" }, /* a6xx_catalog.c:275 a630_hwcg[]; a6xx.xml:899 */
	{ 0x00108u, 0x04222222u, "REG_A6XX_RBBM_CLOCK_CNTL_TSE_RAS_RBBM" }, /* a6xx_catalog.c:276 a630_hwcg[]; a6xx.xml:900 */
	{ 0x00114u, 0x00222222u, "REG_A6XX_RBBM_CLOCK_MODE_GPC" }, /* a6xx_catalog.c:277 a630_hwcg[]; a6xx.xml:912 */
	{ 0x00111u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_MODE_VFD" }, /* a6xx_catalog.c:278 a630_hwcg[]; a6xx.xml:909 */
	{ 0x0010au, 0x00000000u, "REG_A6XX_RBBM_CLOCK_HYST_TSE_RAS_RBBM" }, /* a6xx_catalog.c:279 a630_hwcg[]; a6xx.xml:902 */
	{ 0x00116u, 0x04104004u, "REG_A6XX_RBBM_CLOCK_HYST_GPC" }, /* a6xx_catalog.c:280 a630_hwcg[]; a6xx.xml:914 */
	{ 0x00113u, 0x00000000u, "REG_A6XX_RBBM_CLOCK_HYST_VFD" }, /* a6xx_catalog.c:281 a630_hwcg[]; a6xx.xml:911 */
	{ 0x0011cu, 0x00000000u, "REG_A6XX_RBBM_CLOCK_DELAY_HLSQ" }, /* a6xx_catalog.c:282 a630_hwcg[]; a6xx.xml:920 */
	{ 0x00109u, 0x00004000u, "REG_A6XX_RBBM_CLOCK_DELAY_TSE_RAS_RBBM" }, /* a6xx_catalog.c:283 a630_hwcg[]; a6xx.xml:901 */
	{ 0x00115u, 0x00000200u, "REG_A6XX_RBBM_CLOCK_DELAY_GPC" }, /* a6xx_catalog.c:284 a630_hwcg[]; a6xx.xml:913 */
	{ 0x00112u, 0x00002222u, "REG_A6XX_RBBM_CLOCK_DELAY_VFD" }, /* a6xx_catalog.c:285 a630_hwcg[]; a6xx.xml:910 */
	{ 0x00117u, 0x00000002u, "REG_A6XX_RBBM_CLOCK_DELAY_HLSQ_2" }, /* a6xx_catalog.c:286 a630_hwcg[]; a6xx.xml:915 */
	{ 0x0011bu, 0x00002222u, "REG_A6XX_RBBM_CLOCK_MODE_HLSQ" }, /* a6xx_catalog.c:287 a630_hwcg[]; a6xx.xml:919 */
	{ 0x00118u, 0x00000222u, "REG_A6XX_RBBM_CLOCK_CNTL_GMU_GX" }, /* a6xx_catalog.c:288 a630_hwcg[]; a6xx.xml:916 */
	{ 0x00119u, 0x00000111u, "REG_A6XX_RBBM_CLOCK_DELAY_GMU_GX" }, /* a6xx_catalog.c:289 a630_hwcg[]; a6xx.xml:917 */
	{ 0x0011au, 0x00000555u, "REG_A6XX_RBBM_CLOCK_HYST_GMU_GX" }, /* a6xx_catalog.c:290 a630_hwcg[]; a6xx.xml:918 */
};

/* Expanded from the actual protect macros so the infinite final span remains slot 31. */
static const struct {
	uint32_t reg;
	uint32_t value;
	const char *name;
} a630_cp_protect_table[] = {
	{ 0x00850u, 0x13fc0000u, "REG_A6XX_CP_PROTECT(0)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:509; a6xx.xml:293 */
	{ 0x00851u, 0x00140501u, "REG_A6XX_CP_PROTECT(1)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:510; a6xx.xml:293 */
	{ 0x00852u, 0x0bd0050bu, "REG_A6XX_CP_PROTECT(2)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:511; a6xx.xml:293 */
	{ 0x00853u, 0x8000050eu, "REG_A6XX_CP_PROTECT(3)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:512; a6xx.xml:293 */
	{ 0x00854u, 0x80000510u, "REG_A6XX_CP_PROTECT(4)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:513; a6xx.xml:293 */
	{ 0x00855u, 0x80000534u, "REG_A6XX_CP_PROTECT(5)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:514; a6xx.xml:293 */
	{ 0x00856u, 0x82080800u, "REG_A6XX_CP_PROTECT(6)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:515; a6xx.xml:293 */
	{ 0x00857u, 0x802008a0u, "REG_A6XX_CP_PROTECT(7)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:516; a6xx.xml:293 */
	{ 0x00858u, 0x809008abu, "REG_A6XX_CP_PROTECT(8)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:517; a6xx.xml:293 */
	{ 0x00859u, 0x02b808deu, "REG_A6XX_CP_PROTECT(9)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:518; a6xx.xml:293 */
	{ 0x0085au, 0x81340900u, "REG_A6XX_CP_PROTECT(10)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:519; a6xx.xml:293 */
	{ 0x0085bu, 0x89c8098du, "REG_A6XX_CP_PROTECT(11)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:520; a6xx.xml:293 */
	{ 0x0085cu, 0x80040e00u, "REG_A6XX_CP_PROTECT(12)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:521; a6xx.xml:293 */
	{ 0x0085du, 0x80300e03u, "REG_A6XX_CP_PROTECT(13)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:522; a6xx.xml:293 */
	{ 0x0085eu, 0x830c3c00u, "REG_A6XX_CP_PROTECT(14)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:523; a6xx.xml:293 */
	{ 0x0085fu, 0x7ffc3cc4u, "REG_A6XX_CP_PROTECT(15)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:524; a6xx.xml:293 */
	{ 0x00860u, 0x873c8630u, "REG_A6XX_CP_PROTECT(16)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:525; a6xx.xml:293 */
	{ 0x00861u, 0x80008e00u, "REG_A6XX_CP_PROTECT(17)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:526; a6xx.xml:293 */
	{ 0x00862u, 0x80008e08u, "REG_A6XX_CP_PROTECT(18)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:527; a6xx.xml:293 */
	{ 0x00863u, 0x807c8e50u, "REG_A6XX_CP_PROTECT(19)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:528; a6xx.xml:293 */
	{ 0x00864u, 0x876c9624u, "REG_A6XX_CP_PROTECT(20)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:529; a6xx.xml:293 */
	{ 0x00865u, 0x80049e70u, "REG_A6XX_CP_PROTECT(21)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:530; a6xx.xml:293 */
	{ 0x00866u, 0x861c9e78u, "REG_A6XX_CP_PROTECT(22)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:531; a6xx.xml:293 */
	{ 0x00867u, 0x873ca630u, "REG_A6XX_CP_PROTECT(23)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:532; a6xx.xml:293 */
	{ 0x00868u, 0x8000ae02u, "REG_A6XX_CP_PROTECT(24)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:533; a6xx.xml:293 */
	{ 0x00869u, 0x8cbcae50u, "REG_A6XX_CP_PROTECT(25)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:534; a6xx.xml:293 */
	{ 0x0086au, 0x8000b604u, "REG_A6XX_CP_PROTECT(26)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:535; a6xx.xml:293 */
	{ 0x0086bu, 0x8004be02u, "REG_A6XX_CP_PROTECT(27)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:536; a6xx.xml:293 */
	{ 0x0086cu, 0xdf7cbe20u, "REG_A6XX_CP_PROTECT(28)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:537; a6xx.xml:293 */
	{ 0x0086du, 0xaffcf000u, "REG_A6XX_CP_PROTECT(29)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:538; a6xx.xml:293 */
	{ 0x0086eu, 0x7ffcfc00u, "REG_A6XX_CP_PROTECT(30)" }, /* a6xx_gpu.c:736 a6xx_set_cp_protect(); a6xx_catalog.c:539; a6xx.xml:293 */
	{ 0x0086fu, 0x80011c00u, "REG_A6XX_CP_PROTECT(31)" }, /* a6xx_gpu.c:739 a6xx_set_cp_protect(); a6xx_catalog.c:540; a6xx.xml:293 */
};

/* Metadata is not MMIO: cached-coherent is a Linux quirk, not proof of XNU coherency. */
static const struct {
	const char *name;
	uint32_t value;
} a630_constants[] = {
	{ "chip_id", 0x06030001u }, /* a6xx_catalog.c:947-968 a630 entry */
	{ "gmem_bytes", 0x00100000u }, /* a6xx_catalog.c:947-968 a630 entry */
	{ "quirks", 0x00000050u }, /* a6xx_catalog.c:958-959 a630 entry; adreno_gpu.h:64-66 */
	{ "gmu_cgc_mode", 0x00020202u }, /* a6xx_catalog.c:947-968 a630 entry */
	{ "prim_fifo_threshold", 0x00180000u }, /* a6xx_catalog.c:947-968 a630 entry */
	{ "protect_count", 0x00000020u }, /* a6xx_catalog.c:508-542 a630_protect_regs; adreno_gpu.h:170-175 */
	{ "protect_count_max", 0x00000020u }, /* a6xx_catalog.c:508-542 a630_protect_regs; adreno_gpu.h:170-175 */
	{ "highest_bank_bit", 0x0000000fu }, /* ubwc_config.c:128-134 sdm845_data; ubwc.h:qcom_ubwc_*() */
	{ "ubwc_enc_version", 0x20000000u }, /* ubwc_config.c:128-134 sdm845_data; ubwc.h:qcom_ubwc_*() */
	{ "ubwc_dec_version", 0x20000000u }, /* ubwc_config.c:128-134 sdm845_data; ubwc.h:qcom_ubwc_*() */
	{ "ubwc_swizzle", 0x00000006u }, /* ubwc_config.c:128-134 sdm845_data; ubwc.h:qcom_ubwc_*() */
	{ "macrotile_mode", 0x00000000u }, /* ubwc_config.c:128-134 sdm845_data; ubwc.h:qcom_ubwc_*() */
	{ "min_access_length_bytes", 0x00000020u }, /* ubwc_config.c:128-134 sdm845_data; ubwc.h:qcom_ubwc_*() */
	{ "quirk_cached_coherent", 0x00000010u }, /* a6xx_catalog.c:958-959 a630 entry; adreno_gpu.h:64-66 */
	{ "quirk_4gb_va", 0x00000040u }, /* a6xx_catalog.c:958-959 a630 entry; adreno_gpu.h:64-66 */
	{ "hw_apriv", 0x00000000u }, /* a6xx_catalog.c:958-959 a630 entry; adreno_gpu.h:64-66 */
	{ "pwrup_reglist_present", 0x00000000u }, /* a6xx_catalog.c:947-968 a630 entry */
};

static bool
a630_hw_init_range(vm_offset_t gpu_base, uint32_t first, uint32_t end)
{
	/* Reject invalid software ranges before the first write; GX readiness is caller-owned. */
	if (gpu_base == 0 || (gpu_base & 3) != 0 || first > end || end > A630_INIT_COUNT) {
		return false;
	}
	for (uint32_t i = first; i < end; i++) {
		wr(gpu_base + ((vm_offset_t)a630_hw_init_table[i].reg << 2), a630_hw_init_table[i].value);
	}
	return true;
}

static bool
a630_hw_init(vm_offset_t gpu_base)
{
	/* Static replay only: caller must use ranges where the GMU/security/IRQ steps interleave. */
	return a630_hw_init_range(gpu_base, 0, A630_INIT_COUNT);
}
