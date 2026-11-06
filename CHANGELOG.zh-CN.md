# 更新记录

[English](CHANGELOG.md) | **简体中文**

## 0.4.2

- Skill 名称与源码目录统一改为 `ssh-remote-desktop`，升级时替换旧的 `remote-windows` Skill 目录。
- 中英文 README 前部增加任务用法：按电脑名称选择目标，上传测试包、启动并配置程序、
  查看截图、取回日志；下一句可以直接切换另一台电脑。
- 同步安装提示词、脚本引用、测试和打包路径。协议 1、`ra.exe`、远端路径及桌面行为保持不变。

## 0.4.1

- 项目和仓库改名为 SSH Remote Desktop（`ssh-remote-desktop`）。
- 文档默认英文，提供中文 README、SSH 开通指南、进阶用法、架构和验收说明。
- 安装入口改为可复制给 Codex、Claude Code 等 Agent 的提示词，底层命令移至进阶文档。
- 补充微软官方 OpenSSH 安装步骤与离线下载入口。
- 协议 1、`remote-windows` Skill、`ra.exe` 及已有远端路径保持兼容，桌面行为与 0.4.0 相同。

## 0.4.0

- 实验性支持 Windows 11 同一 SSH 本地账户在开机后或注销后的控制台密码登录。
- 状态增加登录前会话、控制台会话 ID、系统信息和运行时间。
- 首次部署自动检查远端 PowerShell 7，缺少时通过控制端缓存的官方 MSI 全局安装。
- 提供 x64/x86 helper、SHA256、完整 Skill、MIT 许可证和双架构构建 CI。

登录目前验证的是已选账户的标准密码界面，复杂账户选择、PIN/Hello/MFA、
强制 Ctrl+Alt+Del 和新用户引导不在已验证范围。

## 0.3.0

- 使用临时 SYSTEM worker 实验性支持已有桌面的密码解锁与安全桌面观察。
- 核对 Windows 实际锁定状态，并为当前请求唤醒显示输出。

## 0.2.0

- 已具备管理员权限的 SSH 调用端可以显式请求按需高权限 worker。
- 返回 worker 与已启动程序的真实权限信息。

## 0.1.0

- 初版 C++ 单文件 helper：SSH 部署、窗口枚举、截图、UIA、鼠标键盘和中文输入、
  GUI 启动、动作批处理。
- 结果 JSON 和原始图片字节通过同一次 SSH 响应返回。
