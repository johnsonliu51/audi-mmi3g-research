# 2026-10-10：0.6 仍未显示 carplaydiag

## 已收到的实车反馈

0.6 已写入原 SD 卡、读回校验并安全弹出后，用户反馈：“不行，绿菜单上依旧没有carplaydiag”。随后确认仍在车上。

这次只能确认用户没有看到进度菜单。尚未收到本轮照片或 SD 日志，不能判定入口、采集器或页面安装是否运行，不能认定采集成功。宿主兼容检查通过不构成 P0710 菜单或采集实测通过。

本轮检查时电脑 `/Volumes` 没有 SD 卡。未修改部署包、原卡或车机。

## 只读静态复核

0.6 的页面模板仍为 `screen CarPlayDiag Main`，使用 `script/value/label` 和小写 `.esd` 扩展。固定 Toolkit 参考源码中的 ScreenParser/ScriptParser/EngineeringFileFilter 接受这种格式；页面名转小写，父页链接由 parser 自动生成。未发现确定的语法或手动菜单链接遗漏。

参考 ScreenRenderer 在 `connected()` 时启动 parser 扫描，普通页面间跳转不重新扫描。0.6 安装脚本没有主动触发 GEM 重载。因此，页面已经写入但尚未加载是一个待核查分支；完整退出绿色菜单再重新进入只能作为一次低风险观察，不能保证 P0710 会重新连接或重新扫描。

参考启动配置与符号链接将 GEM 页面目录指向 `/mnt/efs-system/engdefs`，与当前安装目标一致。P0710 的实际映射、目录写入结果与菜单发现仍需日志证据。

当前安装器不会重新挂载 EFS。创建失败记录为 `write_failed_readonly_or_unavailable`，该状态还可能包含路径、权限或其他创建错误，不能直接解释为已证实只读挂载。

## 现场操作与回卡检查

已告知用户：完整退出绿菜单回普通 MMI 界面，再进入检查一次；插卡满 6 分钟即可取卡带回电脑，无需继续等待或反复找菜单。

SD 返回后先复制并只读检查：

- 根目录 `MMI3G_DIAG_BOOT.txt`、`MMI3G_DIAG_STATUS.txt`。
- 最新 run 的 `started.txt`、`completed.txt`。
- `ui-installation.txt`、`ui-page.expected`。
- `inventory.txt`、`observation.txt` 与进度快照。

据此区分入口/runner 失败、采集已启动但页面操作失败、页面已安装但 GEM 未加载。确认失败分支后再修正代码与制作新包，保留本次证据。
