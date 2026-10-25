# Lockdown 传输移植

2026-10-09。新增 src/protocol/lockdown_channel.c/.h，GPL-3.0-only，参考 DiPlay 固定提交858a7ee89c188f5c7d825fadbceb514f0bc73f7a的transport/LockdownPlistChannel.kt。

已实现连接后的plist长度帧传输：4字节大端长度、部分读写、单调时钟总期限、有界长度、EOF/超时/错误、关闭与TLS升级所需detach所有权转移。每次读取恰好一个帧，不预读后续TLS数据。传输失败关闭当前通道，不能继续使用半帧状态。默认建议maximum=1MiB，硬上限16MiB。

接口由USBMUX流或TLS适配器提供；读写回调必须遵守期限，调用者串行执行请求。不实现USB访问、配对、plist语义、证书签发、TLS或carkit服务启动；不能作为完整Lockdown客户端宣称成功。

payload是opaque bytes，必须使用有界plist解析器处理响应，不允许XML实体扩展或外部网络取资源。日志不记录PairRecord、HostID、证书/私钥或完整响应。

对应的完整DiPlay路径仍需：读取设备公钥/生成本地主机配对记录→手机信任与Pair→StartSession→按EnableSessionSSL升级TLS→StartService(com.apple.carkit.service)→打开返回端口并按EnableServiceSSL决定是否升级TLS→保留原Lockdown通道直至carkit关闭。

缺少其中任一步都不能将CSM裸写到USB或冒充CarPlay启动。目标QNX TLS/配对材料保存与设备认证需要另行实现并审查。
