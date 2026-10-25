# 协议核心来源与范围

本目录采用 GPL-3.0-only。C 实现参考 DiPlay 项目的 Kotlin CSM、NTB16、USBMUX、Lockdown framing和AVC转换逻辑；不包含 accessory 身份、密钥或 Android 平台代码。上游 DiPlay 继承 xcertplay，详见其 THIRD_PARTY_NOTICES.md。

avc_nal.c/.h 参考同一提交 media/MediaCodecSupport.kt 的toAnnexB，实现length-prefixed AVC→Annex B；显式输入1/2/4字节长度字段，先检查完整数据和容量，再写输出。没有自动判断Annex B或H264解码。不得据此声明首帧渲染。

2026-10-09新增avcC基础解析（参考同文件avcParameterSets）。输出profile、level、NAL长度字段和全部SPS/PPS的Annex B初始化数据。强制检查参数集NAL类型、缺项与边界；profile扩展保留未解析，通过parsed_bytes提示调用者。仍无H264解码、尺寸/SPS语义解析、首帧或QNX实机验证。

2026-10-09新增USBMUX单连接会话核心和Lockdown长度通道。分别见 docs/USBMUX_SESSION_CORE.md、docs/LOCKDOWN_PORT.md。均参考同一DiPlay提交，GPL-3.0-only。不含Pair/TLS、iAP2 link与认证；仅具备独立模块还不能建立iPhone会话。

随后新增usbmux_host.c/.h，共享一次VERSION/SETUP与全局MUX序号，4个独立端口连接及句柄代次，单连接FIN/RST/超时不会直接关闭USB；底层I/O失败终止整个host。接收器后续应使用此模块同时持有Lockdown与carkit，不与旧session共用一个transport。见docs/USBMUX_SHARED_HOST.md。仍不包含USB驱动、TLS或配对，不声称已连接真实iPhone。

新增 ncm_ntb16.c/.h 参考同一提交 transport/Ntb16Codec.kt，GPL-3.0-only；完整许可证保存在 licenses/DiPlay-LICENSE。它只编解码完整 NTB16，不建立 USB/NCM 网络。详见 docs/DIPLAY_RECEIVER_IMPLEMENTATION.md。

新增 usbmux_codec.c/.h 参考同一提交 transport/Iap2UsbMuxHost.kt 和 UsbMuxFrameBuffer.kt，GPL-3.0-only。实现 VERSION 消息、SETUP/TCP envelope 编码及帧头/长度校验。没有 TCP 会话状态、重传、Lockdown/TLS 或 USB I/O。VERSION 的 word8 是版本号，与后续包 magic 不同。输入/输出不得重叠。

独立codec不处理iOS回复额外四字节padding。session/host已实现依据上一帧及严格+4候选的有限兼容；非法边界仍报错，不扫描丢弃未知字节。codec调用方需保留不完整帧并记录错误；frame借用调用者输入缓存，输入变更后失效。

固定参考提交：858a7ee89c188f5c7d825fadbceb514f0bc73f7a。

- https://github.com/shihabal3amri/DiPlay/blob/858a7ee89c188f5c7d825fadbceb514f0bc73f7a/shared/src/main/java/com/shilapi/xcertplay/iap2/wire/Iap2CsmFramer.kt
- https://github.com/shihabal3amri/DiPlay/blob/858a7ee89c188f5c7d825fadbceb514f0bc73f7a/shared/src/main/java/com/shilapi/xcertplay/iap2/wire/Iap2Wire.kt
- https://github.com/shihabal3amri/DiPlay/blob/858a7ee89c188f5c7d825fadbceb514f0bc73f7a/docs/THIRD_PARTY_NOTICES.md

CSM 格式：大端 0x4040、16 位总长度、16 位消息 ID、参数体。CSM 本层没有 checksum。它不是 iAP2 link packet，也不是 USBMUX packet；输入必须先去除这些传输层。

CSM模块包括流分片/粘包处理、起始标记重同步、有界65535字节缓存、参数长度检查和参数顺序/重复ID保留。CSM本身不实现link SYN/ACK/retransmission、USBMUX、认证、NCM、AirPlay、视频或音频。

inspect 工具只输出消息 ID/参数 ID/长度，不输出参数值。格式有效不代表消息语义正确，协议分析必须使用本机合法取得的控制会话数据。

宿主构建示例（产物放在非 iCloud 临时目录）：

```sh
mkdir -p /private/tmp/mmi3g-carplay-build
cc -std=c99 -Wall -Wextra -Werror -O2 src/protocol/iap2_csm.c src/protocol/csm_inspect.c -o /private/tmp/mmi3g-carplay-build/csm-inspect
```

2026-10-08 已按上述命令完成 macOS 宿主编译，开启 Wall/Wextra/Werror，无编译诊断。产物 `/private/tmp/mmi3g-carplay-build/csm-inspect`。

QNX 构建需匹配的 SH4 SDK；候选 `qcc -Vgcc_ntoshle` 必须由实际 SDK 配置与参考 ELF 核实。宿主可执行文件不能放到 MMI 运行。本次未进行单元测试、QNX 构建或 iPhone 实机测试。
