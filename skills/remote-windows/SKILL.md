---
name: remote-windows
description: Deploy and use the SSH Remote Windows desktop helper over an existing SSH connection to transfer files, launch programs, inspect UIA, take screenshots, operate a Windows desktop, and sign in to a Windows 11 local account. Use for remote Windows GUI tasks, not ordinary Linux SSH administration.
---

Use existing SSH/SCP and the Agent's native image reader. No MCP or dedicated local controller client is required. The target needs working SSH. Ordinary GUI commands need an unlocked same-account graphical session; helper 0.3.0 adds password unlock, and 0.4.0 adds Windows 11 local-account console sign-in after logout or boot through an already elevated SSH login. A helper is deployed automatically; the user does not install a runtime or start a worker.

## Connect and deploy

### Obtain the matching helper

This Skill is distributed from https://github.com/xienan2010/remote-windows-ssh-skill-skill. Use the supplied release binary when available. If only the Skill was installed, use the controller's existing PowerShell to download and expand the pinned complete release https://github.com/xienan2010/remote-windows-ssh-skill-skill/releases/download/v0.4.0/remote-windows-ssh-skill-0.4.0.zip into a local cache (for example `%LOCALAPPDATA%/SSHRemoteWindows/releases/0.4.0`). Do not use GitHub's source-code ZIP as a binary package. Select `windows-x64/ra.exe` for an x64 target or `windows-x86/ra.exe` for an x86 target. Check the executable's SHA256 before deployment:

- x64 0.4.0: `d1697b0149d8c758ed0bb9564e4405b873d848929fdcdae28f3ddb6b1c255b6a`
- x86 0.4.0: `4fa4c95ebac67825074ff5b48c3eae04726ba87991478acd9d205041f7c7c2ee`

Reuse a verified cached binary; do not redownload for every command. The controller scripts below remain the installed Skill's adjacent scripts. No compiler, local controller binary or MCP setup is needed on the controller. A mismatched hash is a deployment failure, not permission to execute the binary.

### Use the user's SSH connection

Use the user's SSH host alias or `user@address`; do not embed credentials in files or command lines. For a remembered computer name, reuse its existing `~/.ssh/config` entry. A new alias is optional: direct addresses work. Native SSH handles authentication; unattended operation needs existing key authentication or another working SSH authentication mechanism.

On a Windows controller with PowerShell 7, use the adjacent scripts. Locate the supplied, trusted prebuilt `ra.exe` and verify its release SHA256. Do not assume the target can download it or compile C++.

```pwsh
& '<skill>/scripts/Deploy-Ra.ps1' -Computer win-test -Executable '<release>/ra.exe'
& '<skill>/scripts/Receive-Ra.ps1' -Computer win-test -Request '{"op":"status"}'
& '<skill>/scripts/Receive-Ra.ps1' -Computer win-test -Request '{"op":"windows"}'
```

The deploy script compares hashes and reuses an identical helper, otherwise stages, checks and replaces it in `RemoteAgent/ra.exe` under the SSH home directory. It needs the target's built-in `cmd.exe` and `certutil`. Starting with 0.4.0, an authorized deployment also checks remote PowerShell 7 and globally installs the pinned official MSI if missing; `-SkipPowerShell` opts out. It does not change sshd, SSH authentication, OS autologon or UAC settings. Authentication may be requested for each SSH/SCP invocation. `-SshOption @('-i','<key>')` passes SSH options; `-p` is not portable between SSH and SCP, use `-o Port=...`.

The supplied controller scripts currently require PowerShell 7 and are validated on Windows. For other controllers, read [the wire contract](references/protocol.md); do not install another runtime just to receive an image. If the available shell cannot safely receive binary output, report the missing receiver rather than feeding binary data through a text pipeline.

## Observe, act, verify

