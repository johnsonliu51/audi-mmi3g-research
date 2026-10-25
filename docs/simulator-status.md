# K0942 / Audi 3G MMI：QNX 图形模拟器阶段记录（v5）

日期：2026-09-17

这份记录只描述已经在真实 K0942 `ifs-root` 上运行或从 ELF 静态确认的内容。它不把手工重绘图当成固件画面，也不把“进程启动”当成“已经得到有效 framebuffer”。

## 已经跑通的部分

QEMU 的 SH7785 目标已经能够从提取出的 QNX imagefs 启动真实进程，并进入下面的链路：

```text
dev-ipc
  -> devg-NVMiniRM / devg-NVMEM
  -> io-display -dvid=0x10de,did=0x187
  -> devg-nv.so + libned.so.6
  -> MMI3GApplication / layermanagerV2
```

这不是网页模拟器或假进程：启动命令、进程名、NVRM 日志和调用地址都来自固件内的 QNX 程序。运行时回调地址使用 `devg-nv.so` 的实际装载基址 `0x78201000` 重算后，232 条取指记录全部与文件偏移相差 `+0`；因此目前没有“整页 TLB 偏移”这一解释。

在 `outputs/live-qemu-nv18-ned-resource60b/audi-mmi-qemu-20260915-001202.stderr.log` 中，真实 NVIDIA 路径已经执行到：

```text
NVRM: SetVideoMode: head1 800 x 480, 32 bpp
NVRM: SetVideoMode: 800 x 480, 32 bpp
```

这证明驱动确实尝试建立 MMI 的 800×480 显示模式；但随后仍是：

```text
<LayermanagerV2><ERR>: CGraphicDevice::init: gf_dev_attach() failed!
<LayermanagerV2><ERR>: CGraphicsManager::init: Couldn't attach a device!
<LayermanagerV2><ERR>: CApplication::init: Initialization of graphic environment failed!
```

所以现在还没有一帧可以称为“原始 `MMI3GApplication` + `RDKWidgetD4` 的真实运行画面”。

## 新确认的阻塞点：NED 需要一个命名 IPC 服务

从 `libned.so.6` 的静态代码和运行时寄存器同时核对出，NED 的检查函数（文件偏移 `0x330b0`）在全局句柄为空时调用文件偏移 `0x33f20` 的资源打开函数。该函数的实际逻辑是：

```text
malloc(4)
memset(..., 0, 4)
name_open(path, 0)
```

其中 `path` 不是猜测值，而是由 GOT 偏移 `-0x1a68` 得到的固件字符串 `nMSDKIPC`。`name_open` 的 PLT 入口是 `libned` 的 `0x2d10`，失败时返回 `-1`，资源函数返回 `0x80000000`。

运行时日志给出了同一条证据：

```text
path="nMSDKIPC"
resource-name-open-ret r0=0xffffffff
resource-result       r1=0x80000000
callback-result        r1=0xfffefebd
```

`0xfffefebd` 的具体枚举名称尚未强行命名；固件字符串表确实包含 `NED_STATUS_IPC_FAILURE` 等状态名，但在没有完整枚举表映射前不把二者等同。重要的是：当前 QEMU 没有提供 `nMSDKIPC` 这个 QNX name-space 服务，因此 NED 后端在显示资源初始化阶段失败。固件里可见的是 `/dev/nvminirm0` 和 `/dev/shmem/nvminirm.finished`，它们不能自动替代 `name_open("nMSDKIPC")`。

这比“再加一个 NVIDIA 寄存器”更靠前：如果命名服务不存在，后面的 NED display manager、surface 和 layer 初始化就没有可靠的 IPC 对象可用。

## 已定位 `nMSDKIPC` 的真实服务进程

继续核对完整的 `mmi3g-srv-starter.cfg` 和 EFS 内容后，前一节的“缺少服务”已经可以进一步落到固件组件：

```text
Process #21  /sbin/io-media-nvidia -c /etc/iom.cfg
             ProvidesInterface 15
             RequiresInterface 77, 76

Process #66  /sbin/NvElfLoader -f /etc/NvDspCodecs.elf
Process #67  /usr/bin/devg-NVTS -p 21
             ProvidesInterface 77
Process #68  /usr/bin/GPIOSet -CSample
             ProvidesInterface 76
Process #73  /usr/bin/devg-NVMEM
             ProvidesInterface 83
```

`io-display`（进程 #19）要求接口 15，因此原始图形顺序不是只有 `devg-NVMiniRM -> io-display -> MMI`，而是还要先满足 NVIDIA 媒体/DSP 这条依赖链。更关键的是，当前提取出的 IFS 没有 `/sbin/io-media-nvidia`、`/etc/iom.cfg` 和 `/etc/NvDspCodecs.elf`；它们在 EFS system 中，并通过 `efs_symlinks.cfg` 映射为系统根下的 `/sbin`、`/etc` 路径。

