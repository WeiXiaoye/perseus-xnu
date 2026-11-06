# 触摸：把驱动搬进 UEFI

## 为什么不在内核里做

在小米 MIX 3 上，触摸的每一环都归固件的 DXE 所有：

| 环节 | 归属 | 内核直接碰的后果 |
| --- | --- | --- |
| I/O 轨 `disp_vddio_vreg`（TLMM GPIO79） | TLMM | 可以写，但内核里写之前轨是关的，SDA/SCL 全低 |
| AVDD `pm8998_l28` | PMIC / RPMh | 写 RPMh RSC/TCS → **整机立刻复位** |
| 总线 QUP SE14 | I2C DXE | 引脚复用、时钟都由固件管 |
| 中断 GPIO31 / 复位 GPIO32 | TLMM | — |

内核里 `fts_steps=3`（上电）一执行就复位，就是这个原因。而在 UEFI 里，`TLMM`、`I2C`、
`PmicVreg` 三个协议都可用，edk2-sdm845 还自带 `SynapticsTouchDeviceDxe` / `SynapticsTouchDxe`
这套"设备层 + HID 层"的模板。

## 现在的进度（2026-11-06）

`uefi-probe/StFtsProbe.c` 是一个 UEFI 应用，用固件自己的协议把触摸芯片拉起来：

```
=== perseus touch probe v4 (UEFI) ===
TLMM: Success
I2C : Success
VREG: Success (L28 left alone: this firmware has no other user of it)
before: gpio79=0
gpio79 high: Success, reads 1
reset released, gpio79=1
0x49 answers: ..
FTM5: controller ready: 03 00 00 00 00
FTM5 chip id: 36 39 03 00 54 00     ← 36 39 = "69" = ST FTS5
FTM5 sysinfo: type 01 api 2.0 chip0 3639 fw 0054 cfg 002A
FTM5: scanning on (touch the panel now)
idle: int=1 read=1 raw=00 00 00 00 00 00 00 00
```

**已经打通的**

1. GPIO79 能把 I/O 轨打开（`0 → 1`），打开后 SDA/SCL 回到空闲高；
2. 用 GPIO33/34 **位敲 I2C**，绕开 QUP（SE14 在固件里不可用：任何实例/功能号组合都超时）；
3. `0x49` 应答，`0x38` 不应答 → **这台是 ST FTS，不是 FocalTech**；
4. **FTM5 协议通了**：`0x86` 读事件（命令不发送 STOP，读之前重复起始位）、`0x03` controller ready、
   `FA 20 00 00 00` 读得 `36 39`、`A6 00 00` 读出 sysinfo（api 2.0 / fw 0x54 / cfg 0x2A）；
5. 扫描命令 `A0 00 01` 能被接受并回显。

**卡住的地方**

扫描开着，但**中断脚恒为高、事件恒为 0** —— 芯片数字部分活着，模拟感应前端不工作。
原因：**AVDD（`pm8998_l28`，3.3 V）没有打开**。

## 为什么 AVDD 打不开

- 内核里通过 RPMh 写 L28 → 整机复位；
- UEFI 里 `PmicVreg->VregSetLevelUv(0, EFI_PM_LDO_28, 3300000)` → **卡死**。
  这棵固件里**没有任何 DXE 用 VREG 协议**，别的设备要 L28 时走的是 PMIC 的 ACPI 资源框架
  （`PPP_RESOURCE_ID_LDO28_A`，见 `sdm845Pkg/AcpiTables/legacy/*/DSDT.dsl`）。

## 下一步

1. 用 **PMIC 的 ACPI 资源路径**（而不是 VREG 协议）把 L28 打开，再复测 `int`/`raw`；
2. 事件循环搬进 **DXE 驱动**（照 `SynapticsTouchDeviceDxe`）：TLMM（79/32/31）+ 位敲 I2C（33/34），
   暴露 `EFI_ABSOLUTE_POINTER_PROTOCOL`；
3. 给 hackios 用：采样写进**共享页**（和 `ufs_shared_pa` 同一个套路），
   内核侧只做"读共享页 → `redwood_touch_post()`"，一行硬件寄存器都不碰；
4. 坐标换算：芯片 `x (0..1079) / y (0..2339)` → `65535` 归一化，方向位按需镜像。

## FTM5 速查（内核 `perseus_fts.c` 已验证）

| 操作 | 命令 | 说明 |
| --- | --- | --- |
| 读事件 | `86` | 写 1 字节（**不发送 STOP**），重复起始位读 8 字节 |
| 读硬件寄存器 | `FA` + 4 字节地址 | 芯片 ID：`FA 20 00 00 00` → `36 39` |
| 读系统信息 | `A6 00 00` | 32 字节：type、api、chip0、fw、cfg |
| 开扫描 | `A0 00 01` | 回显事件 `43 01 A0 00 01` |
| 事件类型 | `03` 就绪、`13` 按下、`23` 移动、`33` 抬起、`43` 状态、`F3` 错误 | |
| 坐标 | `x=(e[3]&0xf)<<8|e[2]`，`y=e[4]<<4|e[3]>>4` | `e[1]>>4` 手指号，`e[1]&0xf` 类型 |
