# 2026-10-21 构建尝试

已将 iap2_link.c 加入 tools/build-carplay-core.py 的协议库构建清单，并增加 macOS dataless 源文件检测，防止 iCloud 占位文件使读取和编译无限等待。

QNX 构建实际返回：`tool unavailable: qcc`。本机 PATH 与 Spotlight 未找到可用的 QNX SH 编译器。调用中的 gcc_ntoshle 只是候选参数，尚无 SDK 可确认其可用性。没有生成目标 ELF。

本机 clang 编译读取头文件时等待 iCloud 下载；已经请求逐文件下载。完整 host 构建明确报告未下载的源文件，未成功完成；等待中的单文件编译已停止。没有运行测试或修改 SD 卡。

构建记录原存于本机私有构建目录，不随公开仓库分发。

下一步需要恢复源码本地可读，并准备匹配 QNX SH 目标的编译器、链接器、启动文件与头文件/库。即使协议库成功编译，也不包含完整 USB 后端、认证身份、会话启动及媒体链路，不能当作可用 CarPlay 安装包。