1. Read `status`. Session 0 describes the SSH process, not a graphical login. `sessions` also includes empty-user service/listener/pre-login rows; find the row matching `console_session_id`. If the console is signed out, use the login flow below; for an existing locked same-account desktop use unlock; otherwise read `windows`. Ordinary GUI commands still find same-account active sessions. Resolve `AMBIGUOUS_SESSION` with `params.session_id`.
2. Select the returned numeric HWND and inspect it with `observe`. Pass `expected` from the most recent window (`pid`, `process_start`, `bounds`) when acting. Observe again after a window moves or a selector becomes stale.
3. `Receive-Ra.ps1` returns a small JSON result and an absolute `image_path`. Open the image with the host's native image tool: Codex `view_image` or equivalent; Claude Code `Read`; DeepSeek Harness `read_image` if the current route supports vision. A terminal file path, UIA text or Base64 is not image input. If there is no image-capable tool/model, report that limit and use UIA only when it suffices.
4. Prefer a unique observed UIA selector for controls with working patterns. Otherwise use screenshot coordinates mapped to physical desktop pixels. Captures use GDI screen contents; a covered window may include the covering window. Activate through an `act` batch with a `focus` action before capturing a covered window. An unlocked local console desktop with an active display does not need RDP. When operating an RDP session, minimizing or disconnecting its client can suspend rendering or lock/disconnect the session; that scenario needs separate handling.
5. Batch a few well-understood actions and optionally request a final observation. Input completion means events were accepted, not that the app finished rendering or saving. If an image shows intermediate typing, wait and observe again; do not retype. Stop on error; report `completed`, `failed_index` and `outcome_unknown`. After disconnect/timeout, inspect current state before any retry of clicks, typing, launches or saves. Never replay a whole batch blindly.

Read [command details and examples](references/protocol.md) for launch, UIA selectors and action arguments.

## Programs and files

Use ordinary SSH for headless commands and scripts; use helper `launch` for a GUI program in the interactive session. GUI launch uses the same user's desktop shell as parent when the scheduler disallows job breakaway. If that is unavailable, report `LAUNCH_DETACH_DENIED` instead of starting an application that will die with the worker.

For an authorized operation on an administrator window, request `params.elevated:true` on the desktop call (`windows`, `observe`, `act`, or `launch`). This is supported starting with helper 0.2.0, advertised as `elevated_worker` in `status.result.capabilities`; deploy the newer release before using it. `status.result.privileges` describes the SSH/helper caller and must show `elevated:true` with high/system integrity. A normal-privilege caller gets `ELEVATION_REQUIRED`; the helper does not turn an unprivileged SSH login into an administrator.

```pwsh
& '<skill>/scripts/Receive-Ra.ps1' -Computer win-test -Request '{"op":"windows","params":{"elevated":true}}'
$request = @{op='act';params=@{elevated=$true;window=123456;actions=@(@{type='key';keys='Ctrl+S'});observe=$true}} | ConvertTo-Json -Depth 12 -Compress
& '<skill>/scripts/Receive-Ra.ps1' -Computer win-test -Request $request
```

Requests without `elevated:true` continue to use the ordinary worker policy. Check the response's top-level `worker` field for actual elevation, integrity and session; windows and launches also report target `privileges` when available. There is no sticky high-privilege session: each call chooses its mode. Use high privilege when the task needs it, without asking again for authorization already given. `ELEVATION_REQUIRED` for a higher-integrity target can be resolved by observing and issuing the authorized operation in elevated mode; do not replay already completed parts of a batch. `ELEVATION_DENIED` means Scheduler rejected the request, while `ELEVATION_FAILED` means the requested integrity was not achieved; neither silently falls back to ordinary privilege. Elevated `launch` keeps the child's high privilege while detaching it from the temporary worker.

Use SCP/SFTP directly for files, including game packages and logs. Quote paths with spaces as arguments; keep JSON in a PowerShell variable or input file instead of nesting it in remote shell quotes. No SMB/admin shares are necessary. Do not use helper keystrokes to type executable shell commands into a desktop terminal.

```pwsh
scp './测试包.zip' 'win-test:RemoteAgent/测试包.zip'
scp 'win-test:RemoteAgent/中文 测试.txt' './下载的结果.txt'
```

## Windows 11 local-account console login (experimental, 0.4.0+)

