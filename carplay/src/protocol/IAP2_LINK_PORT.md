# iAP2 链路 C 移植首版

来源：DiPlay 24ae1fa52edbe27392a668d4c9e71a32d5691986 的 Iap2LinkEngine.kt，GPL-3.0。上游版权归原贡献者；本文件记录派生范围。源码地址：https://github.com/shihabal3amri/DiPlay/blob/24ae1fa52edbe27392a668d4c9e71a32d5691986/shared/src/main/java/com/shilapi/xcertplay/transport/Iap2LinkEngine.kt

新增 iap2_link.c/h，独立于 USB、线程和媒体。实现检测标记、SYN/ACK 协商、分片输入、校验和、控制会话 10、单包发送窗口、重复接收确认、超时及 EAK 触发重传、EOF/reset 错误。缓冲分配在堆上约 192 KiB，不放入小线程栈。

首版主动只通告一个接收窗口和会话 10。没有照搬上游 30 包窗口、乱序队列、EA/文件传输会话；不能作为等价完整移植。发送返回 1 表示等待确认，上层须保留待发送数据。输出回调必须复制到有界队列，不能在回调里重入状态机。传输部分写入由队列驱动完成，不能误作整帧发送成功。调用者还必须设置总握手超时，当前握手定时器会持续重试直到调用者关闭。

尚未编译或运行测试，也未连接 iPhone、QNX USB 或实车。无认证身份，无 CarPlay 会话、视频或音频。后续需要验证序号/ACK 边界与对端重协商，再接入平台字节流、认证及识别阶段。不要将此代码加入 SD 安装包并标记可用。
