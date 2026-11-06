# 引导器补丁（xnuloader）

针对 HackiOS 的 `xnuloader/`。这些改动在这一台机器上都是**必需**的，不是优化。

## 补丁内容

| 改动 | 为什么 |
| --- | --- |
| payload 支持第二个 ramdisk（`ramdisk1`） | stage0 需要 `md0` = flashboot、`md1` = root.apfs；只带一张盘时 stage0 报 `cannot mount /dev/md1` 后 halt |
| `boot_args.commandLine` 补 NUL，并把整个 `boot_args` 清零 | 该结构每轮都在同一物理地址且不清零，新参数后面会残留上一轮的字串，内核读到旧参数 |
| 看门狗静默阈值 50 s → 15 s | 挂死也能更快落盘 |
| 周期写盘改为带上内核日志环（`compose_live_log`） | 否则系统正常运行时 logdump 里只有 loader 自己的日志，内核日志要等挂死/返回才写 |

## 应用

```sh
cd xnuloader
git apply ../perseus-xnu/xnuloader/patches/0001-perseus-loader.patch
./build.sh
```

## 相关设备描述

`device/perseus.platform.c` 是 loader 里 `struct platform` 的 perseus 条目（GIC `0x17a00000/0x17a60000`、
8 核、`core_shift 8`、`small_cores 0xF0`、帧缓冲 `0x9d400000`）。把它贴到 `loader.c` 的
`platform_redwood` 旁边并加进 `platforms[]`。
