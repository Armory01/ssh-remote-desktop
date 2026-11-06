# SSH Remote Desktop protocol 1

`ra status`, `ra windows [--elevated]`, `ra observe [--window HWND] [--elevated]`, `ra launch --exe PATH [--arg VALUE] [--elevated]` are convenient text commands. `ra call` reads one UTF-8 JSON request from stdin until EOF. The structured form is preferred for paths, Chinese, quotes and batches.

```json
{"protocol":1,"id":"optional-unique-id","op":"observe","timeout_ms":30000,"params":{"window":123456}}
```

`protocol` defaults to 1. `id` is generated if omitted. `timeout_ms` is 1000–120000; default 30000, except `login` defaults to 120000 in both helper and receiver. Desktop operations accept `session_id`, `elevated` (boolean, default false), and, for development only, `force_bridge`. Unknown protocol versions fail. Every call returns exactly one JSON header line, followed immediately by exactly `image_bytes` raw bytes. No image means zero bytes. Header limit: 1 MiB; image limit: 32 MiB. Exit 0 means success; exit 1 is a helper error. SSH exit 255 is a transport failure, whose action outcome may be unknown.

```json
{"protocol":1,"id":"...","ok":true,"result":{},"error":null,"image_bytes":12345,"mime_type":"image/png"}
```

Errors contain `code`, `message`, `details`. The receiver adds `transport_exit_code` and `image_path`; these are local fields, not sent by the helper. Never use a PTY for remote binary stdout (`ssh -T`). A local terminal for authentication is fine. Do not merge stderr into stdout. `StreamReader`, text pipelines and older PowerShell `>` can corrupt binary images. Read the header as bytes through LF, then copy exactly the declared byte count. Codex/Claude/DeepSeek consume the saved image through their own image tool in a second Agent tool call; no screenshot fetch round trip is needed.

## Operations

| op | params | result |
|---|---|---|
| `status` | none | version, account, SSH process session, all WTS sessions (including empty-user pre-login/service/listener rows), console_session_id, uptime_ms, OS/native architecture, caller privileges, capabilities |
| `windows` | optional session_id | visible titled windows: hwnd, pid, process_start, bounds, title, class |
| `observe` | optional window, expected, screenshot (default true), uia (default true for window), format (`png`/`jpeg`), region `{x,y,width,height}` | window, desktop, capture mapping, bounded UIA tree and optional image |
| `launch` | exe (absolute path), args string array, optional cwd | pid, process_start, session_id, detached, actual child privileges |
| `act` | window, optional expected, actions array (1–50), observe (default false), screenshot/uia/format | completed actions; optional final observation |

## On-demand elevated worker (helper 0.2.0+)

Protocol remains 1; `elevated_worker` in `status.result.capabilities` advertises support. Set `params.elevated:true` on any desktop operation to request a high-integrity worker. The SSH/front-end process must already be elevated with high/system integrity. Scheduler uses `TASK_RUNLEVEL_HIGHEST` for that call; default false uses `TASK_RUNLEVEL_LUA`. Every call is independent. Neither path changes UAC settings, saves administrator credentials, nor installs a persistent privileged service.

```json
{"op":"observe","params":{"elevated":true,"window":123456}}
```

`status.result.privileges` describes the front-end process, not a desktop worker. Desktop responses add a top-level `worker` object (`elevated`, `integrity_level`, `integrity_rid`, `pid`, `session_id`), including errors produced after the worker starts. Window descriptions and launch results include actual target `privileges`; inaccessible target tokens are reported as null. High-integrity child launches are verified before their main thread is resumed, so a requested elevated launch cannot silently execute with the shell's ordinary token. On systems with UAC disabled or special built-in accounts, Windows' ordinary run-level policy may itself be elevated; use actual reported privileges.

