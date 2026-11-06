# Contributing

**English** | [简体中文](CONTRIBUTING.zh-CN.md)

Issues and pull requests are welcome. Include the helper version, Windows build,
controller shell version, request operation and error code when reporting a bug.
Remove credentials, private addresses and unrelated desktop content from logs
and screenshots.

## Build and check

Use Windows, MSVC 2019 or newer, a Windows SDK, CMake 3.15+ and PowerShell 7.
The bundled build script selects Visual Studio 2019 and SDK 10.0.19041:

```pwsh
& ./scripts/Build.ps1
& ./scripts/Build.ps1 -Architecture Win32
& ./scripts/Package.ps1
```

For another installed Visual Studio version or SDK, use CMake directly:

```pwsh
cmake -S . -B build/x64 -A x64
cmake --build build/x64 --config Release --parallel
ctest --test-dir build/x64 -C Release --output-on-failure
```

Keep protocol 1 compatible where possible. Document request/response changes in
`skills/remote-windows/references/protocol.md` and changes in supported behavior
in the README. Test observable behavior relevant to a change; a successful build
alone does not establish support for another Windows release.

## Remote tests

`tests/Remote-Smoke.ps1` and `tests/Remote-Elevation.ps1` operate real GUI fixture
windows over SSH. Run them on a test machine you control. `Remote-Unlock.ps1`
locks the desktop; `Remote-Login.ps1` logs out or reboots the target. Run those
only when that disruption is intended. Credentials are supplied in memory using
the documented SecureString parameters, never committed in scripts or fixtures.

See [test results](docs/TEST_RESULTS.md) for the validated environments. Build
outputs, raw test results, local credentials and unrelated evidence stay outside
the source repository and release archive.
