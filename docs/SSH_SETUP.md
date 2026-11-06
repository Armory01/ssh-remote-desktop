# Enable OpenSSH on Windows

**English** | [简体中文](SSH_SETUP.zh-CN.md) · [Back to README](../README.md)

Do this once on the Windows target, using local access or an existing management
channel. If SSH and file transfers already work, go straight to the
[agent installation prompt](../README.md#install-copy-this-to-your-agent).
Commands below run in an administrator PowerShell on the target, except for the
final connection test, which runs on the controller.

## Option A: Windows optional feature

For Windows 10 1809+, Windows 11 and supported Windows Server releases:

```pwsh
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd
```

This uses Windows servicing and may download files from Windows Update. Check
the installation's `RestartNeeded` result before proceeding. Microsoft's
[quick start](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse)
describes this installation path and its service and firewall setup.

## Option B: download an offline MSI

Download on any connected computer, copy the MSI to the target, then install it
there. This route does not download the optional feature through Windows Update.

Official downloads checked on **2026-11-06**:

| Download | Architecture |
| --- | --- |
| [OpenSSH-Win64-v10.4.0.0.msi](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64-v10.4.0.0.msi) | Windows x64 |
| [OpenSSH-Win32-v10.4.0.0.msi](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win32-v10.4.0.0.msi) | Windows x86 |
| [OpenSSH-ARM64-v10.4.0.0.msi](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-ARM64-v10.4.0.0.msi) | Windows ARM64 |
| [Official releases, including ZIP packages](https://github.com/PowerShell/Win32-OpenSSH/releases) | Other versions and packaging |

The publisher labels **10.4.0.0p1-Preview** as a preview, not a production-ready
release. Use the Windows-serviced feature when that is your requirement.
OpenSSH's available architectures do not expand this project's tested helper
platforms. Read the [upstream release notes](https://github.com/PowerShell/Win32-OpenSSH/releases/tag/10.4.0.0p1-Preview)
and [MSI instructions](https://github.com/PowerShell/Win32-OpenSSH/wiki/Install-Win32-OpenSSH-Using-MSI)
when choosing a package. Use one installation route for the target's SSH service.

For the pinned **x64** MSI, SHA256 is:

```text
788d8643464a01eb63640a2fd746f9ed23d106b5a901a35f88dc8cd40e446ac9
```

Inspect its hash and publisher signature before installing:

```pwsh
Get-FileHash ./OpenSSH-Win64-v10.4.0.0.msi -Algorithm SHA256
Get-AuthenticodeSignature ./OpenSSH-Win64-v10.4.0.0.msi
```

Install the server feature; `-Wait` waits for MSI completion:

```pwsh
$msi = (Resolve-Path './OpenSSH-Win64-v10.4.0.0.msi').Path
Start-Process -FilePath msiexec.exe -ArgumentList "/i `"$msi`" ADDLOCAL=Server /qn /norestart" -Wait -PassThru -WindowStyle Hidden |
    Select-Object ExitCode
```

Exit code `0` means success; `3010` requests a restart. Resolve other installer
errors before continuing. For MSI installations, upstream also requires the
OpenSSH install directory in the system PATH for SCP/SFTP. Add it only if absent:

```pwsh
$sshDirectory = Join-Path $env:ProgramFiles 'OpenSSH'
$machinePath = [Environment]::GetEnvironmentVariable('Path', 'Machine')
if (($machinePath -split ';') -notcontains $sshDirectory) {
    [Environment]::SetEnvironmentVariable('Path', "$machinePath;$sshDirectory", 'Machine')
}
Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd
```

Open a new SSH session after setup. The ZIP option requires additional upstream
service-registration steps; the MSI is the shorter manual route.

## Firewall and connection check

The Windows feature normally creates an inbound TCP port 22 rule. Check it;
create the rule if absent, or enable the existing rule:

```pwsh
$rule = Get-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -ErrorAction SilentlyContinue
if ($rule) {
    $rule | Enable-NetFirewallRule
} else {
    New-NetFirewallRule -Name 'OpenSSH-Server-In-TCP' -DisplayName 'OpenSSH Server' -Direction Inbound -Protocol TCP -LocalPort 22 -Action Allow
}
Get-Service -Name sshd
```

From the controller, replace `windows-host` and `user` with your target details:

```pwsh
Test-NetConnection windows-host -Port 22
ssh user@windows-host whoami
```

Use the target Windows account and a working SSH authentication method. Desktop
operations use the same account as the graphical session. An administrator
account with an elevated SSH token is needed for high-privilege workers and the
experimental login/unlock operations. SSH key authentication does not provide
the Windows password needed for those password-based operations.

If optional-feature installation stalls because of Windows Update, WSUS or an
offline network, use Option B or matching Windows Features on Demand media.
The FoD source must match the Windows version/build and architecture; see
[Microsoft's offline-installation troubleshooting](https://learn.microsoft.com/en-us/troubleshoot/windows-server/system-management-components/cant-install-openssh-features).

Once SSH works, return to your agent. It handles this project's helper deployment
and desktop checks; no MCP, WinRM, SSH default-shell change or persistent helper
service is required.