An ordinary worker rejects a target with a known higher integrity level before input using `ELEVATION_REQUIRED`. An unelevated caller requesting a high worker gets the same code before any worker is started. `ELEVATION_DENIED` means task registration was rejected; `ELEVATION_FAILED` means the worker/child did not obtain the requested high integrity. These cases do not fall back to ordinary privilege. Creating a desktop session, unlocking Windows, and operating the UAC secure desktop remain separate capabilities.

Coordinates are absolute physical desktop pixels, not image-relative. For a capture with origin `(x0,y0)` and scale 1, add `(x0,y0)` to the image point. Regions can include negative monitor origins. Captures are clipped to the virtual desktop and capped at 32 megapixels. GDI captures visible pixels, including occluding windows. UIA is optional and may be absent or truncated; use `uia:false` for a screenshot-only observation when a provider is failing. Current UIA traversal is capped at 200 nodes/depth 8 for observation; selection searches at most 500 nodes/depth 12.

Window identity guard:

```json
{"window":123456,"expected":{"pid":123,"process_start":134000000000000000,"bounds":{"x":100,"y":100,"width":650,"height":340}}}
```

Use 64-bit integer-preserving JSON handling (PowerShell/.NET supports it). JavaScript Number cannot exactly represent `process_start`; preserve the JSON integer or omit that field and retain pid/bounds.

## Actions

```json
{"op":"act","params":{"window":123456,"actions":[
  {"type":"focus"},
  {"type":"click","x":200,"y":200},
  {"type":"type","text":"中文 English"},
  {"type":"key","keys":"Ctrl+S"},
  {"type":"wait","ms":200}
],"observe":true}}
```

| type | fields |
|---|---|
| focus | none; restore/activate target window |
| click / double_click | x,y; optional button left/right/middle |
| type | text; Unicode input, not clipboard paste; focus an observed editable control first |
| key | keys string joined by `+`, or array; Ctrl, Shift, Alt, Win, A–Z, 0–9, F1–F24, Enter, Esc, Tab, Space, Backspace, Delete, Insert, Home, End, PageUp, PageDown, arrows |
| scroll | delta (wheel units; down -120), optional x/y |
| drag | x,y,to_x,to_y, duration_ms (0–2000) |
| wait | ms (0–10000, within request budget) |
| invoke | selector |
| set_value | selector, text |
| uia_scroll | selector, direction (positive down, negative up) |

UIA selector: use observed `runtime_id`, `automation_id` or `name`, optionally `control_type`. All supplied fields must match exactly and uniquely. IDs can change between observations; a missing, ambiguous or incomplete search returns `STALE_TARGET`.

```json
{"op":"act","params":{"window":123456,"actions":[
 {"type":"set_value","selector":{"automation_id":"101"},"text":"中文内容"},
 {"type":"invoke","selector":{"automation_id":"102"}}
]}}
```

`completed` means the API call or input enqueue succeeded; verify the resulting application state. If final observation fails after actions, `actions_completed:true` and `observation_failed:true` preserve that distinction. A partial batch failure stops subsequent actions. This protocol is request/response, with no streaming video or automatic side-effect retries.

Useful errors: `NO_INTERACTIVE_SESSION`, `SESSION_DISCONNECTED`, `DESKTOP_LOCKED`, `AMBIGUOUS_SESSION`, `DESKTOP_ACCESS_DENIED`, `ELEVATION_REQUIRED`, `ELEVATION_DENIED`, `ELEVATION_FAILED`, `WORKER_START_TIMEOUT`, `DESKTOP_BUSY`, `STALE_TARGET`, `FOCUS_FAILED`, `INPUT_REJECTED`, `UIA_UNAVAILABLE`, `UNSUPPORTED_PATTERN`, `LAUNCH_DETACH_DENIED`, `CAPTURE_FAILED`, `TIMEOUT`. Windows 11 initial sign-in uses the 0.4.0 login command below; general secure-desktop input remains outside the implemented commands. These are implementation limits, not a blanket prohibition on authorized automation.

## Temporary SYSTEM desktop operations (experimental, 0.3.0+)

