# perseus-xnu

**实验性 / 开发中（WIP）**

面向 **小米 MIX 3（`perseus`，SDM845）** 的 XNU / iOS 16.5 移植，基于
[HackiOS](https://github.com/oosexgang/hackios-xnu-kernel) 的路线：Apple 开源的 XNU 内核 +
自研 UEFI 引导链，跑**未修改的 iOS 16.5 用户态**（launchd → backboardd → SpringBoard）。

当前状态（2026-11-06）：**已经在这台 MIX 3 上进到 iOS 16.5 桌面（SpringBoard）**。
触控、GPU 仍在开发中。

> 本仓库只包含**补丁、新驱动和脚本**，不包含任何 Apple 二进制文件，也不包含内核构建产物。
> 内核需要在你自己的 Linux 机器上编译，iOS 系统卷需要你自己从 Apple 的 IPSW 里提取。

---

## 设备

| 项目 | 配置 |
| --- | --- |
| 设备代号 | `perseus` |
| SoC | Qualcomm SDM845 (Snapdragon 845) |
| GPU | Adreno 630 |
| 内存 | 6 / 8 GB（实测 8 GB 机型） |
| 屏幕 | 1080 × 2340 AMOLED（Samsung EA8076 / Visionox R66455 双供应商） |
| 触摸 | 同一组引脚上双供应商：ST FTS @0x49 / FocalTech @0x38 / Atmel mXT640U @0x4a |
| 触控总线 | QUPv3 SE14 `0xa98000`，IRQ GPIO31 / RST GPIO32 / SDA GPIO33 / SCL GPIO34 |
| 触摸供电 | `disp_vddio_vreg` = **TLMM GPIO 79**（I/O），`pm8998_l28` 3.3 V（模拟） |
| Wi-Fi / 蓝牙 | WCN3990 |
| PMIC | PM8998 + PMI8998 |
| 启动固件 | [edk2-sdm845](https://github.com/edk2-porting/edk2-sdm845)（Renegade Project），已内置 `perseus.dsc/fdf/dtb` |

---

## 功能状态（2026-11-07）

| 功能 | 状态 |
| --- | --- |
| 启动链 UEFI → loader → XNU | ✅ 已通 |
| 内核启动（8 核、GICv3、定时器、PSCI） | ✅ 已通 |
| 从 UFS 挂载 iOS 16.5 系统卷（APFS） | ✅ 已通 |
| cryptex（dyld 共享缓存）挂载 | ✅ 已通 |
| launchd → backboardd → **SpringBoard 桌面** | ✅ 已通 |
| 显示（UEFI 帧缓冲 0x9d400000，1080×2340） | ✅ 已通（CPU 光栅） |
| adb/fastboot 日志回传（`logdump` 分区） | ✅ 已通 |
| **触摸** | ✅ **已可用**：点按、拖动即时响应（UEFI 供数据，内核只搬运） |
| 上滑/返回手势 | 🚧 收尾中：需要按手指状态去重 |
| GPU（Adreno 630） | ❌ 未开始（内核侧仍是 A660 基线驱动） |
| Wi-Fi / 音频 / 相机 / 蓝牙 | ❌ 未开始 |
| 充电 | ❌ 未验证（只掉电） |

### 触摸是怎么做的（重要）

这台手机的触摸**不能在内核里驱动**：I/O 轨是 TLMM 的 GPIO79，模拟供电是 PM8998 的 L28，
而**内核里写一次 RPMh 开 L28 就会整机复位**（RSC 不属于 XNU）。所以：

```
loader（UEFI，core 0 常驻）                  内核（core 7）
  GPIO79 上电                                  映射共享页（ml_io_map_unmappable）
  RPMh 请求 pm8998_l28 3.3 V ≈ 共享页 ←写──── 轮询 seq/count
  IER、复位、清 FIFO、开扫描                    redwood_touch_post() → /dev/redwood-touch → iOS
  轮询事件、解析坐标、写共享页（每次 flush）
```

内核不碰任何触摸硬件，只读 `touch_shared_pa=` 指向的一页。两个必须踩对的点：

1. **物理地址不是内核指针**：要用 `ml_io_map_unmappable()` 映射，否则第一次读就 data abort；
2. **写完必须 flush cache**：内核是旁路缓存读的，不 flush 的话事件会攒着，直到别的内存活动
   把 cache line 挤出去才突然涌进来（表现为"点一下要等好几秒"）。

代码：`loader/touch.c`、`kernel/perseus_touch.c`，过程记录见 **[docs/touch-uefi.md](docs/touch-uefi.md)**。

---

## 启动链

```
fastboot boot boot-perseus-*.img
  └─ UEFI （edk2-sdm845 / perseus）
       └─ xnuloader（EFI 应用，常驻）
            ├─ md0 = flashboot.apfs（4 MB：stage0 作为 /sbin/launchd）
            ├─ md1 = root.apfs    （4 MB：真正的 init + LaunchDaemons）
            └─ XNU（xnu-8796.121.2，iOS 16.5）
                 └─ stage0 挂 /dev/ufs_userdata（iOS 16.5 系统卷 + 两个 cryptex）
                      └─ 切根 → Apple launchd → SpringBoard
```

`userdata` 里的布局（三段 APFS 容器首尾相接）：

```
偏移 0        iOS 16.5 (20F66) 系统卷
+2.32 GiB     OS cryptex（dyld 共享缓存）
+3.26 GiB     App cryptex
```

---

## 仓库内容

| 路径 | 内容 |
| --- | --- |
| `xnu/patches/` | 对 HackiOS 内核基线的改动（定时器中断号、触摸驱动接入） |
| `xnu/files/` | 新增源文件（`bsd/dev/perseus_fts.c`：ST FTS 触摸驱动，perseus 版） |
| `xnuloader/patches/` | 对 HackiOS loader 的改动（第二张 ramdisk、cmdline 终止、看门狗、运行期日志） |
| `loader/touch.c` | **UEFI 侧的触摸服务**：上电、RPMh 开 L28、复位、FTM5、写共享页 |
| `kernel/perseus_touch.c` | **内核侧的读取端**：映射共享页、post 到 `/dev/redwood-touch` |
| `uefi-probe/` | 独立的 UEFI 探测程序（上机诊断用，会停在屏幕上） |
| `device/` | `perseus.conf`（设备 profile）与 `perseus.platform.c`（loader 的 SoC 描述） |
| `docs/` | 构建步骤、触摸路线与调试记录 |

## 怎么编

本仓库**只有补丁和新文件**，不能单独编译——还需要 HackiOS 的基线树和一套 Linux 工具链
（clang/lld + theos 的 `iPhoneOS16.5.sdk`，不需要 macOS）。

```sh
git clone https://github.com/oosexgang/hackios-xnu-kernel.git
export HACKIOS=$PWD/hackios-xnu-kernel THEOS=$HOME/theos
sh perseus-xnu/scripts/build.sh                  # 内核
BUILD_LOADER=1 sh perseus-xnu/scripts/build.sh   # 内核 + loader.efi
```

脚本会校验基线指纹、跑 `xnu/apply.sh`、应用三个补丁、覆盖新文件、装依赖再编译。
完整步骤、基线版本与排错见 **[docs/BUILD.md](docs/BUILD.md)**。

内核与 loader 的编译产物、iOS 系统卷、厂商固件都**不在**本仓库：前者自行编译，
后者（IPSW、`a630_*` 固件等）必须由使用者自己从官方渠道或自己的设备获取。

---

## 已经踩过并修掉的坑（都能在补丁里看到）

1. **虚拟定时器中断号**：SDM845 的 EL1 虚拟定时器在高通固件上走 PPI 3（`INTID 19`），
   基线写死 27。错了就一个 tick 都没有：没有抢占、没有超时，`loader_return_after` 也不响。
2. **`boot_args` 命令行没有 NUL 结尾**：`boot_args` 每轮都在同一物理地址且不清零，
   新参数后面残留上一轮的字串，内核会读到旧参数（我们被 `fts_steps=6` 坑了很久）。
3. **payload 只带一张 ramdisk**：stage0 需要 `md1` 才能挂 `/redwood` 并 exec 真正的 init，
   缺了就 `cannot mount /dev/md1` 后 halt。
4. **看门狗 50 s → 15 s**，并且**周期写盘要带上内核日志环**，否则正常运行时 logdump 里只有 loader 的日志。
5. **触摸 I/O 供电脚**：MIX 3 是 TLMM **GPIO 79**（`disp_vddio_vreg`），
   小米 8 是 GPIO 23 —— 直接抄小米 8 的条件，芯片根本没上电，I2C 全低。
6. **触摸不能在 kernel 里开 L28**：写 RPMh 会整机复位（RSC 不是 XNU 的）；而
   `PmicVreg` 协议在这个固件里无人使用，一调就卡死。只能在 UEFI 里做。
7. **共享页的物理地址不是内核指针**：要 `ml_io_map_unmappable()` 映射，否则第一次读就
   `Kernel data abort`（我们真撞过一次 panic）。
8. **共享页写完必须 flush cache**：内核旁路缓存读，不 flush 时事件会攒在 loader 的 cache 里，
   直到别的内存活动把 cache line 挤出去——表现就是"点一下要等几秒才动"。

---

## 免责声明

- 本项目是**个人研究项目**，与小米、高通、Apple 均无隶属关系，也未获其授权或认可。
- **实验性质，按现状提供（AS IS），不提供任何明示或暗示的担保**，包括但不限于适销性、特定用途适用性和不侵权。
- 刷入任何引导镜像、改写 `userdata` 分区、修改分区表都**可能造成数据丢失、设备变砖、保修失效**。
  所有操作由设备持有人自行判断并承担全部风险；请先完整备份（含 GPT、`modem`、`persist`、`modemst1/2`、`fsg`）。
- 本项目**不是**可以日常使用的系统：电话、移动数据、音频、相机、蓝牙、充电、GPU 加速等均不可用或不完整。
- 本仓库**不包含**任何 Apple 二进制、iOS 系统镜像、厂商固件或密钥。iOS 材料请自行从 Apple 官方渠道获取，
  厂商固件请从你自己的设备提取；分发受各自许可证约束。
- 请勿将本项目用于商业用途，也请勿在任何非本人持有的设备上操作。
- 因使用本项目导致的任何直接或间接损失，作者不承担责任。

---

## 参考与使用的仓库

只列真正被用到的：

| 仓库 | 用途 |
| --- | --- |
| [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) | **内核基线**：XNU 补丁、`xnuloader` 引导器、UEFI 移植、驱动框架 |
| [oosexgang/hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) | **用户态**：init/stage0、注入库、launchd 任务、APFS 镜像构建、IPSW 提取 |
| [youzjuzi/dipper-xnu](https://github.com/youzjuzi/dipper-xnu) | **小米 8 的 SDM845 移植**，同 SoC 参考：A630、ST FTS 触摸驱动、edk2 集成、分区与载荷工具 |

---

## 致谢

- **[OOSEX GANG](https://github.com/oosexgang)** —— HackiOS 内核与用户态的全部基础工作：
  XNU 补丁、`xnuloader`、UEFI 移植、Adreno/Mesa 适配、以及那条"把 iOS 跑在安卓手机上"的路。
- **[dipper-xnu](https://github.com/youzjuzi/dipper-xnu)（小米 8）** —— 同 SoC 的移植记录：
  本项目的 ST FTS 触摸驱动直接源自它的 `mi8_fts.c`，A630 与 edk2 集成的经验也来自它。
- **[edk2-porting / Renegade Project](https://github.com/edk2-porting/edk2-sdm845)** —— SDM845 的 UEFI 固件
  （`perseus.dsc/fdf`、`SimpleFbDxe`、`UFSDxe`、`ButtonsDxe`），没有它这条启动链无从谈起；
  也感谢 TianoCore / EDK2 与 SimpleInit。
- **Apple** —— 开源的 [XNU](https://github.com/apple-oss-distributions/xnu) 内核源码。
- **Mesa / freedreno** —— Adreno 的寄存器表、UBWC 配置与 ir3 编译器。
- **Linux 内核（Qualcomm SDM845 分支）** —— 设备树、时钟/PMIC/触摸驱动的寄存器与初始化顺序。
- **Team Win / TWRP 与 dipper、perseus 维护者** —— 分区维护、格式化与镜像回读所用的 Recovery。
- **Theos / LLVM / cctools** —— 在 Linux 上构建 Darwin 内核与用户态所需的工具链。

---

## 许可证

- `xnu/` 下的补丁与新增文件（`perseus_fts.c` 等）是 XNU 的衍生作品，按 **APSL 2.0** 分发，与 XNU 一致。
- `xnuloader/` 的补丁同样是 XNU/HackiOS 的衍生作品，按 **APSL 2.0**。
- 其余脚本与文档：MIT。
- 第三方代码保留其原有许可证与版权声明。
