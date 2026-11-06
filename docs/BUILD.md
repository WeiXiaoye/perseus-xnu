# 构建

本仓库**只包含补丁、新驱动和板级描述**。要编出内核，还需要 HackiOS 的基线树和一套 Linux 工具链。

```
perseus-xnu/                     ← 本仓库（补丁 + 新文件）
hackios-xnu-kernel/              ← 基线：xnu/apply.sh、redwood.patch、xnu/files、xnu-deps、xnu-build.sh
hackios-xnu-userland/            ← 用户态与系统镜像（本仓库不涉及构建内核，但启动需要）
```

## 0. 主机要求

- **Linux x86-64**（Ubuntu 24.04 实测；不需要 macOS，也不需要 Xcode）
- `clang`、`lld`、`python3`、`perl`、`git`、`xz`、`flex`、`bison`、`m4`、`make`
- **Theos**（提供 cctools 与 `iPhoneOS16.5.sdk`）
  ```sh
  git clone --recursive https://github.com/theos/theos.git ~/theos
  # 需要 ~/theos/sdks/iPhoneOS16.5.sdk 与 ~/theos/toolchain/linux/iphone/bin
  ```
- 磁盘约 2 GB；内存 4 GB 以上

## 1. 基线

| 项 | 值 |
| --- | --- |
| XNU | `apple-oss-distributions/xnu`，tag `xnu-8796.121.2`，commit `c54f35ca767986246321eb901baf8f5ff7923f6a` |
| HackiOS | 红木（redwood）基线：`xnu/redwood.patch`（73 个文件）+ `xnu/files/`（104 个文件） |
| `xnu/redwood.patch` 的 sha256 | `ac3b2b578a6d1d480079557a576bf1bb4155082d4ee2958373c18f5d78b8e89b` |
| `xnuloader/loader.c`（补丁前）的 sha256 | `52cc604309d7f3587d5a3276a24e08501356852027b0d1945a54ac59ea2aeb9d` |
| 实测工具链 | Ubuntu clang 18.1.3 + `ld64.lld` |

HackiOS 上游改过目录结构（`common/` → `devices/`、`hk_*` → `hakios.*`）。本仓库的补丁是针对
**红木命名的那一版**做的；`scripts/build.sh` 会检查 `redwood.patch` 的 sha256，不一致时会警告。

## 2. 一条命令

```sh
git clone https://github.com/WeiXiaoye/perseus-xnu.git
git clone https://github.com/oosexgang/hackios-xnu-kernel.git

export HACKIOS=$PWD/hackios-xnu-kernel
export THEOS=$HOME/theos
sh perseus-xnu/scripts/build.sh            # 内核
BUILD_LOADER=1 sh perseus-xnu/scripts/build.sh   # 内核 + loader.efi
```

脚本做这些事：

1. 校验基线指纹；
2. `xnu/apply.sh`（拉 Apple 的 tag、打红木补丁）；
3. 依次 `git apply` 本仓库的 3 个补丁、把 `xnu/files/` 覆盖进树里；
4. `xnu-deps/setup.sh`（依赖源码、mig/unifdef 等）；
5. `./xnu-build.sh -j8 -k`；
6. 可选：给 loader 打补丁、插入 `platform_perseus`、`xnuloader/build.sh`。

产物：

```
hackios-xnu-kernel/xnu-src/xnu-8796.121.2/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837
hackios-xnu-kernel/xnuloader/loader.efi
```

## 3. 手动步骤（脚本失败时）

```sh
cd "$HACKIOS" && xnu/apply.sh
cd xnu-src/xnu-8796.121.2
git apply ../../perseus-xnu/xnu/patches/0001-gic-timer-irq-argument.patch
git apply ../../perseus-xnu/xnu/patches/0002-touch-driver-in-files.patch
git apply ../../perseus-xnu/xnu/patches/0003-touch-driver-init-hook.patch
cp -a ../../perseus-xnu/xnu/files/. .
cd ../.. && xnu-deps/setup.sh && ./xnu-build.sh -j8 -k
```

## 4. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `make` 返回 2，但内核文件存在且刚更新 | 正常。导出符号检查在链接之后才失败（`config/Makefile:108`），看文件时间，不看退出码 |
| `xnu/apply.sh: ... already exists` | `xnu-src` 已存在，脚本故意不覆盖。要重来先移走旧目录 |
| 补丁 `does not apply` | 基线版本不对（见 §1 的 sha256）；HackiOS 上游改过命名与目录 |
| `no such file: iPhoneOS16.5.sdk` | `THEOS` 没设或 SDK 没装 |
| `perseus_fts.c: PIN_VDDIO undeclared` | 说明补丁顺序错了：`0001` 里的定义必须在使用之前 |

## 5. 本仓库**不**提供的东西

- **内核与 loader 的编译产物**：不提供，自行编译。
- **iOS 系统卷**：需要你从 Apple 的 IPSW（iPhone X，iOS 16.5 / 20F66）自行提取，本仓库不含任何 Apple 文件。
- **厂商固件**：GPU（`a630_*`）、Wi-Fi（WCN3990）等必须从你自己的设备提取。
- **刷机镜像与脚本**：属于设备侧工作，见 `docs/status-2026-11-06.md` 的说明。
