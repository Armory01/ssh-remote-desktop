# 在 Windows 上开通 OpenSSH

[English](SSH_SETUP.md) | **简体中文** · [返回 README](../README.zh-CN.md)

在被控电脑上通过本地操作或已有管理通道完成一次。如果 SSH 和文件传输已经可用，
直接使用 [Agent 安装提示词](../README.zh-CN.md#安装复制给-agent)。
除最后的连接检查外，下面命令均在目标电脑的管理员 PowerShell 中执行。

## 方式 A：Windows 可选组件

适用于 Windows 10 1809+、Windows 11 和相应 Windows Server：

```pwsh
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd
```

这条路线通过 Windows 维护组件安装，可能需要 Windows Update 下载文件。
安装后查看 `RestartNeeded` 是否要求重启。服务和防火墙设置参考微软
[官方快速开始](https://learn.microsoft.com/zh-cn/windows-server/administration/openssh/openssh_install_firstuse)。

## 方式 B：下载离线 MSI

在可联网的电脑下载 MSI，拷到目标电脑安装，不再通过 Windows Update 获取可选组件。

以下官方入口核对日期为 **2026-11-06**：

| 下载 | 架构 |
| --- | --- |
| [OpenSSH-Win64-v10.4.0.0.msi](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64-v10.4.0.0.msi) | Windows x64 |
| [OpenSSH-Win32-v10.4.0.0.msi](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win32-v10.4.0.0.msi) | Windows x86 |
| [OpenSSH-ARM64-v10.4.0.0.msi](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-ARM64-v10.4.0.0.msi) | Windows ARM64 |
| [官方发布页，含 ZIP 包](https://github.com/PowerShell/Win32-OpenSSH/releases) | 其他版本和包格式 |

上游明确将 **10.4.0.0p1-Preview** 标为尚不适合生产用途的预览版。
如果需要由 Windows 维护的版本，使用可选组件路线。OpenSSH 提供的架构不等于
本项目的 helper 都已在这些架构上验收。选择前查看
[发布说明](https://github.com/PowerShell/Win32-OpenSSH/releases/tag/10.4.0.0p1-Preview)和
[MSI 安装说明](https://github.com/PowerShell/Win32-OpenSSH/wiki/Install-Win32-OpenSSH-Using-MSI)，
同一台机器的 SSH 服务选择一种安装路线。

上述 **x64** MSI 的 SHA256：

```text
788d8643464a01eb63640a2fd746f9ed23d106b5a901a35f88dc8cd40e446ac9
```

安装前核对哈希和发布者签名：

```pwsh
Get-FileHash ./OpenSSH-Win64-v10.4.0.0.msi -Algorithm SHA256
Get-AuthenticodeSignature ./OpenSSH-Win64-v10.4.0.0.msi
```

安装服务端；`-Wait` 会等 MSI 安装完成：

```pwsh
$msi = (Resolve-Path './OpenSSH-Win64-v10.4.0.0.msi').Path
Start-Process -FilePath msiexec.exe -ArgumentList "/i `"$msi`" ADDLOCAL=Server /qn /norestart" -Wait -PassThru -WindowStyle Hidden |
    Select-Object ExitCode
```

退出码 `0` 表示成功，`3010` 表示需要重启；其他退出码先处理安装错误。
上游 MSI 说明还要求把 OpenSSH 安装目录加入系统 PATH，以便 SCP/SFTP 工作，
不存在时再添加：

```pwsh
$sshDirectory = Join-Path $env:ProgramFiles 'OpenSSH'
$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
if (($machinePath -split ';') -notcontains $sshDirectory) {
    [Environment]::SetEnvironmentVariable('Path', "$machinePath;$sshDirectory", 'Machine')
}
Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd
```

安装完成后新建 SSH 连接。ZIP 包另需按上游说明注册服务，手动安装优先使用步骤更少的 MSI。

## 防火墙与连接检查

Windows 可选组件通常会建立 TCP 22 入站规则。检查后，不存在则创建，存在则启用：

```pwsh
$rule = Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue
if ($rule) {
    $rule | Enable-NetFirewallRule
} else {
    New-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -DisplayName 'OpenSSH Server' -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow
}
Get-Service -Name sshd
```

在控制端检查，将 `windows-host` 和 `user` 换成实际地址与账户：

```pwsh
Test-NetConnection windows-host -Port 22
ssh user@windows-host whoami
```

使用目标 Windows 账户和可用的 SSH 认证方式。桌面操作使用与图形会话相同的账户；
高权限 worker 和实验性的登录／解锁要求 SSH 已有管理员高完整性权限。
SSH 密钥认证不会提供 Windows 密码，密码登录／解锁仍需要已知 Windows 密码。

如果 Windows Update、WSUS 或离线网络导致可选组件安装卡住，可使用方式 B，
或使用匹配的 Windows Features on Demand 安装介质。FoD 来源必须匹配系统版本、
构建号和架构，参考微软的[离线安装故障处理](https://learn.microsoft.com/zh-cn/troubleshoot/windows-server/system-management-components/cant-install-openssh-features)。

SSH 连通后回到 Agent 即可。helper 部署和桌面检查由它处理，无需配置 MCP、WinRM，
也不要求修改 SSH 默认 Shell 或新增 helper 常驻服务。
