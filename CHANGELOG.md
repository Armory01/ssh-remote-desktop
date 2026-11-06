# Changelog

**English** | [简体中文](CHANGELOG.zh-CN.md)

## 0.5.0

- Add optional Windows 11 x64 headless display support using a pinned
  VirtualDrivers driver-only package and native SetupAPI in the same helper.
- Add `display` status/ensure/disable/remove, capture-health checks, managed
  device ownership and explicit virtual-only topology selection. Reuse existing
  usable outputs and avoid duplicate monitors.
- Add controller-side verified driver caching and SSH/SCP deployment; no MCP,
  third-party installer, control app or custom resident service. The optional
  UMDF driver is persistently loaded by Windows while enabled.
- Add headless deployment tests and reboot/login validation; update the Skill
  and English/Chinese guides with publisher trust, offline packages and limits.

## 0.4.2

- Rename the installed Skill and its source directory to `ssh-remote-desktop`,
  matching the project. Replace the old `remote-windows` Skill folder on upgrade.
- Put task-based usage examples near the start of both READMEs: choose a computer,
  upload a package, run and configure an application, inspect it and retrieve logs;
  switch computers by name in the next request.
- Update installation prompts, script references, tests and release packaging.
  Protocol 1, `ra.exe`, remote deployment paths and desktop behavior are unchanged.

## 0.4.1

- Rename the project and repository to SSH Remote Desktop (`ssh-remote-desktop`).
- Make English the default documentation language, with Chinese versions of the
  README, SSH setup, advanced usage, architecture and validation documents.
- Lead installation with a prompt users can paste into Codex, Claude Code or
  another compatible agent; move low-level commands out of the README.
- Add Microsoft's OpenSSH setup instructions and official offline download links.
- Keep protocol 1, the `remote-windows` Skill name, `ra.exe` and existing remote
  paths compatible. Desktop behavior is unchanged from 0.4.0.

## 0.4.0

- Experimental Windows 11 local-account console login after logout or boot,
  using a temporary SYSTEM worker and the same account as the elevated SSH login.
- Report pre-login sessions, console session ID, OS information and uptime.
- Install missing remote PowerShell 7 globally during deployment; validate and
  cache the official MSI on the controller. Reuse existing installations and
  allow helper-only deployment with `-SkipPowerShell`.
- Publish complete x64/x86 binaries, SHA256 checksums, Skill and usage documents.
- Prepare the public source repository, MIT license and Windows build CI.
- Publish as SSH Remote Desktop; retain the `remote-windows` Skill name,
  `ra.exe` command and original deployment paths for compatibility.

Login remains experimental: the selected local account's standard password UI
is validated; PIN/Hello/MFA, forced Ctrl+Alt+Del, complex account selection and
new-profile setup are outside the validated paths. Protocol remains version 1.

## 0.3.0

- Add experimental password unlock and secure-desktop observation through
  short-lived SYSTEM workers, without a resident helper service.
- Verify the real WTS lock state and wake the display for the current request.

## 0.2.0

- Add explicitly requested high-integrity workers for an already elevated SSH
  caller, with actual worker and launched-process privilege information.

## 0.1.0

- Initial C++ single-file helper: SSH deployment, window enumeration, screenshots,
  UI Automation, mouse/keyboard/Unicode input, GUI launch and action batches.
- Return JSON and raw image bytes in one SSH response.
