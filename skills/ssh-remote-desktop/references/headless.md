# Optional headless display (0.5.0+)

Only Windows 11 x64 with the x64 helper and an already high-integrity SSH caller
is supported for driver setup. Use `Receive-Ra.ps1` with `{"op":"display"}` first.
This uses the existing temporary SYSTEM console worker, including before login.
Inspect `usable_display`, `capture_ok`, `active_paths`, `devices`, `virtual_active`
and `desktop_ready`; a usable display does not imply a logged-in unlocked account.

If an existing output is usable, reuse it. Do not install a driver merely for a
locked desktop, a black application, or failed capture with existing active paths.
When output is absent and headless setup is authorized:

```pwsh
& '<skill>/scripts/Ensure-Display.ps1' -Computer win-test -TrustPublisher
```

An authorized request to prepare a machine for headless operation covers this
pinned driver installation and its leaf publisher trust; reuse authorization
already given. Explain that the optional OS display driver remains loaded by
Windows; no custom helper service/control app is added. Do not change Secure
Boot, memory integrity, test signing or signature enforcement to make it work.

The script probes before downloading. It caches the pinned 25.7.23 driver-only
ZIP, verifies SHA256, extracts and uploads four files. The helper stages them
with protected ACLs, rechecks fixed hashes and Authenticode, then uses SetupAPI
to create one Root\MttVDD device. `-TrustPublisher` permits only the pinned
SignPath leaf certificate in LocalMachine/TrustedPublisher, never Root.
The ZIP filename says x86 but contains the selected AMD64/x64 driver.

Options: `-DriverZipPath` supplies the exact pinned offline ZIP; `-CacheDirectory`
overrides the controller cache; `-SshOption` and `-RemotePath` work as usual.
`-ForceVirtual` deliberately activates a virtual monitor alongside existing
paths. `-VirtualOnly` deliberately persists only the virtual path, affecting the
console layout. Use those only when requested or needed for an explicit test.
The ordinary no-flag flow preserves a usable existing layout.

Created configuration: C:\VirtualDisplayDriver\vdd_settings.xml, one 1080p60 SDR
monitor; an existing file is retained. Temporary package uploads are cleaned.
The native helper needs no PowerShell or third-party installer on the target.
Driver setup may require a reboot; report `installation.reboot_required` and
follow the user's existing reboot authority. Never blindly repeat installation
after transport loss: read status and inspect the device first.

`display` actions `disable` and `remove` affect only the recorded device created
by this project. `ensure` can re-enable it without another download. `remove`
keeps the driver store, configuration and publisher trust. Other vendors' virtual
adapters must not be removed. Multiple or unhealthy unowned MttVDD instances need
inspection, not duplicate installation. A driver rejection includes the Windows
error and setupapi.dev.log path; do not claim headless readiness on failure.

Driver source: https://github.com/VirtualDrivers/Virtual-Display-Driver (MIT).
ZIP SHA256: e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a.
Signer SHA1: 3CF8CF26D8BA266C3A483AB7D26D4A818E317D76.
