# CarPlay 所需条件与一次只读采集的覆盖范围

0.6包覆盖以下静态线索和180秒内的插拔观察。原始证据为inventory.txt/observation.txt，电脑端摘要只索引“元数据/未知”，不会把文件名字、字符串或USB变化标记为功能已通过。

| 条件 | 本包采集 | 仍需确认 |
|---|---|---|
| QNX/SH4 ABI | uname、pidin info、候选 loader/libc ELF 头 | 实际 SDK、链接与正常启动/退出 |
| USB host/AMI 路径 | io-usb短进程名、usb -vvv接口/endpoint、节点、AMI驱动/配置文件名、插拔快照 | AMI与原厂iPod层的对应关系、完整USB配置、bulk传输与占用归属 |
| USBMUX/iAP2 | 描述符与候选 USB 客户端库、DiPlay 可识别接口线索 | 控制通道、link 会话、真实收发、身份声明与认证 |
| NCM/IPv6 | 网络接口、路由、CDC/NCM 库名字线索 | iPhone NCM 配置、NTB 传输和 IPv6 socket 可用性 |
| GPU/屏幕 | GF 库、io-display、NVMiniRM、layer manager、驱动配置与字符串 | surface 创建/呈现、层权限、刷新、帧延迟、退出恢复原画面 |
| 视频解码 | io-media-nvidia、iom.cfg、DSP节点/finished标记、NvDspCodecs.elf元数据/cksum、API名字 | H264 profile/level、分辨率、持续码流入口、解码surface、帧率与资源消耗 |
| 音频播放 | io-audio/MMI3GMedia、音频节点、snd 库/配置 | PCM 参数、音源路由、音乐/导航/电话混音和结束恢复 |
| Siri 麦克风 | 音频节点与库、原厂电话进程线索 | 原车麦克风通道、可读 PCM、回声消除、会话传输 |
| 旋钮/按键 | 全部 /dev 节点名字、输入进程/库/配置线索 | 实际事件格式、DSI 订阅、焦点、HID 映射与按键响应 |
| 密码与认证 | crypto/ssl 库文件名 | 密码算法可用性、合法 accessory 身份、当前 iOS 接受情况 |
| SD 部署/卸载 | 挂载表、空间、包启动/完成标记 | native 包的启动、停止、恢复和卸载实测 |

## 为什么不能一次只读判定所有条件

AMI 音乐功能可能由原厂服务独占 USB 接口；查询枚举信息不能证明另一进程可占用接口。显卡与解码服务存在也不能证明可向其提交 AVC 码流。GF 字符串存在不能证明函数签名、权限和 layer 协议。认证能否成功还取决于身份材料与手机端。

因此本包尽量一次收齐接口分析所需的基础证据；后续原生探针才会实际调用 API。先测试短时画面与恢复，再离线样本解码/音频，最后在保存原厂状态的前提下测试 USB 会话。接口占用、音源切换与 layer 操作属于有状态实验，不能悄悄并入只读采集。

参考固件的符号/机器码已分析，目标机仍缺匹配SDK/媒体头文件与实际API运行记录。0.6不复制车机二进制，只取固定候选的头部与过滤后的API名字；若工具缺失则未知。新增GF查询源文件单独依赖真实SDK，尚未合进自动采集包。
