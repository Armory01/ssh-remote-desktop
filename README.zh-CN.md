# SSH Remote Desktop

[English](README.md) | **简体中文**

**开通一次 SSH，剩下的桌面操作交给 AI Agent。**

一个让 Codex、Claude Code 等兼容 Agent 通过普通 SSH 传文件、运行程序、
操作远程 Windows 桌面的 Skill。

无需配置 MCP，无需控制面板，无需维护额外常驻服务。
SSH 可用后，Agent 自动下载、校验和部署小巧的原生 helper；
桌面 worker 按需启动，每次请求结束后退出。

[下载](https://github.com/xienan2010/ssh-remote-desktop/releases/latest) ·
[开通 OpenSSH](docs/SSH_SETUP.zh-CN.md) · [进阶用法](docs/USAGE.zh-CN.md) ·
[MIT 许可证](LICENSE)

## 安装后，直接这样用

安装完成后，在请求里提到 **`ssh-remote-desktop`**，再描述你想完成的任务：

```text
使用 ssh-remote-desktop 操作 win-test。把这个测试包上传并解压，
启动程序，按我提供的参数完成配置。把结果截图给我，然后下载程序日志。
```

要操作另一台电脑，下一句直接换名字：

> 切换到 office-pc，在那台电脑上再做一次相同测试。

在控制端 Agent 安装一个 Skill，就可以复用于多台 Windows 电脑。
连接、helper 部署、文件传输和桌面操作命令都由 Agent 处理，你只需要描述任务，
不必编写点击或部署脚本。可以使用已有 SSH Host 别名，或在 Agent 询问时提供目标地址与账户。

## 项目特点

- **沿用熟悉的 SSH。** 文件、命令和桌面操作都走 SSH，使用已有的电脑名称选择目标，
  例如 `work-pc`、`win-test`。
- **让 Agent 负责部署。** 被控端首次开通 SSH，之后的 helper 下载、哈希校验、
  上传和更新由 Agent 完成。
- **单文件原生 helper。** 一个 C++ EXE，静态链接运行库；helper 不要求安装
  Python、Node.js、.NET 或 Visual C++ Redistributable。
- **无需 MCP 和额外监听端口。** 复用 Agent 已有的 Shell、SSH/SCP 和识图工具，
  不需要专用本地控制客户端或远程桌面客户端。
- **按需运行，结束退出。** 不增加 helper 常驻服务，也无需每台电脑运行一个完整的
  AI Agent，由 Windows 按需启动短期桌面 worker。
- **无显示器也可按需准备桌面。** Windows 11 x64 下，Agent 可以通过 SSH 检测缺失的
  显示输出并安装固定版本的 VirtualDrivers 驱动，无需接显示器、保持 RDP 或增加 helper 常驻服务。
- **实际操作桌面。** 支持截图、UI Automation、鼠标键盘、中文输入、程序启动与动作批处理。
- **截图一次返回。** 操作结果和原始图片字节通过同一次 SSH 响应返回，
  无需 Base64，也不需要再连接一次取图。
- **需要时使用高权限。** 已具备管理员权限的 SSH 账户可按次请求高权限 worker；
  实验性的 Windows 11 密码登录和解锁使用临时 SYSTEM worker，不配置持久自动登录。

## 安装：复制给 Agent

把下面这段提示词直接复制给 **Codex、Claude Code，或其他可以加载 Skill、执行 Shell 命令的 Agent**：

```text
请安装这个仓库中的 ssh-remote-desktop Skill：
https://github.com/xienan2010/ssh-remote-desktop

阅读 skills/ssh-remote-desktop/SKILL.md，将完整 Skill 目录（包括 scripts 和
references）安装到你当前支持的用户级技能目录。按 Skill 中固定的版本下载
发布包，使用 helper 前核对其 SHA256。使用现有 SSH/SCP，不要配置 MCP。
如已安装旧名 remote-windows，请将该安装迁移到 ssh-remote-desktop，避免重复安装。

安装完成后，询问我要操作哪台 Windows SSH 电脑，并优先复用已有 SSH 配置。
我提供目标后，请自动处理 helper 部署和环境检查，告诉我桌面是否可用。
不需要让我手工执行底层 helper 命令。
```

安装后的 Skill 名称为 **`ssh-remote-desktop`**，与项目统一。
如果之前安装的是 0.4.2 之前的版本，将旧的 `remote-windows` Skill 目录
替换为完整的 `ssh-remote-desktop` 目录即可，已有远端 helper 和 SSH 配置可以复用。

## Windows 首次准备：开通 SSH

如果已经能通过 SSH 连接目标电脑，这一步就已完成。
否则，在**目标电脑的管理员 PowerShell** 中执行：

```pwsh
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd
```

这是微软提供的 Windows 可选组件安装方式。如果 Windows Update 下载缓慢或不可用，
可以改用离线安装包：

- [官方 OpenSSH MSI / ZIP 下载页](https://github.com/PowerShell/Win32-OpenSSH/releases)。
- [Windows x64 离线 MSI：10.4.0.0 Preview](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64-v10.4.0.0.msi)。
- [开通指南：防火墙、离线安装与连接检查](docs/SSH_SETUP.zh-CN.md)。

上述 GitHub 独立构建是上游 Preview 版本；Windows 可选组件则通过 Windows 维护更新。
参考微软的[官方快速开始](https://learn.microsoft.com/zh-cn/windows-server/administration/openssh/openssh_install_firstuse)。

首次开通 SSH 需要本地操作或其他已有管理通道，Agent 无法通过尚未启用的 SSH 连接电脑。
之后不需要你手动安装或启动本项目的 helper，Agent 会通过 SSH 自动处理。

## 当前支持范围

| 项目 | 当前范围 |
| --- | --- |
| 被控电脑 | Windows；完整实测为 Windows 11 x64，提供 x64、x86 helper。 |
| 控制端 | Windows，已有 **PowerShell 7**、SSH/SCP 和 Agent 的 Shell 工具。 |
| Agent | 提供 Codex、Claude Code、DeepSeek Harness 使用说明；目前完整端到端验收在 Codex 完成。识图需要可用的图片工具与视觉模型。 |
| 桌面 | 同一账户已登录解锁，有有效显示输出；本地控制台不需要保持 RDP 连接。 |
| 无显示器 | 实验性支持 Windows 11 x64、x64 helper、管理员 SSH；可选装 VirtualDrivers 驱动，见[说明](docs/HEADLESS.zh-CN.md)。 |
| 登录／解锁 | 实验性支持 Windows 11 同一本地账户的标准密码界面，要求 SSH 已具备管理员高完整性权限。 |

Agent 还可以在**远端**缺少 PowerShell 7 时自动全局安装，官方安装包经校验后在控制端缓存。
原生 helper 本身不依赖 PowerShell；当前控制端脚本要求控制端已有 PowerShell 7。

macOS/Linux 桌面目标、旧 Windows、PIN/Hello/MFA 和强制 Ctrl+Alt+Del 尚不在验收范围。
无物理显示器时，Agent 可按需准备虚拟显示输出；驱动由 Windows 持续加载，helper 本身仍按请求运行。详见[无显示器说明](docs/HEADLESS.zh-CN.md)和[验收说明](docs/TEST_RESULTS.zh-CN.md)。

## 更多资料

- [进阶用法与故障处理](docs/USAGE.zh-CN.md)
- [实现方案与历史设计](docs/IMPLEMENTATION_PLAN.zh-CN.md)
- [Skill 与通信协议](skills/ssh-remote-desktop/SKILL.md)
- [构建和贡献](CONTRIBUTING.zh-CN.md)
- [更新记录](CHANGELOG.zh-CN.md)

源码、脚本和 Skill 使用 MIT 许可证。内嵌的 nlohmann/json 带有自己的 MIT 声明，
见[第三方说明](third_party/README.zh-CN.md)。
