# 许可证

本仓库混合了不同来源的代码，各部分遵循其原始许可证：

| 路径 | 许可证 | 说明 |
| --- | --- | --- |
| `xnu/patches/`、`xnu/files/` | **APSL 2.0** | XNU 的衍生作品，与 Apple 开源的 XNU 一致 |
| `xnuloader/patches/` | **APSL 2.0** | HackiOS loader 的衍生作品（其本身也是 APSL） |
| `device/`、`docs/`、其余脚本 | **MIT** | 本项目的板级描述与文档 |

`xnu/files/bsd/dev/perseus_fts.c` 源自 [dipper-xnu](https://github.com/youzjuzi/dipper-xnu) 的
`mi8_fts.c`（小米 8，SDM845），按 perseus 的供电脚与量程修改；上游同样是 XNU/HackiOS 的衍生作品。

第三方代码保留其原有版权与许可证声明。

## 不含 Apple 材料

本仓库**不包含**任何 Apple 二进制文件、iOS 系统镜像、dyld 共享缓存、厂商固件或密钥。
iOS 材料由使用者自行从 Apple 官方渠道获取，厂商固件由使用者从自己的设备提取。
