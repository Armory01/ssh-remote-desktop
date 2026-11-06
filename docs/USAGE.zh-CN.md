# SSH Remote Desktop 详细用法

[English](USAGE.md) | **简体中文** · [返回 README](../README.zh-CN.md)

C++ 单文件 Windows 桌面 helper，通过现有 SSH 调用。控制端使用 Agent 已有的 Shell、SSH/SCP 和图片读取工具，不需要 MCP、本地 RemoteAgent 客户端或额外 C++ 运行库。

当前版本 **0.5.0**：窗口枚举、PNG/JPEG 截图、鼠标/键盘/中文输入、UIA 读取与操作、GUI 程序启动、动作批处理、按需高权限 worker、实验性的临时 SYSTEM 密码解锁/Windows 11 本地账户登录，以及自动部署和更新。新增可选的 [Windows 11 x64 无显示器支持](HEADLESS.zh-CN.md)：由 Agent 通过 SSH 按需安装 VirtualDrivers 驱动，不增加 helper 常驻服务。远端先开通管理员 SSH；Agent 可在开机后尚无人登录或注销后，按需登录同一本地账户的控制台。

## 使用

当前控制端脚本使用已有的 **PowerShell 7**。本仓库开发环境已具备它，不会自动安装其他运行环境。`win-test` 可以是既有 SSH Host 别名，也可以直接换成 `user@192.0.2.10`。

以下路径以源码仓库为基准；如果使用解压后的发布包，将 EXE 路径换成 `./windows-x64/ra.exe`。

```pwsh
# 一次部署，之后相同文件直接复用；不同文件先校验再替换。
& ./skills/ssh-remote-desktop/scripts/Deploy-Ra.ps1 -Computer win-test -Executable ./dist/0.5.0/windows-x64/ra.exe

& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"status"}'
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"windows"}'
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"observe","params":{"window":123456}}'
```

最后一条返回结构化结果及本地图片绝对路径 `image_path`。Agent 再用自己的图片工具打开它。截图已经随同一次 SSH 调用传回，不需要第二次连接取图，也不经过 Base64 文本。

```pwsh
$request = @{
    op = 'act'
    params = @{
        window = 123456 # 从 windows 返回的 HWND 选择
        actions = @(
            @{type='focus'},
            @{type='click'; x=200; y=200},
            @{type='type'; text='中文测试'},
            @{type='key'; keys='Ctrl+S'},
            @{type='wait'; ms=200}
        )
        observe = $true
    }
} | ConvertTo-Json -Depth 12 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request

# GUI 启动使用 helper；普通命令和文件继续使用 SSH/SCP。
$request = @{op='launch';params=@{exe='C:\Windows\System32\notepad.exe';args=@('C:\Users\user\RemoteAgent\测试.txt')}} | ConvertTo-Json -Depth 8 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
scp './测试包.zip' 'win-test:RemoteAgent/测试包.zip'
scp 'win-test:RemoteAgent/日志.txt' './日志.txt'
```

认证由原生 SSH 处理。密码不会写入 helper 或 Skill。交互式调用可使用密码；要让 Agent 无人值守执行多次连接，应使用已配置的 SSH 密钥或其他可用认证机制。每条命令可以新建 SSH 连接，首版不增加连接池；将连续操作合并成 `act` 可减少往返。

## 按需高权限

在已授权的管理员窗口操作中，给这一次请求加上 `params.elevated:true`：

```pwsh
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"windows","params":{"elevated":true}}'
$request = @{op='launch';params=@{elevated=$true;exe='C:\Path\AdminApp.exe';args=@()}} | ConvertTo-Json -Depth 8 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
```

`windows`、`observe`、`act`、`launch` 都支持该参数；直接 CLI 对应 `--elevated`。SSH 进程本身必须已具备管理员高完整性权限，可通过 `status.result.privileges` 检查。临时任务使用 `TASK_RUNLEVEL_HIGHEST`，返回头的 `worker` 标明实际权限、PID 和会话；窗口和启动结果也报告目标权限。

默认请求仍采用普通用户任务策略，每次独立选择权限。普通 worker 遇到更高完整性窗口时，在输入前返回 `ELEVATION_REQUIRED`；没有足够权限的调用端请求高权限 worker 也返回该错误。不会自动重放已经执行的批次。请求高权限但未获得时明确报错，不降级执行。

