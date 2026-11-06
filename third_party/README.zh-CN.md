# 编译依赖

[English](README.md) | **简体中文**

`nlohmann/json.hpp` 为 nlohmann/json v3.11.3 的单头文件，使用 MIT 许可证，编译进 EXE，不要求目标机器安装依赖。

- 来源：https://github.com/nlohmann/json/tree/v3.11.3
- SHA256：`9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6`
- 许可证：源码目录中的 `nlohmann/LICENSE.MIT`；发布包中的 `licenses/nlohmann-json.txt`

## 可选下载的显示驱动

[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
采用 MIT 许可证。无显示器部署单独下载固定的 25.7.23 driver-only ZIP；本项目源码和
发布包不嵌入该驱动，也不包含 Nefcon 或上游控制面板。helper 直接调用 Windows SetupAPI。
哈希、发布者信任和部署细节见 [HEADLESS.zh-CN.md](../docs/HEADLESS.zh-CN.md)。
