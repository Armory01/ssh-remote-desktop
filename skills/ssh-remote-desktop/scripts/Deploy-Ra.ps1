[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Computer,
    [Parameter(Mandatory)][string]$Executable,
    [string[]]$SshOption = @(),
    [switch]$SkipPowerShell,
    [string]$PowerShellMsiPath,
    [string]$PowerShellCacheDirectory
)
$ErrorActionPreference = 'Stop'
if ($Computer.StartsWith('-')) { throw 'Computer must be an SSH host alias or user@host.' }
$Executable = (Resolve-Path -LiteralPath $Executable).Path
$expected = (Get-FileHash -LiteralPath $Executable -Algorithm SHA256).Hash
# Check the supplied release checksum before calling this script. This hash verifies
# transport and reuse; it is not a substitute for trusting the release source.
$ssh = (Get-Command ssh -ErrorAction Stop).Source
$scp = (Get-Command scp -ErrorAction Stop).Source
$options = @('-o','ConnectTimeout=10') + $SshOption
function Remote([string]$Command) {
    $lines = & $ssh -T @options $Computer $Command
    if ($LASTEXITCODE) { throw "Remote command failed ($LASTEXITCODE): $Command" }
    return ($lines -join "`n")
}
function Complete-Deployment([hashtable]$Result) {
    if(!$SkipPowerShell){
        $arguments=@{Computer=$Computer;SshOption=$SshOption}
        if($PowerShellMsiPath){$arguments.MsiPath=$PowerShellMsiPath}
        if($PowerShellCacheDirectory){$arguments.CacheDirectory=$PowerShellCacheDirectory}
        try{$Result.powershell=(& (Join-Path $PSScriptRoot 'Install-Pwsh.ps1') @arguments|ConvertFrom-Json -AsHashtable)}
        catch{throw "Helper deployment completed, but PowerShell setup failed: $($_.Exception.Message)"}
    }else{$Result.powershell=@{skipped=$true;reason='SkipPowerShell requested'}}
    $Result|ConvertTo-Json -Depth 14 -Compress
}
$probe = Remote 'cmd.exe /d /s /c "(if not exist RemoteAgent mkdir RemoteAgent) & echo ARCH=%PROCESSOR_ARCHITECTURE% & (if exist RemoteAgent\ra.exe certutil -hashfile RemoteAgent\ra.exe SHA256)"'
$bytes = [IO.File]::ReadAllBytes($Executable)
if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw 'Executable is not a PE file.' }
$pe = [BitConverter]::ToInt32($bytes, 60)
if ($pe -lt 64 -or $pe + 6 -gt $bytes.Length) { throw 'Invalid PE header.' }
$machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
if ($machine -notin @(0x14c, 0x8664)) { throw 'Only Windows x86/x64 releases are supported.' }
if ($machine -eq 0x8664 -and $probe -notmatch 'ARCH=AMD64') { throw 'The x64 helper requires an AMD64 remote system.' }
if ($probe -match '(?im)^\s*([0-9a-f]{64})\s*$' -and $Matches[1] -ieq $expected) {
    $state = Remote 'cmd.exe /d /s /c "".\RemoteAgent\ra.exe" status"' | ConvertFrom-Json -AsHashtable
    if (!$state.ok -or $state.protocol -ne 1) { throw 'Existing helper failed its protocol check.' }
    Complete-Deployment @{ computer=$Computer; changed=$false; remote_path='.\RemoteAgent\ra.exe'; sha256=$expected; status=$state.result }
    return
}
$staged = 'ra-' + [guid]::NewGuid().ToString('N') + '.upload.exe'
& $scp @options $Executable ($Computer + ':RemoteAgent/' + $staged)
if ($LASTEXITCODE) { throw 'Helper upload failed.' }
try {
    $hash = Remote ('cmd.exe /d /c certutil -hashfile RemoteAgent\' + $staged + ' SHA256')
    if ($hash -notmatch '(?im)^\s*([0-9a-f]{64})\s*$' -or $Matches[1] -ine $expected) { throw 'Uploaded helper checksum mismatch; current helper was not replaced.' }
    $state = Remote ('cmd.exe /d /s /c "".\RemoteAgent\' + $staged + '" status"') | ConvertFrom-Json -AsHashtable
    if (!$state.ok -or $state.protocol -ne 1) { throw 'Uploaded helper failed its protocol check; current helper was not replaced.' }
    $null = Remote ('cmd.exe /d /c move /y RemoteAgent\' + $staged + ' RemoteAgent\ra.exe')
    $deployment=@{ computer=$Computer; changed=$true; remote_path='.\RemoteAgent\ra.exe'; sha256=$expected; status=$state.result }
} finally {
    # The unique staged file belongs to this deployment only.
    try { $null = Remote ('cmd.exe /d /c "if exist RemoteAgent\' + $staged + ' del /q RemoteAgent\' + $staged + '"') }
    catch { Write-Warning "Could not remove this deployment's staging file RemoteAgent/$staged. $($_.Exception.Message)" }
}
Complete-Deployment $deployment
