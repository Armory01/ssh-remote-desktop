# Headless Windows over SSH

**English** | [简体中文](HEADLESS.zh-CN.md) · [README](../README.md)

Version 0.5.0 adds optional VirtualDrivers support for **Windows 11 x64**.
Use the x64 helper and an already elevated administrator SSH account. The user
only enables SSH; the agent can download, verify and deploy the display driver.
Normal GUI operations still need the same account logged in and unlocked.

## What the agent does

Ask: “Use ssh-remote-desktop to prepare this Windows computer for operation
without a monitor, then sign in and test the application.”

The agent reads `display status`. If an existing physical, VM or virtual display
can be captured, `display ensure` reuses it. A locked session, black application
or capture error alone does not justify installing a driver. When display paths
are absent, the supplied script obtains the pinned driver-only ZIP, caches it on
the controller, uploads four files and invokes the same `ra.exe` installer.

For a newly authorized installation, the script's `-TrustPublisher` option
permits adding the **specific pinned SignPath leaf certificate** to
`LocalMachine/TrustedPublisher`. Authenticode and fixed SHA256 checks run first.
It does not add root certificates or alter Secure Boot, test signing, memory
integrity or signature enforcement. If Windows rejects the driver, inspect the
reported error and `C:\Windows\INF\setupapi.dev.log`.

No control application, Nefcon, custom service, logon task or extra listening
port is installed. Windows loads the optional UMDF display driver in its own
WUDFHost process while enabled. The helper remains one executable and its
workers exit after each call; **the display driver is a persistent OS component**.

## Advanced commands

```pwsh
& ./skills/ssh-remote-desktop/scripts/Receive-Ra.ps1 -Computer win-test -Request '{"op":"display"}'
& ./skills/ssh-remote-desktop/scripts/Ensure-Display.ps1 -Computer win-test -TrustPublisher
```

No driver download happens if an existing output is usable. For a deliberate
VirtualDrivers-only headless test, add `-VirtualOnly`; this changes and persists
the active display topology, leaving other adapters installed. `-ForceVirtual`
instead activates the virtual monitor alongside existing active paths. These
switches are explicit choices, not defaults. Repeated calls reuse one device.

`-DriverZipPath <file>` supplies the exact pinned offline ZIP. `-CacheDirectory`
overrides `%LOCALAPPDATA%\SSHRemoteDesktop\drivers`. The helper copies files to a
protected temporary directory and checks all four hashes again before use.
Per-call uploads and staging are removed. No remote PowerShell is required for
this flow. `-RemotePath` and `-SshOption` work like the other controller scripts.

The initial configuration creates **one 1920×1080, 60 Hz SDR monitor** in
`C:\VirtualDisplayDriver\vdd_settings.xml`. An existing configuration is retained.
The helper records its created instance in
`HKLM\SOFTWARE\SSHRemoteDesktop\VirtualDisplay`. It does not remove or replace
other vendors' display devices. An already installed healthy MttVDD can be
reused; an unhealthy unowned device requires inspection instead of replacement.

```json
{"op":"display","params":{"action":"status","screenshot":true}}
{"op":"display","params":{"action":"disable"}}
{"op":"display","params":{"action":"remove"}}
```

Disable/remove only affect this project's recorded device. `ensure` can re-enable
its disabled device without downloading a package. Removal keeps the driver
store package, configuration and publisher trust; it does not revoke a shared
publisher certificate. Active topology changes may affect the desktop layout.

## Driver source and limits

- [VirtualDrivers / Virtual-Display-Driver](https://github.com/VirtualDrivers/Virtual-Display-Driver), MIT.
- Pinned [25.7.23 driver-only release](https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/tag/25.7.23).
  The release is titled Beta; its ZIP filename contains `x86`, but its actual
  DLL and INF are **AMD64/x64**. This is not a 32-bit Windows package.
- ZIP SHA256: `e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a`.
- Signer leaf SHA1: `3CF8CF26D8BA266C3A483AB7D26D4A818E317D76`.

The driver is downloaded separately, not embedded in our release. Other Windows
versions, ARM64 and native 32-bit targets are outside this feature's scope.
See [actual tests and security-policy limits](TEST_RESULTS.md); a successful
test does not establish compatibility with every firmware or driver policy.

A virtual monitor supplies a display surface, not a Windows login or an encoder
for every rendering API. Use the existing `login`/`unlock` flow separately when
needed. GDI capture limitations for protected content and some GPU applications
still apply. No RDP client is needed for the local console path.