高权限程序通过显式保留 worker 令牌来启动；即使为脱离临时任务而使用普通权限 shell 作为父进程，也会在程序主线程运行前核对其真实权限。无需常驻服务、保存额外管理员口令或修改 UAC 设置。高权限 worker 操作的是普通桌面中的管理员程序；Windows 密码解锁使用下面独立的临时 SYSTEM 路径；高权限 worker 本身不提供安全桌面通用输入。

## Windows 11 开机后／注销后密码登录

0.4.0 的 `login` 用于已经启动到登录界面、但控制台尚无图形用户的 Windows 11。要求管理员 SSH，目标限定为 SSH 账户对应的本地账户；不创建新账户，不安装凭据提供程序，不配置持久自动登录，不增加常驻服务。

```pwsh
# $passwordText 是本次授权使用的 Windows 账户密码，不输出或保存请求。
$request = @{op='login';params=@{user='WIN-TEST\user';password=$passwordText}} | ConvertTo-Json -Depth 8 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
$request = $null; $passwordText = $null
```

`user` 可省略，默认即 SSH 账户；不接受其他账户。`login` 默认超时 120000 ms，其他请求仍默认 30000 ms。登录前允许控制台用户名为空，通过当前 console_session_id 找目标，不能缓存注销前的会话 ID。SYSTEM worker 复用原有保护目录、管道和限时 Job，在登录界面确认可唯一匹配的本地账户标签及密码框，提交一次，随后验证实际 WTS 用户 SID 和 Default 桌面。

结果区分 `authenticated` 与 `desktop_ready`。用户环境仍在加载时可返回 authenticated=true / desktop_ready=false，Agent 应继续只读观察而不是再次提交密码。已经登录且未锁定时为 already_signed_in=true / password_submitted=false；已登录且锁定返回 USE_UNLOCK。账户无法唯一确认返回 ACCOUNT_SELECTION_REQUIRED，不输入密码。本轮验证的是标准本地密码界面，复杂账户选择布局、其他凭据提供程序、首次创建用户配置文件的引导页尚未全面验收。

在 win-test 上已实测注销后的新控制台会话、重启后 SSH 恢复且控制台无人登录的路径。测试由 Windows 的真实登录状态验证，不以启动 Explorer 或仅获取用户令牌代替登录。`status` 现在包含空用户名的服务／监听／登录前会话、console_session_id、uptime_ms 和 OS 信息；Agent 应选中当前控制台行，不把 Session 0 的 SSH 认证误认为已经进入图形桌面。

## 临时 SYSTEM 密码解锁原型

0.3.0 增加 `desktop_probe`（只读桌面、显示输出、锁定状态、截图与 UIA）和 `unlock`。要求已有管理员高完整性的 SSH 登录，unlock 的目标是同一账户已经登录的 active 桌面；新登录使用上节的 login。协议仍为 1，EXE 仍只有一个，没有 SCM 服务或常驻 worker。

```pwsh
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"desktop_probe","params":{"format":"jpeg"}}'
# $passwordText 来自本次授权使用的凭据；不要输出请求或保存到文件。
$request = @{op='unlock';params=@{password=$passwordText}} | ConvertTo-Json -Depth 8 -Compress
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request $request
$request = $null; $passwordText = $null
```

请求通过 SSH stdin 传输，不把密码放在远端命令行、任务参数或文件中；在同一个 PowerShell 进程内调用接收脚本，避免将含密码的 JSON 作为新 pwsh 进程的启动参数。helper 会清理自己控制的密码缓冲区，但不能承诺宿主/模型/系统内存完全没有副本。

前台为本次调用建立管理员和 SYSTEM 才可写的临时目录，复制同一个 EXE。临时 SYSTEM 任务先删除任务注册，再借助目标会话 Winlogon 的 SYSTEM 主令牌创建本次 worker；不向 Winlogon 注入代码。worker 受一个 kill-on-close Job 约束，launcher 等待其退出，前台清理 EXE 和目录。正常请求和超时路径均检查清理；突然断电/进程崩溃仍可能留下无触发器的任务注册或临时文件，需要管理员清理。没有安装额外服务。

解锁时先临时请求唤醒显示，使用不提交表单的输入唤醒锁屏，识别唯一的 LogonUI 密码框，核对目标账户和焦点，清空后输入并提交一次。若已有“密码不正确”提示，可点击观测到的确定按钮回到密码框，结果标明 `prior_error_dismissed`。不进行密码猜测或自动重试；找不到密码框返回 `PASSWORD_FIELD_UNAVAILABLE`。成功必须同时满足 Windows 会话为 unlocked、输入桌面为 Default、同一账户仍在原会话；已解锁时返回 `already_unlocked:true`，不输入密码。普通 GUI 命令会拒绝 Windows 报告为 locked 的会话，避免操作旧画面。

