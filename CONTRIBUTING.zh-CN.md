# 参与贡献

[English](CONTRIBUTING.md) | **简体中文**

欢迎提交 Issue 和 PR。报告问题时提供 helper 版本、Windows 构建号、控制端 Shell
版本、请求操作和错误码；日志或截图中请去掉凭据、私人地址和无关桌面内容。

## 构建与检查

使用 Windows、MSVC 2019+、Windows SDK、CMake 3.15+ 和 PowerShell 7。
随仓库构建脚本指定 Visual Studio 2019 和 SDK 10.0.19041：

```pwsh
& ./scripts/Build.ps1
& ./scripts/Build.ps1 -Architecture Win32
& ./scripts/Package.ps1
```

使用其他已安装的 Visual Studio 或 SDK 时，可以直接调用 CMake：

```pwsh
cmake -S . -B build/x64 -A x64
cmake --build build/x64 --config Release --parallel
ctest --test-dir build/x64 -C Release --output-on-failure
```

尽量保持协议 1 的兼容性。请求／响应变化写入
`skills/ssh-remote-desktop/references/protocol.md`，支持范围变化写入 README。
验证与修改有关的实际行为；编译成功不能单独证明支持其他 Windows 版本。

## 远程验收

`Remote-Smoke.ps1` 和 `Remote-Elevation.ps1` 通过 SSH 操作真实测试窗口。
`Remote-Unlock.ps1` 会锁定桌面，`Remote-Login.ps1` 会注销或重启目标电脑，
只在确实需要这些操作的自有测试机运行。密码通过 SecureString 参数和内存传输，
不要写入脚本或测试数据。

已验证环境见[验收记录](docs/TEST_RESULTS.zh-CN.md)。构建产物、原始测试结果、
本地凭据和无关排障资料不提交到源码仓库或发布包。
