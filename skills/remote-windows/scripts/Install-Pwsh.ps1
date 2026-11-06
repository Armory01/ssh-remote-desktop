[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Computer,
    [string[]]$SshOption=@(),
    [string]$MsiPath,
    [string]$CacheDirectory=(Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'RemoteAgent/cache/powershell')
)
# Optional system-wide tool installation; ra.exe itself does not require pwsh.
# Pinned official x64 MSI, authenticated by SHA256 and Microsoft Authenticode.
$ErrorActionPreference='Stop'
$version='7.6.9'
$expected='50d33c7494aaf82b987cfd4de3f232d04f91fdd4c0deeca8a669cced3e7cf8f9'
if($Computer.StartsWith('-')){throw 'Computer must be an SSH alias or user@host.'}
function Invoke-RaSsh([string]$Command){
    $text=& ssh.exe -T -o ConnectTimeout=10 @SshOption $Computer $Command
    if($LASTEXITCODE){throw "SSH command failed (exit $LASTEXITCODE)."}
    return ($text -join "`n").Trim()
}
function Assert-Msi([string]$Path){
    if((Get-FileHash -LiteralPath $Path).Hash.ToLowerInvariant() -ne $expected){throw 'Official PowerShell MSI SHA256 mismatch.'}
    $signature=Get-AuthenticodeSignature -LiteralPath $Path
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation(?:,|$)'){throw 'PowerShell MSI must have a valid Microsoft signature.'}
}
$probe=Invoke-RaSsh 'cmd.exe /d /c "echo RA_PF=%ProgramFiles% & echo RA_PF64=%ProgramW6432% & echo RA_ARCH=%PROCESSOR_ARCHITECTURE% & echo RA_WOW=%PROCESSOR_ARCHITEW6432% & ver"'
$remoteProgramFiles=if($probe -match '(?m)^RA_PF64=([^%\r\n]+)'){$Matches[1].Trim()}elseif($probe -match '(?m)^RA_PF=([^%\r\n]+)'){$Matches[1].Trim()}else{throw 'Cannot locate remote Program Files.'}
if($remoteProgramFiles -match '["%&|<>^!\r\n]'){throw 'Unsupported remote Program Files path.'}
$pwshPath=$remoteProgramFiles+'\PowerShell\7\pwsh.exe'
$versionCommand='cmd.exe /d /s /c ""'+$pwshPath+'" -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.ToString()""'
$found=& ssh.exe -T -o ConnectTimeout=10 @SshOption $Computer $versionCommand 2>$null
if($LASTEXITCODE -eq 0 -and (($found -join '').Trim() -match '^7\.\d+\.\d+$')){
    @{changed=$false;computer=$Computer;path=$pwshPath;version=($found -join '').Trim();cache_hit=$null}|ConvertTo-Json -Compress
    return
}
if($probe -notmatch 'RA_(?:ARCH|WOW)=AMD64' -or $probe -notmatch '\b10\.0\.\d+\.\d+'){
    @{changed=$false;computer=$Computer;skipped=$true;reason='Automatic PowerShell setup supports x64 Windows 10.0 only; native ra.exe remains usable.'}|ConvertTo-Json -Compress
    return
}
$folder=Join-Path ([IO.Path]::GetFullPath($CacheDirectory)) "$version/win-x64"
$cacheFile=Join-Path $folder "PowerShell-$version-win-x64.msi"
[IO.Directory]::CreateDirectory($folder)|Out-Null
$cacheHit=$false
if(Test-Path -LiteralPath $cacheFile){
    try{Assert-Msi $cacheFile;$cacheHit=$true}
    catch{Write-Warning 'Cached PowerShell MSI failed validation; replacing this cache entry.';Remove-Item -LiteralPath $cacheFile -Force}
}
if(!$cacheHit){
    $staging=Join-Path $folder ([guid]::NewGuid().ToString('N')+'.download.msi')
    try{
        if($MsiPath){Copy-Item -LiteralPath (Resolve-Path -LiteralPath $MsiPath).Path -Destination $staging}
        else{Invoke-WebRequest "https://github.com/PowerShell/PowerShell/releases/download/v$version/PowerShell-$version-win-x64.msi" -OutFile $staging}
        Assert-Msi $staging
        Move-Item -LiteralPath $staging -Destination $cacheFile -Force
    }finally{if(Test-Path -LiteralPath $staging){Remove-Item -LiteralPath $staging -Force}}
}
Assert-Msi $cacheFile
$remoteProfile=Invoke-RaSsh 'cmd.exe /d /c echo %USERPROFILE%'
if($remoteProfile -match '["%&|<>^!\r\n]'){throw 'Unsupported remote profile path.'}
$name='ra-pwsh-'+[guid]::NewGuid().ToString('N')+'.msi'
$remote=$remoteProfile+'\RemoteAgent\'+$name
$batchName=[IO.Path]::GetFileNameWithoutExtension($name)+'.cmd'
$logName=[IO.Path]::GetFileNameWithoutExtension($name)+'.log'
$localBatch=Join-Path $folder $batchName
$body=@'
@echo off
start "" /wait "%SystemRoot%\System32\msiexec.exe" /i "%~dp0__MSI__" /qn /norestart ADD_PATH=1 ENABLE_PSREMOTING=0 REGISTER_MANIFEST=1 USE_MU=1 ENABLE_MU=1 /L*v "%~dp0__LOG__"
set "ra_code=%errorlevel%"
echo RA_MSI_EXIT=%ra_code%
exit /b 0
'@
try {
    [IO.File]::WriteAllText($localBatch,$body.Replace('__MSI__',$name).Replace('__LOG__',$logName),[Text.Encoding]::ASCII)
    Invoke-RaSsh 'cmd.exe /d /c "if not exist RemoteAgent mkdir RemoteAgent"'|Out-Null
    & scp.exe @SshOption $cacheFile ($Computer+':RemoteAgent/'+$name)
    if($LASTEXITCODE){throw 'PowerShell MSI transfer failed.'}
    $hash=Invoke-RaSsh ('certutil -hashfile "'+$remote+'" SHA256')
    if(($hash -replace '\s','') -notmatch [regex]::Escape($expected)){throw 'Remote MSI SHA256 mismatch.'}
    & scp.exe @SshOption $localBatch ($Computer+':RemoteAgent/'+$batchName)
    if($LASTEXITCODE){throw 'PowerShell MSI bootstrap upload failed.'}
    $installed=Invoke-RaSsh ('cmd.exe /d /c RemoteAgent\'+$batchName)
    if($installed -notmatch 'RA_MSI_EXIT=(0|3010)(?:\s|$)'){throw "PowerShell system-wide MSI installation failed: $installed. Log: $remoteProfile\RemoteAgent\$logName"}
    $actual=Invoke-RaSsh $versionCommand
    if($actual -ne $version){throw 'PowerShell post-install version check failed.'}
    @{changed=$true;computer=$Computer;path=$pwshPath;version=$actual;reboot_required=($installed -match 'RA_MSI_EXIT=3010');cache_hit=$cacheHit;cached_msi_path=$cacheFile}|ConvertTo-Json -Compress
} finally {
    if(Test-Path -LiteralPath $localBatch){Remove-Item -LiteralPath $localBatch}
    try{Invoke-RaSsh ('cmd.exe /d /c del /q RemoteAgent\'+$name+' RemoteAgent\'+$batchName)|Out-Null}
    catch{Write-Warning "Could not remove this deployment's MSI staging file: $remote"}
}
