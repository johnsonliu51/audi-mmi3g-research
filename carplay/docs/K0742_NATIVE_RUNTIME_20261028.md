# K0742 最小原生诊断：目标运行证据

2026-10-28：本次只复核已返回的实车采集记录，没有重新运行车机或执行部署。公开的是脱敏结论，不分发 SD 备份、驱动、SDK、原厂模块或构建二进制。

## 验证环境与方法

回传的 `train.txt` 与 `status.txt` 均标记 `HN+_CN_AU3G_K0742`。对照同一运行目录的 `native.txt`、`status.txt`、`driver-load.txt`、`network-final.txt`，区分程序自身输出、包装器退出码及后续网络实验。

## 已确认的原生运行边界

| 证据 | 可支持的结论 |
|---|---|
| `MMI runtime check 0.2 - NOT A CARPLAY RECEIVER` | 目标环境产生了该最小原生诊断的标识输出 |
| `pointer_bytes=4` | 诊断报告 32 位指针宽度 |
| 包装器 `native_exit=0`，程序输出 `complete=1` | 该次最小检查走完并返回成功 |
| `protocol_state=0`、`usb_opened=0`、`carplay_session=not_attempted` | 仅检查空闲协议对象；没有启动真实手机或 CarPlay 会话 |

检查范围为 libc 启动、标准输出、若干路径的 access 检查、空闲协议对象分配/释放与退出。设备路径可访问不等于已打开、调用成功或完成设备功能。未测试绘图、视频、音频或输入事件 API。

因此，“最小原生诊断没有任何目标运行证据”已不再适用于这个 K0742 样本；但这不是通用 ABI 兼容证明，也不外推到历史 P0710 或其他固件。

## 同次网络实验的失败证据

用户对该次采集的说明为未接入 USB 网卡。普通 RAM 文件写入、驱动暂存成功；`driver_load_exit=1`，mount 输出 `No such process`。随后 ifconfig 输出 `Address family not supported by protocol family`。

加载前的记录包含 `io-pkt-v4-hc`。上述现象提示网络栈可能在加载操作后退出或不可用，但没有完整的加载后进程快照或崩溃证据，不能据此确定根因。尤其不能在网卡未接入的情况下宣称 ASIX 兼容或不兼容。

## 后续候选与未完成项

本地后续 0.3 采集候选已暂停主动驱动加载、地址配置和 shell 启动，转为原生检查及被动 USB/网络观察。其准备、写卡和读回校验不是新一次目标运行验收；本次未取得 0.3 的实车结果。

MMI USB 网卡链路、电脑到车机的 shell、真实 iPhone 会话、原屏视频、原车音频及控制均未获验证。完整 CarPlay 接收器仍未实现。