Use this for a configured Windows 11 machine that has booted to the login screen or whose graphical user has logged out. SSH must already work with an administrator/high-integrity caller; sshd should start automatically for reboot recovery. The target is the same local account as the SSH login. SSH authentication alone does not log on a graphical desktop. No MCP, resident helper, SCM service, credential provider or persistent autologon setup is needed.

Inspect status and desktop_probe first. A pre-login console normally has an empty `user`; its session ID may change after logout. Use the current console_session_id, not a remembered ID. The helper restricts login to the SSH account and refuses another user's console or an existing disconnected session rather than silently switching/logging out anyone.

Send `op:login` with `params.password` through the normal in-memory JSON stdin path. Optional `params.user` accepts the same local account's bare name, `COMPUTER\name` or `.\name`; other accounts are rejected. The default login timeout is 120000 ms in both helper and receiver to allow user-profile startup. Do not put credentials in argv, files, logs or EncodedCommand. Reuse credentials and authorization already supplied by the user; never guess a password.

The temporary SYSTEM worker confirms the current account label against unambiguous local account information, then the password field and focus, submits once, and checks the actual WTS user SID/session. Supported labels are the selected account's standard LogonUI title. Unrecognized/multiple-account selection layouts return ACCOUNT_SELECTION_REQUIRED without inputting a password; this prototype does not expose general secure-desktop clicking. PIN/Hello, forced Ctrl+Alt+Del and account-setup/OOBE pages are separate unvalidated paths.

Interpret the result:

- authenticated=true / desktop_ready=true: Windows confirms the account and the unlocked Default desktop; proceed with normal GUI commands.
- authenticated=true / desktop_ready=false: credentials worked but the user environment is still preparing; poll status/desktop_probe, do not submit credentials again.
- already_signed_in=true / password_submitted=false: no-op; the account was already signed in. A locked existing desktop returns USE_UNLOCK instead.
- PASSWORD_FIELD_UNAVAILABLE / ACCOUNT_SELECTION_REQUIRED: password_submitted=false; inspect the login UI before taking another step.
- LOGIN_NOT_CONFIRMED, timeout or disconnect after submission: inspect the actual console account/state before any explicit retry. No automatic credential replay.

Validated on win-test with logout and reboot before any graphical login. Windows 11 is the login implementation scope; the older helper compilation target does not imply this feature is supported on old Windows.

## Existing Windows session password unlock (experimental, 0.3.0+)

Use `desktop_probe` from an already elevated SSH process to inspect the actual input desktop, `result.wts.locked`, display outputs, optional screenshot and UIA. It uses a temporary SYSTEM worker. A Windows-reported locked state takes precedence over a cached Default-desktop image. This is read-only; it does not sign in, unlock, or change desktop settings.

```pwsh
& '<skill>/scripts/Receive-Ra.ps1' -Computer win-test -Request '{"op":"desktop_probe","params":{"format":"jpeg"}}'
```

For an authorized Windows unlock with a supplied valid password, use `op:unlock` and `params.password` in the JSON stdin request, optionally `session_id`. The SSH process must already be administrator/high; the session must be active, already logged in as the SSH account. Do not require RDP. Do not log the request or put credentials in files, SSH options, remote argv or a new pwsh process's command-line arguments. Build the request in memory and call Receive-Ra.ps1 within the same PowerShell process. Use the session's Windows password; an SSH key does not supply a Windows password. Never guess a password.

The same EXE is staged temporarily with an administrators/SYSTEM-only writable ACL. A temporary SYSTEM task launches the target-session worker; no SCM service is installed. The worker requests display wake for this call, locates a unique enabled Windows password field and verifies its focus and session account, then submits once. It may dismiss an observed pre-existing incorrect-password notice and reports `prior_error_dismissed`. Success requires actual Windows unlocked state plus Default desktop and the same account/session. `already_unlocked:true` is a no-op with `password_submitted:false`; no password is required for that no-op.

No password or authentication attempt is replayed automatically. For `PASSWORD_FIELD_UNAVAILABLE`, inspect with desktop_probe; provider selection, SAS, an unready interface or unavailable display can prevent the prototype. For `UNLOCK_NOT_CONFIRMED`, `TIMEOUT`, connection loss or `outcome_unknown`, inspect Windows state before any explicit retry. Prefer 30000 ms or more to allow the lock interface to initialize. Do not treat a successful API input enqueue as an unlocked desktop.

