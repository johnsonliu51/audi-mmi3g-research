# USBMUX 单连接 C 会话核心

日期：2026-10-09。目标是把 DiPlay 的 USBMUX 单连接机制移到平台无关 C99；本交付没有 QNX USB I/O、Lockdown、TLS、iAP2 会话、NCM、AirPlay 或媒体输出，不能作为 CarPlay 接收器安装运行。

## 文件与来源

- `src/protocol/usbmux_session.h`：事件驱动 API、状态与缓存。
- `src/protocol/usbmux_session.c`：USBMUX v2 握手及一个 TCP 字节流。
- `src/protocol/usbmux_codec.c/.h`：使用已有的单帧解码、VERSION 与 SETUP 编码；未改动这两个文件。
- SPDX 为 GPL-3.0-only；完整许可证在 `src/protocol/licenses/DiPlay-LICENSE`。

固定上游提交：`858a7ee89c188f5c7d825fadbceb514f0bc73f7a`。

- [Iap2UsbMuxHost.kt](https://github.com/shihabal3amri/DiPlay/blob/858a7ee89c188f5c7d825fadbceb514f0bc73f7a/shared/src/main/java/com/shilapi/xcertplay/transport/Iap2UsbMuxHost.kt)：v2/SETUP、TCP 封装、连接和关闭。
- [UsbMuxFrameBuffer.kt](https://github.com/shihabal3amri/DiPlay/blob/858a7ee89c188f5c7d825fadbceb514f0bc73f7a/shared/src/main/java/com/shilapi/xcertplay/transport/UsbMuxFrameBuffer.kt)：增量拆帧及严格四字节尾部候选。

## 这次实现的范围

| 环节 | 实现 |
|---|---|
| 握手 | 发出 20 字节 VERSION；只接受 protocol=0、length=20、version=2；等待期间最多忽略 32 个先前 TCP 帧；随后发送 SETUP=7 |
| 建连 | `READY → SYN_SENT → OPEN`，校验端口与 SYNACK 的确认号；SYN 消耗一个序号；重复 SYNACK 不重复推进序号 |
| 收发 | 一个 TCP 字节流；最多一个未确认的数据段；最多 16 KiB/段，并限制到对端窗口；累积 ACK，序号按无符号 32 位回绕 |
| 增量帧 | 保留分片，逐帧处理合包；帧长度上限 65536；缓存 65540 字节以容纳严格限定的额外四字节边界 |
| 接收队列 | 16384 字节，写满时广告窗口降为 0，读取后发送窗口更新；溢出导致终止；重复和交叠数据不重复交付；乱序数据仅回复当前 ACK |
| 退出 | 主动 FIN、被动 FIN、同时关闭、RST；主连接最终退出时只关闭 transport 一次；有效已接收数据在正常关闭后可继续读到 EOF |
| 超时 | 握手/连接期限由调用方传入；写入 2000 ms，数据确认 5000 ms，关闭 5000 ms；调用方必须持续调用 tick |
| 记录 | 状态、协议号、字节数、flags、错误原因及回绕相关序号；不记录流负载、手机诊断文本、配对数据或密钥 |
| 清理 | `clear()` 关闭一次并用 volatile 写入清零所有缓存；失败后也必须调用 |

## 严格四字节回复尾部兼容

仅在此前 VERSION、已验证 TCP 或限定诊断帧允许尾部、当前边界非法时检查下一处 **恰好 +4** 的位置。候选必须是 `word8=0xfaceface`，并满足有效 TCP 头/非零端口，或完整、有限长度的 subtype=4 可打印 ASCII 诊断。候选不完整时保留缓存，非法时终止，不进行扫描式重同步。

合法的原边界优先。该处理参考 DiPlay 的捕获规则，不代表所有 iOS 版本都已兼容。此前 `PROVENANCE.md` 关于未移植 padding 的描述仍适用于原来的 `usbmux_codec` 单帧 API；兼容逻辑现位于新增会话层。

## 调用契约

1. 在堆或独立静态存储分配 `mmi_mux_session`，不要放到车机默认小线程栈。每实例约 97 KiB，内部不分配内存。
2. 提供 `write_all`、`close_transport` 和可选的 `diagnostic`。所有调用串行；回调不能重入这个会话。输入、输出和 session 内部缓存不得重叠。
3. `write_all` 必须循环处理实际 USB 短写，并在传入期限内结束；只有全部写完才返回 0。任何中断/失败都会使该会话永久失败并关闭 transport。
4. USB bulk 读取由平台适配层负责。获得的数据交给 `feed`；任何读取超时也调用 `tick`，让核心处理期限。时间来自同一个单调时钟；倒退的时间被拒绝。
5. `start(now, handshake_timeout)` 后等待状态 `READY`；再调用 `connect(source, destination, initial_sequence, now, connect_timeout)`。USBMUX 连接仅表示传输通道，不表示手机已经配对。
6. `send` 的 `accepted` 可能小于请求长度，调用方保留剩余数据。`AGAIN` 不缓存请求，也不产生新数据包，需要在收到确认/窗口更新后重试。零窗口没有自动探测包，外部控制器应设总操作期限。
7. `receive` 返回 `AGAIN`、`EOF` 或已读取数量。若窗口更新的写入失败，`received` 仍然报告已经复制给调用方的字节，不能按未读取再次处理。
8. 正常退出调用 `close`，继续读入和 `tick` 直到 `CLOSED` 或失败，再调用 `clear`。紧急退出可 `abort`，它尝试 RST 并终止。`clear` 后必须重新 `init` 且使用新的 transport。

只有已取得的 iPhone bulk pipe 才能传给此核心。`start`、`connect`、`send`、窗口 ACK 及关闭都会主动向手机写数据，所以本模块不属于只读诊断，不应在接口归属、并发访问及恢复方式未确认时自动执行。

## 单连接限制与 DiPlay 集成前的必补项

2026-10-09 后续新增了 [共享 host 核心](USBMUX_SHARED_HOST.md)，实现下述共享握手、全局序号及多连接生命周期。后续接收器集成应使用 `usbmux_host.c/.h`；本文件描述的单连接 API 保留为早期组件，其关闭 transport 的语义没有改变。

**当前 session 同时代表整个 USBMUX host 和它唯一的 TCP 连接。** 其 `close_transport` 终止的是 USB pipe，而不是仅释放一个端口。不能创建两个独立 session 共享同一个 pipe：它们会冲突使用握手、MUX 序号与接收流。

完整 DiPlay 流程需要在启动 carkit 服务后保持 secure Lockdown 连接存活，再使用返回的服务端口。因此不能用“关闭本实例中的 Lockdown、复用已关闭的 USB、再打开 carkit”的方式集成。

下一层必须拆成：

- 唯一的 USBMUX host，统一 VERSION、SETUP、mux sequence/ack、读线程/增量缓存及 USB pipe。
- 按源端口 demux 的有界连接表，各 TCP 连接独立的序号、接收队列、期限及 FIN/RST 生命周期。
- 关闭一个连接只释放其端口；只有 host 退出时关闭 USB transport。
- 独立 secure Lockdown/TLS 与 carkit 服务流，配对材料和清理必须另行实现。

本次没有多连接、完整 TCP 重传/拥塞算法、SACK、乱序缓存、keepalive、TLS 或 Lockdown 请求。USB 基础传输假定顺序可靠，数据 ACK/关闭超时直接失败，未自动重试。QNX 的 USB ownership、bulk 读写、中断退出、原厂 AMI 恢复也没有实现。

## 构建记录及验证边界

宿主 macOS 仅编译，产物置于非 iCloud 的临时目录：

```sh
cc -std=c99 -Wall -Wextra -Werror -pedantic -O2 -c src/protocol/usbmux_session.c -o /private/tmp/mmi3g-carplay-build/usbmux_session.o
cc -std=c99 -Wall -Wextra -Werror -pedantic -O2 -c src/protocol/usbmux_codec.c -o /private/tmp/mmi3g-carplay-build/usbmux_codec.o
```

2026-10-09 编译通过，无诊断。未运行单元测试、协议模拟或 iPhone 测试；未用 QNX SH4 SDK 编译、未部署到 SD、未执行车机代码。人工检查覆盖序号回绕、重复 SYNACK、FIN 重复和最终 ACK 交叉、一次性 transport 关闭，以及有界缓存；这些检查不替代运行验证。