These operations require an already high-integrity administrator SSH/helper caller. unlock targets an existing active same-account desktop; 0.4.0 desktop_probe also reaches a signed-out console. They are independent of `params.elevated`. `status` advertises `experimental_desktop_probe` and `experimental_password_unlock`; protocol remains 1. `status.result.console_session_id` reports the physical console; each user session includes `locked:true/false/null` (unknown). The Win7/2008 R2 WTS lock-flag reversal is decoded, but those systems are not tested.

| op | params | result |
|---|---|---|
| `desktop_probe` | optional session_id, screenshot/uia/format | input_desktop, window_station, wts.locked/session_flags/station, display dimensions/adapters, desktop inventory; optional screenshot/UIA |
| `unlock` | optional session_id; password string for a locked desktop, 1..256 UTF-16 units without NUL | unlocked, already_unlocked, password_submitted, session_id; prior_error_dismissed if an observed old incorrect-password notice was dismissed |

```json
{"op":"desktop_probe","params":{"format":"jpeg"}}
{"op":"unlock","timeout_ms":30000,"params":{"session_id":1,"password":"<supplied valid Windows password>"}}
```

Do not save or log the credential-bearing request. Send it through the same `ra call` stdin/wire; no password argv option exists. In PowerShell call the receiver in the existing process with an in-memory request, not as a password-bearing new process command line. Passwords are not returned, and password-field values are not read. JSON parse errors omit the original malformed input. Known owned buffers are cleared where practical, without promising that every host/OS memory copy is erased.

A SYSTEM task starts the same executable in a protected temporary directory. A target-session worker uses the existing Winlogon SYSTEM primary token and a kill-on-close lifetime job. Before transmitting the request, the front end checks the named-pipe client's actual PID, account SID and session. Registration is removed before GUI work; the launcher and front end collect results and clean up. No SCM service or persistent logon task is created. Abnormal host failure may leave temporary registrations/files; do not delete unrelated Windows Temp content.

Unlock serializes concurrent unlock requests and requests display wake for its lifetime. It uses non-submitting input to dismiss the lock curtain, waits for a unique LogonUI password edit, checks its focus and the original same-user session, clears its old contents, types and submits once. It never forces `SwitchDesktop` or changes SAS/UAC/autologon policy. A pre-existing incorrect-password notice can be acknowledged using its observed OK button. The only submit Enter follows validated password focus and typing. Success is verified from the Windows session lock state and Default desktop, then the front end verifies account/session again. An already-unlocked Default desktop returns a no-op without credentials; a different secure prompt on an unlocked session returns `NOT_LOCKED` without input.

Errors include `SYSTEM_STAGE_FAILED`, `SYSTEM_START_FAILED`, `SYSTEM_REQUIRED`, `PASSWORD_FIELD_UNAVAILABLE`, `SESSION_STATE_UNKNOWN`, `DESKTOP_CHANGED`, `NOT_LOCKED` and `UNLOCK_NOT_CONFIRMED`, as well as existing permission/timeout errors. `PASSWORD_FIELD_UNAVAILABLE` includes `password_submitted:false`; no password was sent. After a submitted password, timeout or disconnect, inspect state before any explicit retry. No automatic credential retry or guessing is implemented. A disconnected session is not silently reconnected; signing in a logged-out user requires the explicit 0.4.0 login operation below. Forced SAS, other credential providers and headless display configurations require separate validation.
## Windows 11 local-account console login (experimental, 0.4.0+)

`experimental_local_password_login` advertises this command. It uses the same temporary SYSTEM mechanism as unlock; it does not install a service or credential provider, configure OS autologon, create users, or start Explorer as a substitute for Winlogon.

```json
{"op":"login","params":{"user":"COMPUTER\\localuser","password":"<supplied Windows password>"}}
```