Validated on Windows 11 x64 with the local console and password provider, including an API-requested display-off test. Initial sign-in after logout/boot uses the separate 0.4.0 login command above. Disconnected-session reconnection, PIN/Hello/smartcard/MFA, forced Ctrl+Alt+Del and old Windows are not validated by this unlock prototype. It does not change SAS/UAC/autologon policy. No physical monitor is not the same as no graphical session: a VM adapter, virtual display or dummy display can provide output, but a usable display surface still needs verification.

## First deployment and system-wide remote PowerShell 7

Deploy-Ra.ps1 now checks remote PowerShell 7 automatically as part of authorized deployment. It reuses a standard global 7.x installation; if missing on the supported x64 Windows platform, it obtains the pinned official 7.6.9 MSI, checks SHA256 and Microsoft Authenticode, uploads it and a temporary bootstrap batch, and installs globally into Program Files with system PATH. Remote bootstrap only uses cmd/certutil/msiexec; no remote PowerShell is needed to install PowerShell. The native helper itself remains independent of pwsh. The existing Windows controller scripts still require controller PowerShell 7; other controller bootstrap commands are not yet packaged/tested.

The MSI is cached on the controller under `%LOCALAPPDATA%\RemoteAgent\cache\powershell\7.6.9\win-x64\`. Cache contents are validated before reuse; partial downloads use unique staging names and invalid cache entries are replaced. The 112 MiB MSI is not embedded in the Skill. `-PowerShellMsiPath` supplies an offline official MSI, `-PowerShellCacheDirectory` overrides the controller cache root, and `-SkipPowerShell` deploys just the native helper. The standalone Install-Pwsh.ps1 uses `-MsiPath` / `-CacheDirectory` for those options. A failed runtime setup reports that the helper deployment already completed; inspect state before retrying. A required reboot is reported, not performed automatically.

This does not change SSH's default shell or enable WinRM. Skill commands explicitly invoke PowerShell 7 over the existing SSH channel:

```pwsh
ssh -T win-test 'pwsh -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.ToString()"'
scp './task.ps1' 'win-test:RemoteAgent/task.ps1'
ssh -T win-test 'pwsh -NoProfile -NonInteractive -File C:\Users\user\RemoteAgent\task.ps1'
```

Use the actual SSH home path on the target for uploaded scripts. Prefer uploading complex UTF-8 scripts instead of composing several levels of shell quotes. If PATH is stale, explicitly invoke the installed `Program Files\PowerShell\7\pwsh.exe` with correct path quoting. WinRM/Enable-PSRemoting and an SSH PowerShell subsystem are not prerequisites for this plain SSH execution. Keep credentials out of script/command-line text; login/unlock credentials use the helper's stdin protocol.

## Authorization and current capabilities

Carry out the user's authorized automation, including ordinary application sign-in using supplied credentials and clicking confirmations that belong to the requested task. Reuse authorization already given. Do not treat login, confirmation, or elevation as categorically forbidden by this Skill; determine whether the requested operation is authorized and technically supported. Follow any restrictions of the actual host tools being used.

Ordinary GUI commands require an unlocked graphical session and use ordinary privilege unless elevated mode is requested. A high-integrity worker operates administrator applications on the ordinary desktop. Helper 0.3.0 provides temporary SYSTEM password unlock; 0.4.0 also signs in to the same local Windows 11 account after logout/boot. These scoped commands do not expose general UAC secure-desktop input. If an unavailable capability is needed, explain the specific implementation gap or Windows permission boundary instead of claiming this Skill prohibits the operation. Administrator operations that can already be completed through an authorized SSH command can use that route.

Report `NO_INTERACTIVE_SESSION`, `DESKTOP_LOCKED`, or access errors with the actual state. Re-deploying the same helper does not create a graphical session or increase its privileges. Persistent OS autologon changes credential storage and machine logon behavior, so configure it only when that setup is part of the user's request.
