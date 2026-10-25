# 原生运行诊断：第一步结果与第二步候选包

日期：2026-10-21。

## 已完成

- 原有协议源码已恢复为本地可读。八个模块在 Mac host 构建和 GNU QNX 3.3.5 SH 构建中均编译成功：iap2_csm、ncm_ntb16、usbmux_codec、lockdown_channel、avc_nal、usbmux_session、usbmux_host、iap2_link。
- 修复 avc_nal.c 的旧 GCC 有符号/无符号比较警告，将掩码 31 写为 31u；不改变 NAL 类型判断。
- GNU SH 构建产出 libmmi_carplay_core.a（40,338 bytes）及 mmi-runtime-check（11,324 bytes）。这是协议库与诊断，不是完整接收器。
- 新版诊断不再使用 stat/utsname 结构体，只打印路径 access 检查结果和本地空闲协议对象的分配/释放结果；不启动握手。
- 对照本地已有 K0942 IFS41 的 libc.so.2，诊断所有强符号均能按名称解析。参考 libc SHA256：57060bfc7892f3b62cdbaf9ee051a5119e2bb2b175b186ccee02d3aa17f5cf99。
- 新诊断只依赖 libc.so.2，解释器 /usr/lib/ldqnx.so.2，入口位于可执行 PT_LOAD 内；代码段与可写数据段分开。
- 已建立可重复启动的专用 Linux 构建 VM 入口 tools/qnx-build-vm.py。本次构建结束后已 sync 并停止 VM。

## 架构标志结论

GNU SH ELF 定义将 0x8 标为 SH3E、0x9 标为 SH4。当前旧版 assembler 产出的可执行文件为 0x8，参考 libc 为 0x9；仅发现差异不能证明不兼容，也不能据此宣称可运行。未改写 ELF 头，仍保留该差异作为目标加载器验收条件。

来源：https://raw.githubusercontent.com/RTEMS/sourceware-mirror-binutils-gdb/master/include/elf/sh.h

## 第一阶段边界

静态符号名称检查只满足一个必要条件；K0942 参考固件不是本车 P0710。P0710 调用 ABI、加载器对标志的接受情况和实际运行仍未验证。没有执行目标程序、协议测试或车机实验。

## 第二步候选包

artifacts/MMI3G-Native-Runtime-Experiment-0.1.zip

SHA256：fd4086af622b3577e1d79b5387cafd3579beb57e381bb500c0adcbac730123b9

准备的启动链：encoded copie_scr.sh → SD 写启动记录 → native-runtime/start.sh → 原生诊断 → 保存 stdout/stderr/退出码。

该包不创建 carplaydiag 菜单、不接管显示或按键、不加载媒体库、不打开 USB、不写 EFS。它会替换 SD 根目录 copie_scr.sh，因此必须先对当前卡的真实入口做备份；不能用旧备份覆盖新卡，也不能把原 Toolkit 的全套安装 run.sh 串起来重新执行。旧 run.sh 会 remount EFS 并安装模块，不适合作为此次最小实验的前置动作。

成功阶段的含义：

| 记录 | 说明 |
|---|---|
| MMI_NATIVE_BOOT.txt: encoded-entry-called | 编码启动入口已执行到 SD 日志写入处 |
| shell-entered | 独立包装脚本已进入 |
| native-start-requested | 开始尝试加载原生程序 |
| native-returned-zero | 本诊断退出码为 0，不代表 CarPlay 成功 |
| stderr.txt 与 native_exit | 原生加载或运行失败的实际输出 |

没有启动记录仍可能是入口未执行、SD 不可写、挂载或路径条件未通过，不能从没有菜单推断是哪一种。

下一步需要将当前 Toolkit SD 卡插回电脑，读取当前入口和已有日志，再备份与部署。当前没有挂载这张卡，尚未写入卡或进行实车运行。先不要求用户上车。

编码方案参考固定 Toolkit 提交 e9ca3469835e3e1c78dabfb5cbe5676d2b752500，builder/encoder.py 与 core/copie_scr_plain.sh。候选入口沿用原 Toolkit 的 SD remount 方式，不 remount 内部文件系统。
