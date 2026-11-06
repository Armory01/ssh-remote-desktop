# 通过 SSH 操作无显示器 Windows

[English](HEADLESS.md) | **简体中文** · [README](../README.zh-CN.md)

0.5.0 增加可选的 VirtualDrivers 支持，当前限 **Windows 11 x64**、x64 helper 和
已有管理员高完整性 SSH 登录。用户只需开通 SSH，Agent 可完成驱动下载、校验和部署。
普通 GUI 操作仍要求同一账户已登录、解锁。

## Agent 如何处理

直接说：“使用 ssh-remote-desktop，把这台 Windows 准备成无显示器也能操作，
然后登录桌面并测试程序。”

Agent 先读 `display status`。已有真实、虚拟机或虚拟显示输出且截图可用时，
`display ensure` 直接复用；锁屏、应用黑屏或截图错误本身不应触发装驱动。
显示路径缺失时，脚本在控制端下载并缓存固定版本的 driver-only ZIP，上传四个文件，
由同一个 `ra.exe` 调用 Windows 原生 API 安装。

新的授权安装通过脚本 `-TrustPublisher` 允许将包内**指定的 SignPath 叶证书**加入
`LocalMachine/TrustedPublisher`。先验证固定 SHA256 和 Authenticode；不导入根证书，
不修改 Secure Boot、测试签名、内存完整性或签名强制策略。Windows 拒绝安装时，
查看返回的错误和 `C:\Windows\INF\setupapi.dev.log`。

不安装控制面板、Nefcon、自定义服务、登录任务或额外监听端口。启用期间，Windows
在自己的 WUDFHost 中加载 UMDF 显示驱动。helper 仍是单个 EXE，worker 每次请求后退出；
**显示驱动本身是持续生效的系统组件**，不能把“不增加 helper 常驻服务”理解成没有驱动进程。

## 进阶命令

```pwsh
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"display"}'
& ./skills/ssh-remote-desktop/scripts/Ensure-Display.ps1 -Computer win-test -TrustPublisher
```

已有可用输出时不会下载驱动。需要明确验证 VirtualDrivers 独立提供桌面时，加
`-VirtualOnly`：它会切换并持久保存仅启用虚拟屏的显示布局，保留其他显卡设备。
`-ForceVirtual` 则在已有输出旁增加虚拟屏。两者都不是默认行为，重复执行复用同一个设备。

`-DriverZipPath <文件>` 可提供完全匹配的离线 ZIP；`-CacheDirectory` 可覆盖默认的
`%LOCALAPPDATA%\SSHRemoteDesktop\drivers` 缓存目录。helper 将文件复制到受保护的临时
目录，再次校验四个文件的哈希。每次请求的上传文件和临时目录会清理，不依赖远端 PowerShell。
`-RemotePath`、`-SshOption` 与其他控制端脚本一致。

初次创建 `C:\VirtualDisplayDriver\vdd_settings.xml`，配置为 **1 个 1920×1080、
60 Hz、SDR 屏幕**；若该文件已存在，保持原配置。创建的设备实例记录在
`HKLM\SOFTWARE\SSHRemoteDesktop\VirtualDisplay`。不移除其他厂商的显示设备；
已有健康的 MttVDD 可复用，不健康且不属于本项目的实例会返回错误供检查。

```json
{"op":"display","params":{"action":"status","screenshot":true}}
{"op":"display","params":{"action":"disable"}}
{"op":"display","params":{"action":"remove"}}
```

禁用、移除只操作本项目记录的设备。`ensure` 可以不下载包直接恢复其禁用设备。
移除保留驱动仓库包、配置和发布者信任，不撤销可能被其他软件共用的证书。
显示布局切换可能影响桌面窗口位置。

## 驱动来源与边界

- [VirtualDrivers / Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)，MIT。
- 固定使用 [25.7.23 driver-only 发布](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23)。
  该发布标题含 Beta；ZIP 文件名虽然含 `x86`，实际 DLL 和 INF 是 **AMD64/x64**，不是 32 位驱动。
- ZIP SHA256：`e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a`。
- 签名叶证书 SHA1：`3CF8CF26D8BA266C3A483AB7D26D4A818E317D76`。

驱动独立下载，不打包进我们的发布文件。其他 Windows 版本、ARM64 和真正的 32 位系统
不在本功能范围。实际测试环境及签名策略边界见[验收记录](TEST_RESULTS.zh-CN.md)，
不能把一台机器成功当成所有固件和安全策略都已通过。

虚拟显示器提供显示表面，不会替代 Windows 登录；需要时仍走已有 `login`/`unlock`。
受保护内容及部分 GPU 程序仍可能受 GDI 截图限制。本地控制台路径不需要保持 RDP。
