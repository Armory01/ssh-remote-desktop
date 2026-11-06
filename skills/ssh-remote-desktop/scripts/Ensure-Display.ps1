[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Computer,
    [string[]]$SshOption = @(),
    [string]$RemotePath = '.\RemoteAgent\ra.exe',
    [switch]$ForceVirtual,
    [switch]$VirtualOnly,
    [switch]$TrustPublisher,
    [string]$DriverZipPath,
    [string]$CacheDirectory = (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'SSHRemoteDesktop/drivers')
)
$ErrorActionPreference = 'Stop'
$receiver = Join-Path $PSScriptRoot 'Receive-Ra.ps1'
$params = @{action='ensure';force_virtual=[bool]$ForceVirtual;virtual_only=[bool]$VirtualOnly;trust_publisher=[bool]$TrustPublisher}
function Invoke-Ensure {
    & $receiver -Computer $Computer -SshOption $SshOption -RemotePath $RemotePath -Request (@{op='display';params=$params}|ConvertTo-Json -Depth 10 -Compress)
}
$first = Invoke-Ensure | ConvertFrom-Json -AsHashtable
if ($first.ok -or $first.error.code -ne 'DRIVER_PACKAGE_REQUIRED') {
    $first | ConvertTo-Json -Depth 40 -Compress
    return
}
if (!$TrustPublisher) {
    # The native installer also checks this before adding publisher trust.
    throw 'A new VirtualDrivers installation needs -TrustPublisher, authorizing the pinned SignPath leaf certificate in LocalMachine/TrustedPublisher. No root certificate is added.'
}
$expected = 'e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a'
$folder = Join-Path ([IO.Path]::GetFullPath($CacheDirectory)) 'VirtualDrivers/25.7.23'
[IO.Directory]::CreateDirectory($folder) | Out-Null
$cacheFile = Join-Path $folder 'VirtualDisplayDriver-x86.Driver.Only.zip'
function Assert-Zip([string]$Path) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ine $expected) { throw 'VirtualDrivers package SHA256 mismatch.' }
}
$cacheHit = $false
if (Test-Path -LiteralPath $cacheFile) {
    Assert-Zip $cacheFile
    $cacheHit = $true
} else {
    $download = Join-Path $folder ([guid]::NewGuid().ToString('N')+'.download.zip')
    try {
        if ($DriverZipPath) { Copy-Item -LiteralPath (Resolve-Path -LiteralPath $DriverZipPath).Path -Destination $download }
        else { Invoke-WebRequest 'https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip' -OutFile $download }
        Assert-Zip $download
        Move-Item -LiteralPath $download -Destination $cacheFile -Force
    } finally { if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download -Force } }
}
$files = @('mttvdd.cat','MttVDD.dll','MttVDD.inf','vdd_settings.xml')
$run = 'vdd-upload-'+[guid]::NewGuid().ToString('N')
$local = Join-Path $folder $run
$options = @('-o','ConnectTimeout=10')+$SshOption
function Remote([string]$Command) {
    $lines = & ssh -T @options $Computer $Command
    if ($LASTEXITCODE) { throw "Display deployment SSH command failed ($LASTEXITCODE)." }
    return ($lines -join "`n").Trim()
}
$profilePath = Remote 'cmd.exe /d /c echo %USERPROFILE%'
if ($profilePath -match '["%&|<>^!\r\n]' -or $profilePath -notmatch '^[A-Za-z]:\\') { throw 'Unsupported remote profile path.' }
$remoteFolder = $profilePath+'\RemoteAgent\'+$run
try {
    [IO.Directory]::CreateDirectory($local) | Out-Null
    # Extract only the four expected entries from the hash-pinned archive.
    $archive = [IO.Compression.ZipFile]::OpenRead($cacheFile)
    try {
        foreach ($file in $files) {
            $entry = $archive.GetEntry('VirtualDisplayDriver/'+$file)
            if (!$entry) { throw "Driver archive lacks $file." }
            [IO.Compression.ZipFileExtensions]::ExtractToFile($entry,(Join-Path $local $file))
        }
    } finally { $archive.Dispose() }
    $null = Remote ('cmd.exe /d /c mkdir "'+$remoteFolder+'"')
    & scp @options @($files | ForEach-Object { Join-Path $local $_ }) ($Computer+':RemoteAgent/'+$run+'/')
    if ($LASTEXITCODE) { throw 'Driver package upload failed.' }
    $params.package_path = $remoteFolder
    $result = Invoke-Ensure | ConvertFrom-Json -AsHashtable
    if ($result.ok) { $result.result.package_cache_hit=$cacheHit; $result.result.cached_package=$cacheFile }
    $result | ConvertTo-Json -Depth 40 -Compress
} finally {
    try {
        # Fixed names in this call's unique folder; never delete unrelated files.
        $null = Remote ('cmd.exe /d /c del /q '+(($files | ForEach-Object {'"'+$remoteFolder+'\'+$_+'"'}) -join ' '))
        $null = Remote ('cmd.exe /d /c rd "'+$remoteFolder+'"')
    } catch { Write-Warning "Could not clean this call's driver upload: $remoteFolder" }
    foreach ($file in $files) { $path=Join-Path $local $file; if(Test-Path -LiteralPath $path){Remove-Item -LiteralPath $path -Force} }
    if(Test-Path -LiteralPath $local){Remove-Item -LiteralPath $local} # empty directory only
}
