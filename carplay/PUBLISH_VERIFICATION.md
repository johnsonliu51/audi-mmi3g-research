# 发布前检查

日期：2026-10-26。环境：本机 macOS，系统 C 编译器。

对以下八个模块逐个执行：

```sh
cc -std=c99 -Wall -Wextra -Werror -O2 -I carplay/src/protocol \
  -c carplay/src/protocol/MODULE.c -o LOCAL_BUILD_DIR/MODULE.o
```

模块：iap2_csm、ncm_ntb16、usbmux_codec、lockdown_channel、avc_nal、usbmux_session、usbmux_host、iap2_link。

结果：8/8 编译成功，没有编译诊断。生成物保留在非 iCloud 本机目录，不加入仓库。
这不是单元测试、协议兼容测试、目标 QNX 构建、手机握手或实车验证；本次没有执行 SD/车辆部署。

发布内容检查：保留协议来源和 GPL 许可证；未加入 artifacts ZIP、固件/SDK、对象/可执行二进制、内存转储、私人本机绝对路径、账号凭据或车辆 VIN。
仅发布可完整读取和审核的文件；云端未下载部分未纳入，已在 README 说明完整性限制。