EFS 中的真实 `io-media-nvidia`（3,298,076 bytes）同时包含 `name_attach`、`name_open` 和字面量 `nMSDKIPC`，还包含 `RenderManagerIPC`、`NvVideoIpcThread` 等同一媒体/显示 IPC 组件。因而当前失败的直接原因不是 MMI 里缺少一个菜单编码，而是缩短启动链既没有挂载 EFS，也没有运行这个真实服务。这里的静态证据已经足以改变实验顺序；在获得一次完整链的运行时 `name_attach("nMSDKIPC")` 记录前，不把它写成“已完全证明的服务端调用点”。

完整 EFS 的启动前提也由 IFS `.script` 明确写出：`devf-generic -s0x0,128m` 后由 `checkPersistence` 访问 `0x06200000`，再由 `inflator /mnt/efs-system/bin` 展开 EFS，随后 `efs_symlinks.cfg` 建立 `/sbin/io-media-nvidia` 等映射。当前 `--promote-mmi-chain` 诊断变体会把这些前置 RUN 记录全部替换成 PRINT，因此它不可能看到 `io-media-nvidia`；下一版应先验证“真实 EFS 挂载 + 原始接口依赖”，再决定是否需要 QEMU 侧补 name-service。

为此已经加入一个显式的隔离构建选项 `--script-full-efs`，并由原始未压缩 payload 生成了 `outputs/ifs-patches/payload-script-full-efs-v2.bin`（SHA-256：`a7dc10be07d33c64d5c7e2906e49162687e7903d36edf7824497daeeada677e1`）。它只把 fesh 的 IFS 路径改成 `/mnt/ifs-root/bin/fesh`，保留 EFS 挂载、持久化检查、inflator、symlink builder 以及原始 `serverstarterboot`；运行时再挂载一次性 `outputs/flash-ab/audi-mmi-full-128m.raw`，即可做完整启动链 A/B。该模式仍是诊断副本，绝不写回车辆。

## 目前仍未解决的第二层问题

即使把 `nMSDKIPC` 的 name-open 伪造成成功，也不能直接宣布能显示画面，因为：

1. `devg-NVMiniRM` 仍报告大量 `Stub function called`，并且检测到 native AGP/AGP bus 的 unsupported configuration；
2. 某次 60 秒实验中 `io-display` 在 `IP=0` 崩溃，随后 MMI 出现 `HBSemaphoreAccessQNX.cpp:123: ASSERTION failed`；
3. NV18 BAR1 在崩溃前只出现 6066 个非零字节，范围约为 `0x4..0x17ff`，这更像初始化/命令数据，尚未证明是按 800×480×32bpp 排列的 framebuffer；
4. 直接跳过 HBSemaphore 断言会转成未初始化同步对象的 SIGSEGV，属于诊断实验，不是修复方案。

## 对胎压 UI 的实际含义

K0942 的 `carServiceRkaHighDisplay`、`RDKWidgetD4`、模型 61059 和 `TEST(18)` 注入路径已经在固件层确认；但在图形环境没有完成 attach 之前，无法在电脑上让原始 QNX 程序把这个页面真正合成出来。当前阻塞点位于显示/IPC 基础设施，不是 `DisplayPressure` 菜单编码本身。

下一步的可复核顺序是：

1. 先用包含 EFS system 的隔离 flash 映像恢复 `devf-generic -> checkPersistence -> inflator -> efs_symlinks`，并按配置启动 `NvElfLoader -> devg-NVTS/GPIOSet -> io-media-nvidia -> io-display`；
2. 记录 `io-media-nvidia` 是否实际创建 `nMSDKIPC`，再重新记录 `neddDisplayMgrInit`、`neddDisplayCount`、surface 分配和 `gf_dev_attach` 的返回值；
3. 只有在真实 display surface、stride/format 和可读回像素出现后，才触发固件自带的 `TEST(18)`，再抓取 RDK 数值页；若完整媒体链仍不能创建服务，才使用最小 QNX name-service A/B，而不是直接伪造成功返回。

在第 3 步以前，任何“已经显示数字胎压页面”的截图都只能算重绘或静态重建，不能算固件运行时帧。

## 可复核材料

- 运行链和 RDK 入口：[K0942-RDK-runtime-chain.md](K0942-RDK-runtime-chain.md)
- 装载地址校验：[devg-nv-runtime-mapping-v3.md](devg-nv-runtime-mapping-v3.md)
- 真实 NVRM/graphics 日志：`live-qemu-nv18-ned-resource60b/audi-mmi-qemu-20260915-001202.stderr.log`
- `libned` 静态样本：`work/k0942_ifs41/mnt/ifs-root/usr/lib/libned.so.6`
