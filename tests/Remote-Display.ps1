[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Computer,
    [string[]]$SshOption=@(),
    [string]$RemotePath='.\RemoteAgent\ra.exe',
    [string]$DriverZipPath,
    [switch]$Reinstall
)
# Opt-in machine-wide test. Changes the console to virtual-only; -Reinstall
# also removes this project's owned device, then tests the deployment path.
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$receiver=Join-Path $root 'skills/ssh-remote-desktop/scripts/Receive-Ra.ps1'
$out=Join-Path $root '.test-output'
[IO.Directory]::CreateDirectory($out)|Out-Null
$checks=[Collections.Generic.List[string]]::new()
function Expect($Condition,[string]$Name){if(!$Condition){throw "FAILED: $Name"};$checks.Add($Name)}
function Call([hashtable]$Params){& $receiver -Computer $Computer -SshOption $SshOption -RemotePath $RemotePath -OutputDirectory $out -Request (@{op='display';params=$Params}|ConvertTo-Json -Compress)|ConvertFrom-Json -AsHashtable}
function Good($Reply,[string]$Name){Expect $Reply.ok "$Name ($($Reply.error.message))";return $Reply}
$initial=Good (Call @{action='status'}) 'console display diagnostics'
if($Reinstall){
    if(@($initial.result.devices|Where-Object managed).Count){$null=Good (Call @{action='remove'}) 'remove owned virtual device'}
    $missing=Call @{action='ensure';force_virtual=$true}
    Expect (!$missing.ok -and $missing.error.code -eq 'DRIVER_PACKAGE_REQUIRED') 'missing package is reported before installation'
    $run='vdd-bad-'+[guid]::NewGuid().ToString('N')
    $badFile=Join-Path $out ($run+'.dll')
    $profilePath=(& ssh -T @SshOption $Computer 'cmd.exe /d /c echo %USERPROFILE%') -join ''
    if($LASTEXITCODE -or $profilePath -match '["%&|<>^!\r\n]'){throw 'Invalid SSH profile response'}
    $remoteFolder=$profilePath.Trim()+'\RemoteAgent\'+$run
    try {
        [IO.File]::WriteAllText($badFile,'deliberately invalid pinned driver')
        & ssh -T @SshOption $Computer ('cmd.exe /d /c mkdir "'+$remoteFolder+'"')
        if($LASTEXITCODE){throw 'Bad-package fixture mkdir failed'}
        & scp @SshOption $badFile ($Computer+':RemoteAgent/'+$run+'/MttVDD.dll')
        if($LASTEXITCODE){throw 'Bad-package fixture upload failed'}
        $bad=Call @{action='ensure';force_virtual=$true;trust_publisher=$true;package_path=$remoteFolder}
        Expect (!$bad.ok -and $bad.error.code -eq 'DRIVER_HASH_MISMATCH') 'tampered package rejected before device creation'
        $untouched=Good (Call @{action='status'}) 'status after rejected package'
        Expect ($untouched.result.devices.Count -eq 0) 'rejected package leaves no virtual device'
    } finally {
        & ssh -T @SshOption $Computer ('cmd.exe /d /c del /q "'+$remoteFolder+'\MttVDD.dll"')|Out-Null
        & ssh -T @SshOption $Computer ('cmd.exe /d /c rd "'+$remoteFolder+'"')|Out-Null
        if(Test-Path -LiteralPath $badFile){Remove-Item -LiteralPath $badFile}
    }
}
$arguments=@{Computer=$Computer;SshOption=$SshOption;RemotePath=$RemotePath;ForceVirtual=$true;VirtualOnly=$true;TrustPublisher=$true}
if($DriverZipPath){$arguments.DriverZipPath=$DriverZipPath}
$ensured=Good (& (Join-Path $root 'skills/ssh-remote-desktop/scripts/Ensure-Display.ps1') @arguments|ConvertFrom-Json -AsHashtable) 'cached package deployment and activation'
Expect ($ensured.result.devices.Count -eq 1 -and $ensured.result.devices[0].started) 'one healthy VirtualDrivers device'
Expect ($ensured.result.virtual_active -and $ensured.result.active_path_count -eq 1 -and $ensured.result.active_paths[0].virtualdrivers_vdd) 'only VirtualDrivers provides the active output'
Expect ($ensured.result.capture_ok -and $ensured.result.desktop_ready) 'headless desktop is capturable and unlocked'
$capture=Good (Call @{action='status';screenshot=$true}) 'console screenshot'
Expect ($capture.image_bytes -gt 0 -and (Test-Path -LiteralPath $capture.image_path)) 'image returned in the same SSH response'
$repeat=Good (Call @{action='ensure';virtual_only=$true}) 'repeat ensure'
Expect (!$repeat.result.changed -and $repeat.result.devices.Count -eq 1) 'repeat ensure creates no duplicate monitor'
$reuse=Good (Call @{action='ensure'}) 'ordinary ensure reuses usable output'
Expect (!$reuse.result.changed -and $reuse.result.reused_existing_display) 'no reinstall or topology change for a usable display'
if($ensured.result.devices[0].managed){
    $null=Good (Call @{action='disable'}) 'disable owned device'
    $enabled=Good (Call @{action='ensure';virtual_only=$true}) 're-enable owned device without package'
    Expect ($enabled.result.devices.Count -eq 1 -and $enabled.result.devices[0].started -and $enabled.result.virtual_active -and $enabled.result.capture_ok) 'disabled device recovers without duplicate installation'
}
@{computer=$Computer;checks=$checks;display=$ensured.result;image_path=$capture.image_path}|ConvertTo-Json -Depth 25|Tee-Object (Join-Path $out 'display-summary.json')
