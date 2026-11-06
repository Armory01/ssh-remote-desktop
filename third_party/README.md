# Third-party dependencies

**English** | [简体中文](README.zh-CN.md)

`nlohmann/json.hpp` is the single-header distribution of nlohmann/json v3.11.3.
It is MIT licensed and compiled into the helper, with no remote installation required.

- Source: https://github.com/nlohmann/json/tree/v3.11.3
- SHA256: `9BEA4C8066EF4A1C206B2BE5A36302F8926F7FDC6087AF5D20B417D0CF103EA6`
- License: `nlohmann/LICENSE.MIT` in source; `licenses/nlohmann-json.txt` in releases.

## Optional downloaded display driver

[VirtualDrivers/Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver)
is MIT licensed. Headless setup separately downloads its pinned 25.7.23 driver-only
ZIP; neither that driver nor Nefcon/control applications are embedded in our
source or release. The helper uses Windows SetupAPI directly. Package hashes,
publisher trust and deployment details are documented in [HEADLESS.md](../docs/HEADLESS.md).
