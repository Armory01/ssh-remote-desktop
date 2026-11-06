# Changelog

## 0.4.0

- Experimental Windows 11 local-account console login after logout or boot,
  using a temporary SYSTEM worker and the same account as the elevated SSH login.
- Report pre-login sessions, console session ID, OS information and uptime.
- Install missing remote PowerShell 7 globally during deployment; validate and
  cache the official MSI on the controller. Reuse existing installations and
  allow helper-only deployment with `-SkipPowerShell`.
- Publish complete x64/x86 binaries, SHA256 checksums, Skill and usage documents.
- Prepare the public source repository, MIT license and Windows build CI.
- Publish as Remote Windows SSH Skill; retain the `remote-windows` Skill name,
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