原型已在 win-test 的 Windows 11 x64、本地控制台上验证；没有 RDP 连接。属于密码解锁原型，不代表所有凭据提供程序、强制安全注意序列或无显示输出设备均已支持。不会修改自动登录、UAC、SAS 策略或保存系统登录密码。

## 首次部署与远端全局 PowerShell 7

`Deploy-Ra.ps1` 默认在 helper 部署／复用之后检查远端 PowerShell 7。已有标准全局安装则直接复用；缺少时自动取得经过校验的官方 7.6.9 x64 MSI，上传并静默全局安装到 Program Files，加入系统 PATH。`-SkipPowerShell` 可单独部署原生 helper；helper 本身不依赖 PowerShell，旧平台不会因此获得新的兼容性承诺。

控制端安装包缓存默认位于：

```text
%LOCALAPPDATA%\RemoteAgent\cache\powershell\7.6.9\win-x64\PowerShell-7.6.9-win-x64.msi
```

缓存按版本／架构分目录，下载先写唯一临时文件，SHA256 与微软签名通过后才放入正式缓存。已有缓存重新校验后复用，损坏缓存重新获取。MSI 不塞进 Skill 发布包；离线包可通过 `-PowerShellMsiPath`（独立安装脚本使用 `-MsiPath`）导入，缓存根目录可用 `-PowerShellCacheDirectory` 覆盖。

```pwsh
& ./skills/ssh-remote-desktop/scripts/Deploy-Ra.ps1 -Computer win-test -Executable ./dist/0.5.0/windows-x64/ra.exe
# 只部署 helper，或指定已下载的官方 MSI：
& ./skills/ssh-remote-desktop/scripts/Deploy-Ra.ps1 -Computer win-test -Executable ./dist/0.5.0/windows-x64/ra.exe -SkipPowerShell
& ./skills/ssh-remote-desktop/scripts/Install-Pwsh.ps1 -Computer win-test -MsiPath '<官方 MSI 路径>'
```

控制端附件使用已有的 PowerShell 7；远端首次引导只用 cmd、certutil、MSI，不要求预装 pwsh。为避免 SSH/CMD 多层引号错误，上传短期批处理调用 msiexec，结束清理批处理与 MSI，安装日志保留便于诊断。安装不会自动重启机器；3010 会报告 reboot_required。

不修改 SSH 默认 shell，也不启用 WinRM。SSH 的默认 shell 可以继续是 cmd，Skill 显式调用 pwsh 即可：

```pwsh
ssh -T win-test 'pwsh -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.ToString()"'
# 复杂脚本先 SCP 上传，再用 pwsh -NoProfile -NonInteractive -File 调用。
```

如果当前 SSH 环境的 PATH 尚未刷新，显式使用 `C:\Program Files\PowerShell\7\pwsh.exe`。win-test 上已实测卸载到缺少 pwsh，再通过部署流程从控制端缓存恢复全局 7.6.9，重启后仍能直接通过 SSH 调用。

## Skill 与 Agent

共享入口是 [skills/ssh-remote-desktop/SKILL.md](../skills/ssh-remote-desktop/SKILL.md)，详细命令见 [协议说明](../skills/ssh-remote-desktop/references/protocol.md)。

| Agent | 加载方式 | 图片入口 |
|---|---|---|
| Codex | 将 `ssh-remote-desktop` 整个目录复制到自己的 skills 目录，或在项目指令中引用该 SKILL.md | 当前可用的 `view_image` 或等效工具 |
| Claude Code | 同一目录放到 `~/.claude/skills/ssh-remote-desktop`，或由项目指令引用 | `Read` |
| DeepSeek Harness | 用所用版本的技能加载入口加载同一 SKILL.md，或作为任务说明读取；脚本和协议不变 | 图像路由支持时使用 `read_image` 或等效原生入口 |

不修改三个 Agent 的默认工具或模型路由。兼容性依靠命令/图片工具契约；当前实际端到端测试由本会话的 Codex 完成，未单独运行 Claude Code 和 DeepSeek Harness。无图像工具或无视觉模型的路由不能仅靠 Skill 获得识图能力。

## 桌面执行方式

SSH 进程通常位于 Windows Session 0，不能直接操作已登录桌面。前台 `ra.exe` 检查同账户已有会话，创建一次性的交互式计划任务，启动同一个 EXE 的内部 worker。worker 在桌面执行，通过仅本账户和 SYSTEM 可访问的本机命名管道回传结果，前台再写到 SSH stdout。

