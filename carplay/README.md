# MMI 3G 原生 CarPlay 研究（未完成接收器）

2026-10-26 发布。目标为 QNX/SH 平台上的原屏、有线 CarPlay 研究，不是可用安装包。

## 本次公开内容

- 八个协议模块及头文件：iAP2 CSM、iAP2 link、NCM NTB16、USBMUX codec/session/shared host、Lockdown framing、AVC NAL 转换。
- 仍可完整读取并已筛查的构建/ELF 审计/诊断打包工具与阶段报告。
- 协议实现的来源说明和 GPL-3.0-only 许可证，见 `src/protocol/PROVENANCE.md`、`IAP2_LINK_PORT.md` 和 `licenses/DiPlay-LICENSE`。

部分历史文件仍是 iCloud 未下载占位文件，尚未纳入此次发布。因此本目录不是原项目完整镜像；部分工具引用的探针源文件、SD 模板、vendor 编码器或测试可能缺失，不能保证直接调用所有工具均可运行。

## 已验证与未验证

历史记录表明协议核心曾完成宿主构建和 GNU QNX SH 构建；本次另对这里公开的八个模块进行 macOS 宿主严格编译检查，结果见 PUBLISH_VERIFICATION.md。
QNX 候选产物 `e_flags=0x8` 与 K0942 参考 `0x9` 的差异仍需解决/验证，不能修改标志假装兼容。P0710 ABI、目标 loader 和实机运行未验证。
历史 SD 诊断曾完成宿主 mock/部署校验，但实车用户反馈未出现预期诊断菜单；新的最小原生诊断未取得目标运行证据。

尚缺真实手机 USB/AMI I/O、配对/TLS、accessory 认证、完整会话及媒体链路。没有真实 iPhone 会话、首帧、原车声音和旋钮输入的联合验证，不能宣称 CarPlay 可用。

## 安全与复现

先读 `docs/PROJECT_STATUS.md`（2026-10-10 历史快照），再读 `docs/NATIVE_RUNTIME_STAGE1_20261021.md`（更新阶段）。旧阶段文档的“缺编译器”与后来的 GNU 构建是不同时间点，不能混读为当前冲突结论。
只在隔离目录进行宿主编译；不要运行部署脚本、覆盖 SD 卡入口或写 EFS，除非已核对当前卡内容并有独立备份及用户授权。
原厂固件、SDK、认证凭据、内存转储和构建二进制不随仓库分发。

## 许可证

`src/protocol` 按其原有 GPL-3.0-only 声明与来源保留。iAP2 link 的参考提交与其他模块不同，详见各文件头和 IAP2_LINK_PORT.md。
其余研究资料和工具保留原有权利，不因本次发布而擅自统一改为另一许可证。上游名称、商标和第三方代码权利归各自所有者。
