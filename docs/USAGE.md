# Advanced usage

**English** | [简体中文](USAGE.zh-CN.md) · [Back to README](../README.md)

These commands are for agents, developers and troubleshooting. For normal
installation, use the [copyable agent prompt](../README.md#install-copy-this-to-your-agent).
Current version: **0.4.2**, protocol **1**.

## Connect and deploy

Use an existing SSH alias such as `win-test`, or `user@host`. The Windows
controller scripts require existing PowerShell 7 and SSH/SCP. Native SSH owns
authentication; unattended repeated calls need a working unattended mechanism.

From an extracted release directory:

```pwsh
& ./skills/ssh-remote-desktop/scripts/Deploy-Ra.ps1 -Computer win-test -Executable ./windows-x64/ra.exe
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"status"}'
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"windows"}'
```

For a source checkout, the packaged binary is under
`dist/0.4.2/windows-x64/ra.exe`. Check its release SHA256 before deployment.
The default remote path remains `RemoteAgent/ra.exe` under the SSH user's home;
this path, the `ra.exe` command and internal task/pipe prefixes retain the early
prototype names to preserve existing deployments.

Deployment compares hashes, verifies a staged helper and reuses identical files.
By default it also checks remote PowerShell 7 and globally installs a verified
official MSI if missing on the supported platform. `-SkipPowerShell` deploys only
the native helper. The helper itself does not depend on PowerShell.

`-SshOption @('-i','<key>')` passes SSH options. Use `-o Port=...` for a custom
port because `ssh -p` and `scp -p` mean different things.

## Observe, act and verify

Choose a real HWND returned by `windows`:

```pwsh
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"observe","params":{"window":123456}}'
```

The result includes an absolute local `image_path`. The screenshot already came
back in the same SSH response: there is no second network fetch. Use the agent's
native image tool to view it; printing its path or Base64 does not provide image
input to the model. Codex can use `view_image`, Claude Code `Read`, and DeepSeek
Harness its supported image route. The model must support vision.

Prefer an unambiguous observed UIA selector when the control supports it.
Otherwise use physical screenshot coordinates. Re-observe after moving a window;
pass its observed `expected` PID, process start time and bounds to catch stale
targets. GDI captures visible pixels, so focus a covered window before observing.

```pwsh
$request = @{
    op='act'
    params=@{
        window=123456
        actions=@(
            @{type='focus'},
            @{type='click';x=200;y=200},
            @{type='type';text='中文测试'},
            @{type='key';keys='Ctrl+S'},
            @{type='wait';ms=200}
        )
        observe=$true
    }
} | ConvertTo-Json -Depth 12 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
```

A batch accepts 1–50 actions and stops at the first failure. Check `completed`,
`failed_index` and `outcome_unknown`. Input completion means Windows accepted the
events, not that an application finished saving. After a timeout or disconnect,
observe current state before an explicit retry; never replay a whole batch blindly.

## Programs and files

Use SSH for headless commands, SCP/SFTP for files, and helper `launch` for a GUI
program in the interactive desktop. The launched program can outlive the worker:

```pwsh
$request=@{op='launch';params=@{exe='C:\Windows\System32\notepad.exe';args=@('C:\Users\user\RemoteAgent\test.txt')}} | ConvertTo-Json -Depth 8 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
scp './test-package.zip' 'win-test:RemoteAgent/test-package.zip'
scp 'win-test:RemoteAgent/result.log' './result.log'
```

If Windows prevents a GUI child from outliving the temporary scheduler job and
no usable desktop-shell parent is available, launch returns `LAUNCH_DETACH_DENIED`.

## On-demand elevated worker

Add `params.elevated:true` to `windows`, `observe`, `act` or `launch` when the task
requires it. The SSH caller must already have administrator/high integrity;
check `status.result.privileges`. Direct text commands accept `--elevated`.

```pwsh
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"windows","params":{"elevated":true}}'
```

The response's `worker` field reports the actual token, PID and session. The next
ordinary call still uses ordinary-worker policy. An insufficient caller receives
`ELEVATION_REQUIRED`; a requested high-privilege worker never silently falls back.
This operates elevated applications on the ordinary desktop, not arbitrary UAC
secure-desktop prompts. No UAC configuration changes are needed.

## Experimental Windows 11 login and unlock

An elevated SSH login can request `login` after boot/logout for the same local
account, or `unlock` for its existing active locked session. These use temporary
SYSTEM workers and a known Windows password; SSH key authentication does not
provide that password. They do not configure persistent auto-login or a resident
service. `desktop_probe` is the read-only diagnostic operation.

Build credential requests in memory and call the receiver in the same PowerShell
process. Do not save the request, print it or pass it to a new process as argv:

```pwsh
# $passwordText contains the password supplied for this authorized operation.
$request=@{op='login';params=@{password=$passwordText}} | ConvertTo-Json -Depth 8 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
$request=$null; $passwordText=$null
```

Use `op='unlock'` for an existing locked session. Login defaults to a 120-second
timeout; other requests default to 30 seconds. Each operation submits credentials
once. Login verifies the actual Windows user SID and session afterward.

| Result | Next step |
| --- | --- |
| `authenticated=true`, `desktop_ready=true` | Continue with desktop work. |
| `authenticated=true`, `desktop_ready=false` | Observe profile startup; do not submit again. |
| `already_signed_in=true` or `already_unlocked=true` | No-op; `password_submitted=false`. |
| `USE_UNLOCK` | The account already has a locked desktop; use unlock. |
| `ACCOUNT_SELECTION_REQUIRED`, `PASSWORD_FIELD_UNAVAILABLE` | Inspect the login UI; no credential submission occurred. |
| Timeout, disconnect, `LOGIN_NOT_CONFIRMED` or `UNLOCK_NOT_CONFIRMED` | Inspect actual Windows state before any explicit retry. |

Only the selected local account's standard Windows 11 password screen has been
validated. Complex account selection, PIN/Hello/MFA, forced Ctrl+Alt+Del,
new-profile onboarding and disconnected-RDP reconnection remain separate paths.

## Remote PowerShell 7 and offline cache

`Deploy-Ra.ps1` reuses a standard global 7.x installation. Otherwise it verifies
the pinned Microsoft MSI's SHA256 and Authenticode signature, caches it under
`%LOCALAPPDATA%/RemoteAgent/cache/powershell/7.6.9/win-x64/`, and installs remotely.
It reports required restarts without automatically restarting Windows.

Use `-PowerShellMsiPath` for an offline MSI, `-PowerShellCacheDirectory` to change
the cache root, or standalone `Install-Pwsh.ps1` with `-MsiPath`/`-CacheDirectory`.
The roughly 112 MiB MSI is not embedded in this project's ZIP. If installation
fails after helper deployment, the script reports that distinction.

No WinRM, SSH PowerShell subsystem or SSH default-shell change is required:

```pwsh
ssh -T win-test 'pwsh -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.ToString()"'
scp './task.ps1' 'win-test:RemoteAgent/task.ps1'
ssh -T win-test 'pwsh -NoProfile -NonInteractive -File C:\Users\user\RemoteAgent\task.ps1'
```

Use the target's actual user path. If PATH has not refreshed, explicitly invoke
`C:\Program Files\PowerShell\7\pwsh.exe` with correct path quoting.

## Desktop and transport notes

A logged-in, unlocked local console with valid display output needs no RDP
connection. RDP minimization/disconnection can suspend rendering, disconnect the
session or lock it. No physical monitor does not prevent login, but screenshots
need a usable real, VM, virtual or dummy display output.

SSH usually runs in Session 0. The same EXE enters the selected desktop via a
temporary scheduled task and returns results through a local named pipe. Normal,
elevated and SYSTEM workers all end after their request. A crash or power loss
can leave temporary files or triggerless task registrations for later cleanup.

Protocol 1 returns a JSON line followed by exactly `image_bytes` raw bytes. Use
`ssh -T`, keep stderr separate and never feed image output through a text pipeline.
Read-only UIA has a time budget; a blocked modifying provider can still cause a
request timeout with an unknown action outcome.

See the [full protocol](../skills/ssh-remote-desktop/references/protocol.md),
[architecture](IMPLEMENTATION_PLAN.md), [test results](TEST_RESULTS.md) and
[contributor instructions](../CONTRIBUTING.md).
