# 内核补丁

针对 HackiOS 内核基线（`oosexgang/hackios-xnu-kernel`，`xnu/redwood.patch` + `xnu/files/` 之后的状态）。

## 应用顺序

```sh
# 1. 按 HackiOS 的方式准备源码树（会 checkout xnu-8796.121.2 并打上红木基线）
xnu/apply.sh

# 2. 本仓库的补丁
cd xnu-src/xnu-8796.121.2
git apply ../../perseus-xnu/xnu/patches/0001-gic-timer-irq-argument.patch
git apply ../../perseus-xnu/xnu/patches/0002-touch-driver-in-files.patch
git apply ../../perseus-xnu/xnu/patches/0003-touch-driver-init-hook.patch

# 3. 新文件
cp -a ../../perseus-xnu/xnu/files/. .

# 4. 构建
cd ../.. && ./xnu-build.sh -j8 -k
```

`make` 最后返回 2 是 HackiOS 的已知现象（导出符号检查在内核链接之后才失败），
看 `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837` 的时间，不要看 make 的退出码。

## 每个补丁做什么

| 补丁 | 作用 |
| --- | --- |
| `0001-gic-timer-irq-argument.patch` | 把 EL1 虚拟定时器的 INTID 从写死的 27 改成启动参数 `gic_timer_irq`（默认仍是 27）。perseus 需要 19 |
| `0002-touch-driver-in-files.patch` | 把 `bsd/dev/perseus_fts.c` 编进内核 |
| `0003-touch-driver-init-hook.patch` | 在 `redwood_input_init()` 里调用 `mi8_fts_init()`（和 goodix/buttons 并列） |

## 触摸驱动的启动参数

| 参数 | 含义 |
| --- | --- |
| `fts=1` | 打开驱动 |
| `fts_steps=N` | 只做到第 N 步然后退出（1 时钟、2 引脚与供电、3 上电、4 引擎时钟、5 I2C 探测、6 运行） |
| `fts_noldo=1` | 跳过对 PM8998 L28 的 RPMh 请求，只开 GPIO79 的 I/O 轨 |
| `fts_watch=S` | S 秒后退出（0 = 一直跑） |
| `fts_orient=B` | 方向：bit0 镜像 x、bit1 镜像 y、bit2 交换 |
| `fts_raw=N` | 打印 N 条原始事件 |

**注意**：perseus 的 I/O 轨是 `disp_vddio_vreg` = GPIO79（小米 8 是 GPIO23）。
直接用小米 8 的条件，芯片不会上电，I2C 会一直读到低电平。
