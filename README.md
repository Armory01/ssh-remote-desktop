# SSH Remote Desktop

**English** | [简体中文](README.zh-CN.md)

**Enable SSH once. Let your AI agent handle the desktop.**

A Skill that lets Codex, Claude Code and other compatible agents transfer files,
run programs and operate a remote Windows desktop through ordinary SSH.

No MCP setup. No remote control panel. No helper service to keep running.
Once SSH works, your agent downloads, verifies and deploys a tiny native helper
for you. Desktop workers start when needed and exit after each request.

[Download](https://github.com/xienan2010/ssh-remote-desktop/releases/latest) ·
[OpenSSH setup](docs/SSH_SETUP.md) · [Advanced usage](docs/USAGE.md) ·
[MIT License](LICENSE)

## Use it like this

Once installed, mention **`ssh-remote-desktop`** and describe the result you want:

```text
Use ssh-remote-desktop on win-test. Upload this test package, extract it,
launch the application and complete the setup using the settings I provide.
Show me a screenshot of the result, then download the application log.
```

To work on another computer, just name it in the next request:

> Switch to office-pc and run the same test there.

One Skill on your controlling agent, reused across your Windows computers.
The agent handles the connection, helper deployment, file transfers and desktop
commands. You describe the task without writing click or deployment scripts.
Use existing SSH host aliases, or provide a target address and account when asked.

## Why use it?

- **One familiar connection.** Files, commands and desktop operations all use SSH.
  Select computers by existing SSH names such as `work-pc` or `win-test`.
- **Let the agent do the setup.** Enable SSH on the target once; your agent handles
  helper downloads, checksum verification, deployment and updates.
- **A single native helper.** One C++ executable with a static runtime. No Python,
  Node.js, .NET or Visual C++ Redistributable required by the helper.
- **No MCP or extra listening port.** Reuse the agent's shell, SSH/SCP and image
  tools. No separate Remote Desktop client or custom local controller is needed.
- **Nothing extra to keep running.** No resident helper service and no full AI
  agent on every computer. Windows starts short-lived desktop workers on demand.
- **Real desktop interaction.** Screenshots, UI Automation, mouse, keyboard,
  Unicode input, program launch and action batches.
- **Headless Windows, on demand.** On Windows 11 x64, the agent can detect a
  missing display and install a pinned VirtualDrivers display driver over SSH.
  No physical monitor, RDP connection or additional helper service is needed.
- **Efficient image return.** A screenshot travels with its result in one SSH
  response, as raw image bytes. No Base64 or second transfer to retrieve the image.
- **Elevated tasks when needed.** An already elevated SSH account can request a
  high-privilege worker. Experimental Windows 11 password login and unlock use
  temporary SYSTEM workers, without persistent auto-login settings.

## Install: copy this to your agent

Paste this into **Codex, Claude Code, or another agent that can load Skills and
execute shell commands**:

```text
Install the ssh-remote-desktop Skill from:
https://github.com/xienan2010/ssh-remote-desktop

Read skills/ssh-remote-desktop/SKILL.md. Install the complete Skill folder,
including its scripts and references, into your own supported user-level
skills directory. Download the release pinned by the Skill and verify
the helper's SHA256 before using it. Use existing SSH/SCP; do not set up MCP.
Migrate an existing remote-windows installation to the new Skill name so
only ssh-remote-desktop remains installed.

After installation, ask which Windows SSH host I want to use. Reuse my
existing SSH configuration where available. Once I provide the target,
handle helper deployment and environment checks for me, then report whether
the desktop is ready. Do not require me to run low-level helper commands.
```

The installed Skill is named **`ssh-remote-desktop`**, matching the project.
For an installation from before 0.4.2, replace the old `remote-windows` Skill
folder with the complete `ssh-remote-desktop` folder. Existing remote helper
deployments and SSH configuration can be reused.

## First-time Windows preparation: enable SSH

If you can already SSH into the Windows computer, this step is complete.
Otherwise, run these commands in an **administrator PowerShell on that computer**:

```pwsh
Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0
Set-Service -Name sshd -StartupType Automatic
Start-Service -Name sshd
```

This is Microsoft's Windows optional-feature installation path. If Windows
Update is slow or unavailable, use an offline package instead:

- [Official OpenSSH MSI / ZIP downloads](https://github.com/PowerShell/Win32-OpenSSH/releases).
- [Windows x64 offline MSI — 10.4.0.0 Preview](https://github.com/PowerShell/Win32-OpenSSH/releases/download/10.4.0.0p1-Preview/OpenSSH-Win64-v10.4.0.0.msi).
- [Setup guide: firewall, offline options and connection checks](docs/SSH_SETUP.md).

The standalone GitHub build above is an upstream Preview release; the Windows
optional feature is the Windows-serviced option. See Microsoft's
[quick start](https://learn.microsoft.com/en-us/windows-server/administration/openssh/openssh_install_firstuse).

SSH must be enabled through local access or an existing management channel;
an agent cannot connect through SSH before it is available. After that, you do
not manually install or start our helper: the agent handles it over SSH.

## Current support

| Component | Current scope |
| --- | --- |
| Remote computer | Windows; end-to-end tested on Windows 11 x64. x64 and x86 helpers included. |
| Controller | Windows with existing **PowerShell 7**, SSH/SCP and the agent's shell tools. |
| Agents | Skill guidance for Codex, Claude Code and DeepSeek Harness; end-to-end validation currently in Codex. Image tasks need a working image tool and vision model. |
| Desktop | An unlocked same-account desktop with an active display; a local console does not require an RDP connection. |
| Login / unlock | Experimental: the same Windows 11 local account's standard password screen, using an already elevated SSH login. |
| Headless display | Experimental Windows 11 x64 support with the x64 helper and administrator SSH. Optional VirtualDrivers driver; [details](docs/HEADLESS.md). |

The agent can also install missing **remote** PowerShell 7 globally, using a
verified official installer cached on the controller. The native helper itself
does not depend on PowerShell. Current controller scripts require PowerShell 7
to be present on the controller.

macOS/Linux desktop targets, older Windows, PIN/Hello/MFA and forced
Ctrl+Alt+Del are outside the validated scope. Headless image capture still needs
a usable real or virtual display. See [validation details](docs/TEST_RESULTS.md).

## More information

- [Advanced usage and troubleshooting](docs/USAGE.md)
- [Architecture](docs/IMPLEMENTATION_PLAN.md)
- [Skill and wire protocol](skills/ssh-remote-desktop/SKILL.md)
- [Build and contribute](CONTRIBUTING.md)
- [Changelog](CHANGELOG.md)

Code, scripts and Skill are MIT licensed. Bundled nlohmann/json has its own MIT
notice; see [third-party information](third_party/README.md).
