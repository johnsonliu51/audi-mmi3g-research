# 复现路线与当前缺口

历史阶段记录截止 2026-09-17；2026-10-26 整理发布。本次仅核对记录，未重新运行 QEMU 或车机。

## 固件解析

需要自行合法取得 K0942 HN+_EU_AU3G_K0942_3 / MU9498 的原始固件。
使用支持该 RAR 过滤器的 unrar 解压；bsdtar 曾出现“可列目录但解出零填充文件”的失败情况。
核对 metainfo2、非零 IFS 头 `eb 7e ff 00`、EM_SH 与 stored_size，不能对失败占位文件继续分析。
历史上 mmi-ifs 的 Python 2 脚本经隔离适配并链接 LZO 后，解析出 671 个压缩块。

## 模拟器阶段

历史 QEMU 上游基线提交：`ff1d2d19d7e24893e2012d879f8e73077e17b9bd`。
必须额外取得本项目定制的 SH7785/Audi 硬件模型与补丁；仅检出上游提交不能复现。
当前这部分源码尚未公开归档核验完毕，因此这里不是完整的一键构建教程。

真实链路曾到达 dev-ipc → devg-NVMiniRM/devg-NVMEM → io-display → devg-nv.so/libned.so.6 → MMI3GApplication/layermanagerV2。
历史日志中的 `SetVideoMode: 800 x 480, 32 bpp` 后仍有 `gf_dev_attach() failed!`。

## 下一步验证顺序（计划，不是已成功）

1. 以一次性 flash 副本恢复 devf-generic → checkPersistence → inflator → efs_symlinks。
2. 按配置满足 NvElfLoader → devg-NVTS/GPIOSet → io-media-nvidia → io-display 的依赖。
3. 证明媒体服务实际发布 nMSDKIPC，检查 NED 的资源打开返回值。
4. 证明 gf_dev_attach 成功，取得真实 surface 的 stride/format 和像素读回。
5. 最后才测试固件 TEST(18)/RDK 路径，不把手工重绘当运行时帧。

真实车辆胎压还需要 J502/地址65、传感器和有效网关/CAN 数据；固件资源不能凭空提供 bar/psi。

## 已知不可复现门槛

旧实验曾丢失一份 full-EFS payload。历史哈希可以识别它，但不能从哈希还原文件。
在定制源码与原始输入完整可读前，不保证精确重建同一实验二进制。
