# 2026-10-21：QNX SH 编译路径打通

## 本次完成

1. iCloud 占位文件仍未全部下载。通过本任务历史创建命令恢复 iap2_link.c/h 到独立工作目录，未覆盖占位原件。恢复记录与字节数已保存；源码副本包含在构建包中。
2. Mac clang 对恢复的 iap2_link.c 执行 C99、Wall/Wextra/Werror 编译，成功生成 arm64 Mach-O 对象。没有执行该对象或协议测试。
3. 从 QNX 6.3.2 Linux 开发包提取 SH 所需工具、头文件及库。提取器 tools/extract-qnx632-sdk.py 按安装索引映射路径，对压缩内容解压并核对每个内容 MD5；未执行安装器或修改授权。
4. 创建独立 Debian amd64/QEMU 构建环境；仅监听本机 127.0.0.1:22293 的 SSH。SDK 工具是 Linux i386 程序，因此在虚拟机中添加了 Debian 32 位 libc 运行库。
5. qcc 报告 license check failed；没有激活或修改检查。改用其独立 GNU cc1 3.3.5、SH assembler/linker 组件构建，参数显式定义 QNX/SH 宏并使用实际 SDK 头文件、CRT、libgcc/libc。
6. 新建 src/qnx_runtime_check.c 与 tools/build-qnx-sh-gnu.sh。成功编译并链接 mmi-runtime-check；产物只依赖 libc.so.2，加载器 /usr/lib/ldqnx.so.2。虚拟机和临时 HTTP 服务已在 sync 后停止。

## 产物

artifacts/MMI3G-QNX-SH-Build-0.1.zip，29,761 bytes。
SHA256：46517b66dcbb946f244a58ecf65779e9f58c2b393666de9db0c8a60e1da905e2。

- mmi-runtime-check：11,504 bytes，ELF32 little-endian / EM_SH / ET_EXEC。
- iap2_link.o：6,372 bytes，SH relocatable object。
- qnx_runtime_check.o：2,064 bytes，SH relocatable object。
- source/：实际编译的源文件与构建脚本。
- build-manifest.json：源码/产物 SHA256、格式与未验证状态。
- COPYING：GNU GPLv3 文本。

运行程序只打印 uname、指针大小和设备路径 stat 结果，并分配/释放空闲 iAP2 状态机；没有打开 USB、读取按键流、接管图层、加载媒体库或启动 CarPlay。它是构建与运行时诊断候选，不是安装包。

## 尚未解决的条件

- 旧 assembler 根据指令选择的 ELF flags：可执行文件 0x8，两个对象为 0x2/0x1；与此前参考固件的 0x9 不同。未改写文件头伪装匹配。此前严格参考格式检查仍会拒绝这些 flags；需要核实 SH 指令子集及目标加载器兼容性。
- 编译环境来自 6.3.2，P0710 的精确运行库/调用 ABI 没有验证。
- 未在 QNX、模拟车机或实车执行，没有运行实现测试。
- 全部旧模块源码并未恢复；本次成功范围为新链路模块及最小运行时诊断。
- 没有完整 USB 后端、附件身份、认证和 CarPlay 会话、视频解码、音频及显示输入适配。
- 没有 SD 自动启动/安装脚本，没有修改 SD、车机固件或全局系统配置。

## 工作数据和可重建入口

基础目录：本机私有的 MMI3G-CarPlay 构建目录（不随公开仓库分发）。

- qnx632-sdk/：提取的 SDK 文件与提取清单。
- sdk632-media/：原始 JAR 与安装索引。
- recovered-source-20261021/：从任务命令记录恢复的源码。
- linux-build-vm/：Debian overlay、netboot kernel/initrd、附加存储驱动、SSH 密钥和本机 known_hosts；私钥不在交付 ZIP 中。
- builds/20261021/qnx-gnu/：产物及完整来源清单。

SDK 下载源是第三方历史镜像，不是厂商签名认证的供应链；本次 MD5 用于检查安装包内容内部一致性。镜像 URL：https://fsck.technology/software/QNX/QNX%206/QNX%20Momentics%20Development%20Suite%206.3.2/6.3.2-linux-200709062316.iso 。镜像 SHA256 见产物清单。

虚拟机本次使用 Debian netboot initramfs 启动 shell，加载 extra-modules 中的 virtio_blk/ext4，挂载 /dev/vda1 到 /target，再绑定 dev/proc/sys 并 chroot 启动仅公钥认证 SSH。运行工具所需的 32 位库只在客体中修改；未修改 Mac 的 libc。

GNU 构建命令（在具备 Linux i386 运行条件的构建环境中）：

    QNX_HOST=/opt/qnx632/host/linux/x86 QNX_TARGET=/opt/qnx632/target/qnx6 \
      sh tools/build-qnx-sh-gnu.sh /path/to/source /path/to/output

构建脚本不运行输出程序。不要将此 ZIP 解压到 Toolkit 卡当作 CarPlay 安装程序。

## 来源

- QNX 6.3.2 gcc 文档：https://qnx.com/developers/docs/6.3.2/neutrino/utilities/g/gcc.html
- QNX 6.3.2 qcc 文档：https://qnx.com/developers/docs/6.3.2/neutrino/utilities/q/qcc.html
- QNX ldrel 文档：https://www.qnx.com/developers/docs/6.3.2/neutrino/utilities/l/ldrel.html
- DiPlay 来源和许可见 source/src/protocol/iap2_link.c 头部及现有 IAP2_LINK_PORT.md。