`params.user` is optional. It must identify the SSH account, using its bare local name, `COMPUTER\name`, or `.\name`; other accounts fail with `UNSUPPORTED_ACCOUNT` before a worker is started. The caller must already be high-integrity administrator, and the OS must be Windows 11. Default timeout is 120000 ms for login; explicitly supplied timeouts retain the normal 1000..120000 range. Credentials use the same stdin-only protocol, not argv, files, logs or EncodedCommand.

Selection follows the current physical console_session_id and accepts a pre-login WTS row with an empty user. Logout can destroy the previous session and create a new ID; never reuse a remembered ID. `desktop_probe` accepts an explicit current console session_id before login, or an explicit existing same-user active session for prior workflows. Service Session 0 and listener rows are never login targets. An unavailable console returns LOGIN_NOT_READY. A console belonging to another account returns OTHER_USER_SESSION. An existing user session elsewhere is not silently moved/reconnected.

The front end obtains the SSH account's local name, SID and unique display labels from Windows. It sends this metadata only through the verified SYSTEM pipe. The worker matches the selected standard LogonUI account title and a unique enabled password edit; duplicate local display names are not accepted as identity proof. A supported standard selected-account title is required. ACCOUNT_SELECTION_REQUIRED is returned without password submission for unrecognized or ambiguous account-selection UI. Complex multi-account selection, Other User layouts and additional credential providers have not been broadly validated.

The worker verifies the console is still signed out immediately before input, verifies the actual password focus, and submits once. Afterward Windows must expose the requested account SID in the active session. Authenticated state and Default-desktop readiness are separate:

```json
{"authenticated":true,"desktop_ready":true,"already_signed_in":false,"password_submitted":true,"session_id":2,"account":"COMPUTER\\localuser"}
```

If profile startup exceeds the wait budget after authentication, the reply may instead have authenticated=true / desktop_ready=false. Continue read-only status/desktop_probe polling; do not authenticate again. An already-signed-in unlocked account returns already_signed_in=true / password_submitted=false without touching its UI; desktop_ready can be false for another secure prompt. An already-signed-in locked account returns USE_UNLOCK.

Password-field/account-detection failures report password_submitted=false. LOGIN_NOT_CONFIRMED after submission carries outcome_unknown=true: observe actual console state before any explicit retry. There is no credential-guessing or automatic retry loop. Forced Ctrl+Alt+Del, PIN/Hello/MFA, new-profile setup pages and old Windows are separate unvalidated scenarios. Normal GUI operations after sign-in continue to use their ordinary/high worker policies; SYSTEM privilege is not sticky.

## Deployment-managed PowerShell 7 (0.4.0 attachments)

Deploy-Ra.ps1 automatically invokes Install-Pwsh.ps1 after helper deployment or reuse. The result contains a `powershell` object with changed/path/version, or skipped/reason. `-SkipPowerShell` opts out. `-PowerShellMsiPath` supplies the pinned offline package and `-PowerShellCacheDirectory` overrides the controller cache. Standalone Install-Pwsh.ps1 uses `-MsiPath` and `-CacheDirectory`.

The controller caches the official pinned 7.6.9 x64 MSI under LocalAppData/RemoteAgent/cache/powershell/7.6.9/win-x64. SHA256 and Microsoft Authenticode are checked before cache reuse or installation; partial download files are unique and removed on failure. A missing target installation consumes the verified cache (cache_hit=true) or downloads and verifies one (cache_hit=false). An already-installed remote 7.x is reused without accessing/downloading an MSI, so cache_hit is null. The MSI is not embedded in the Skill or helper archive.

Remote setup uses cmd/certutil/msiexec and a temporary batch file to avoid nested SSH/CMD quoting. Batch and MSI uploads are removed on completion, with a verbose installation log retained for diagnosis. Installation is global with system PATH, does not auto-reboot, does not configure SSH DefaultShell/subsystems, and uses ENABLE_PSREMOTING=0. Invoke pwsh explicitly over SSH for scripts; WinRM is not involved. powershell.exe remains Windows PowerShell 5.1 and should not be confused with pwsh.exe.