worker 在执行前删除自己的任务注册；前台也负责结束/清理。没有服务、开机任务或常驻 Agent。EXE 使用 Windows GUI 子系统避免计划任务弹出控制台，同时通过 Win32 标准句柄维持 CLI 输入输出。直接在交互式 `cmd.exe` 中运行 GUI 子系统程序时，若需要等待退出/退出码，使用 `start /wait`；SSH 和接收脚本已验证能等待并取回退出码。

GUI 程序优先通过 `CREATE_BREAKAWAY_FROM_JOB` 启动。任务调度器禁止脱离时，使用同账户同会话的桌面 shell 作为父进程，让程序继承 shell 的作业环境；高权限启动额外通过 `CreateProcessAsUserW` 显式保留高权限令牌。该方式使用 Windows 的[父进程属性 API](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattribute)。缺少可访问的桌面 shell 或所需令牌权限时返回 `LAUNCH_DETACH_DENIED`。

## 当前限制

- 实机已验证：Windows 11 x64、Windows OpenSSH、Windows 控制端 PowerShell 7；x64 helper 操作 x64/x86 程序，x86 helper 在该 x64 系统上的基本调用也通过。Win7 SP1/Server 2008 R2 SP1 是编译兼容目标，**尚未实机验证**，不是已认证支持范围。
- SSH 账户必须与图形登录账户相同。按需高权限 worker 要求调用端已有管理员权限；它不创建额外管理员身份。0.4.0 增加 Win11 本地账户在开机后或注销后的控制台密码登录；RDP disconnected 重连、PIN/Hello、强制 Ctrl+Alt+Del 和通用 UAC 安全桌面操作仍不在本轮验收范围。用户授权内的普通软件登录与确认操作可以自动执行，Skill 不将登录或提权列为一律禁止的操作。
- 截图是 GDI 可见桌面像素。被遮挡窗口可能包含遮挡内容；先 `focus` 再截图。本地控制台桌面已登录解锁且有有效显示输出时不需要 RDP。使用 RDP 会话时，最小化或断开客户端可能导致画面停滞、会话断开或锁定；这些状态需要单独处理。没有物理显示器不阻止 Windows 登录；0.5.0 的 VirtualDrivers 本地控制台路径已在真实拔线与重启后验收，见[无显示器说明](HEADLESS.zh-CN.md)。RDP 断开会话的自动重连不在该功能范围。
- UIA 节点有数量、深度和时间预算。只读观察使用独立 COM 线程，超过约 3 秒返回截图及 UIA 警告，随本次进程退出回收线程；修改控件的 UIA 调用仍可能阻塞，最终由整次请求超时回收 worker，动作结果可能未知。不会自动重放动作。
- SSH 断线不代表已提交动作没执行，已接收的批次可能继续到超时。超时/断线后先观察状态，再决定是否重试。输入完成仅表示 API 接受或事件已入队，需要检查应用结果。
- 当前接收/部署附件验证的是 Windows + PowerShell 7。POSIX 控制端可使用同一协议，但本项目尚未提供并验收无额外依赖的 POSIX 接收脚本。

## 开发与验证

开发机使用 MSVC 2019、Windows SDK 10.0.19041、CMake 3.15+，C++17，`/MT` 静态运行库。编译器只在开发机需要。

```pwsh
& ./scripts/Build.ps1
& ./scripts/Build.ps1 -Architecture Win32
& ./tests/Remote-Smoke.ps1 -Computer win-test
& ./tests/Remote-Smoke.ps1 -Computer win-test -FixtureExecutable ./build/Win32/Release/ra_gui_fixture.exe
& ./tests/Remote-Elevation.ps1 -Computer win-test
& ./scripts/Package.ps1
```

远端 smoke 测试上传独立测试窗口，验证 UIA、鼠标、快捷键、Unicode、截图、失效目标、批次中断和中文文件下载，然后关闭本次窗口。`tests/askpass.cpp` 是开发测试专用的原生 SSH_ASKPASS 适配器，只读取测试进程环境中的口令；**不打入发布包，也不是控制端使用前提**。

验收详情和未验证项目见 [docs/TEST_RESULTS.md](TEST_RESULTS.zh-CN.md)。原方案保存在 [docs/IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.zh-CN.md)。第三方源码及许可证见 [third_party/README.md](../third_party/README.md)。
