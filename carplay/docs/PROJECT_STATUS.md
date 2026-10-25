# 当前状态与最小可用版验收

更新：2026-10-10。目标仍为原厂中国版P0710、原厂AMI有线CarPlay、原屏/旋钮/音响；最终一个SD启动入口，包含日志与退出恢复。

最新交付：0.6已修复启动/工具兼容、采集过滤、进度读取、页面模板比较与清理路径，写入同一张SD卡并安全弹出。旧pdksh5.2.14与macOS ksh93各通过实际包29、进度/页面41、采集/摘要32、runner25项检查；新包部署7项通过。16个包文件读回一致，217个原用户文件哈希复核一致，只读FAT检查退出0。随后用户实车反馈仍未看到carplaydiag，当前仍在车上；尚未取得0.6返回日志，不能确认采集成功或确定菜单缺失原因。新反馈见CAR_TEST_20261010_MISSING_PROGRESS_PAGE_0.6.md，部署证据见SD_DEPLOYMENT_0.6_20261010.md。

上一轮实车反馈：0.5未出现carplaydiag，已插卡十几分钟；照片确认/main和SD1挂载路径/mnt/sdcard10t12。详见CAR_TEST_20261009_MISSING_PROGRESS_PAGE.md。

SD1详细页进一步显示Cardtype为secure digital、路径仍为/mnt/sdcard10t12。返回电脑后只读核对确认15个包文件未变、根状态仍prepared、logs为空；卡根新增本包空写探针目录，证明入口至少执行到mkdir。0.5将rmdir清理失败当作写探针失败并重试同一路径，是与现象高度一致的启动兼容缺陷；参考固件还存在printf可用性风险。0.5此次未采集成功、无菜单安装记录；失败现场已备份，后续0.6部署保留旧文件与空探针。详见SD_RETURN_INSPECTION_20261009.md。

## 本轮实际完成

- USBMUX单连接会话与共享host：版本/SETUP、SYN/ACK、序号/窗口、deadline和关闭；共享host提供4个独立端口连接，结构上支持同时持有Lockdown与carkit，未用手机验证。
- Lockdown长度帧通道：部分读写、时限、有界帧、EOF/错误、关闭和TLS升级所有权转移。
- 统一宿主构建协议库与接口探针，记录源码/产物哈希；协议库尚未运行测试。诊断脚本的宿主mock检查见下文。
- 目标ELF/依赖审计：校验SH4、PT_INTERP、loader、可执行入口与未版本化符号，防止把宿主产物或缺依赖程序当车机包；不等于调用ABI已通过。
- GNU官方binutils SH后端构建完成，可按PT_LOAD分析原厂机器码。
- 原屏路线定位到Toolkit的layer-manager注册→VFB→GF surface SID，避免直接占用原厂layer0。
- 新增只依赖真实SDK头文件的GF显示查询源文件；参考GF会写36/44字节info结构，必须先核对SDK布局。showScreen会操作固定displayable列表，未作为可逆演示直接部署。
- 已找到带H264 Baseline/Main任务标记的DSP载荷和解码类线索；不是实时解码成功证据。
- 已定位合法的Momentics SH SDK与Aviage媒体头文件来源；当前尚未取得本机可用安装包。
- 已生成0.4 SD诊断源与ZIP并写入用户现有Toolkit卡：180秒USB/进程/DSP/网络观察、240秒静态采集、完成/中断/根状态标记、条件式SD读写恢复、敏感字段过滤、真实显示配置链接处理及电脑摘要工具。宿主runner25项、采集摘要32项、部署7项检查通过；文件读回校验及只读FAT检查通过，卡已安全弹出。仍无目标机运行记录。
- 同日晚间升级为0.5原屏进度候选版并写入同一张卡：14项处理进度、真实USB查询退出码交接、原子快照、独立GEM查看器与单页安装/撤销。此次用户要求的进度包已包含ENABLE；只在现有EFS可写时新增一个自有ESD，不重新挂载EFS。34项进度/页面/runner集成检查、25项runner、32项采集摘要及7项新版部署检查通过；15个包文件读回一致，202个原用户文件复核一致。只读FAT检查退出0、卡已正常卸载并安全弹出。GEM发现/输出与P0710实车运行仍未验证。

本轮协议库宿主构建成功，产物与源码哈希在`/private/tmp/mmi3g-carplay-build/host/build-manifest.json`。当前0.6诊断ZIP SHA256：`623629a7d580a9fd98ebbc7ab9311e981e1d5a3ed39d97b13f94172fd496f6e0`；旧0.5/0.4 ZIP保留。诊断脚本仅在macOS模拟环境执行，并已实际写SD；接收器和车机程序未执行。最新写卡记录见SD_DEPLOYMENT_0.6_20261010.md，0.5历史记录见SD_DEPLOYMENT_0.5_20261009.md，上午0.4历史记录见SD_DEPLOYMENT_20261009.md。

## 还不能声称完成的功能

| 条件 | 当前状态 |
|---|---|
| SH4/QNX完整构建 | 缺qcc、SDK头文件、启动对象；现有产物为宿主平台 |
| USB/AMI适配 | 有描述符/API参考，尚无车机USB I/O、占用协调或回退实测 |
| Lockdown配对/TLS/carkit | 长度帧与共享多连接核心已有；plist解析、配对、TLS、服务启动及实际连接待实现 |
| iAP2 link/控制/认证 | 有CSM编解码；链路引擎、identification/MFi等待移植 |
| NCM/IPv6 | 有NTB16；控制协商、USB收发、QNX网络桥待实现 |
| AirPlay/加密/媒体会话 | 尚未移植成原生接收服务 |
| 显示/视频 | 有原厂VFB、DSP、GF接口线索；ABI、实时输入、首帧和恢复待验证 |
| 音频/麦克风/按键 | 有服务/接口参考；路由、事件映射和实测待完成 |
| accessory认证身份 | 未取得/使用可在当前iOS验证的身份或原厂认证服务 |
| 最终SD安装包 | 未形成；现有0.6为诊断/进度候选包，不含native接收器 |

## 外部输入

1. 可用且合法的QNX SH4 SDK及匹配Aviage开发头文件/安装包；固件只有运行环境，不能自动提供完整开发环境。
2. 本车USB/AMI/GF/DSP诊断日志与后续阶段实验记录；参考固件结果不能代表P0710的实际运行状态。
3. 可用的accessory认证途径。Lockdown信任、AirPlay身份和MFi身份不能互相替代。公开源码不包含可任意生成的Apple信任私钥。

## 最小可用版的完成证据

必须同时出现真实iPhone会话、原屏连续视频、原车声音、旋钮/确认/返回响应、正常停止与原厂恢复，并留有版本/日志/卸载记录。源文件数量、宿主编译或单个函数存在都不构成完成证据。

接下来取得开发依赖与本车日志，核查LM与媒体buffer ABI，构建GF查询及短时native显示/解码探针；在共享USBMUX host上继续补配对/TLS和AirPlay服务。未经这些阶段验证，不发布标为“能用的CarPlay”的包。获取方式与所需输入见DEVELOPMENT_INPUTS.md，SD操作见SD_DIAGNOSTICS.md。
