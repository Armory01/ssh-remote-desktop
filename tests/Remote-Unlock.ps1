[CmdletBinding()]
param([Parameter(Mandatory)][string]$Computer,[Parameter(Mandatory)][securestring]$Password,[string[]]$SshOption=@(),[string]$RemotePath='.\RemoteAgent\ra.exe',[switch]$DisplayOff)
# This acceptance test intentionally locks and unlocks the supplied user's
# existing desktop. It never guesses credentials or tests an incorrect password.
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$receiver=Join-Path $root 'skills/remote-windows/scripts/Receive-Ra.ps1'
$out=Join-Path $root '.test-output'
[IO.Directory]::CreateDirectory($out)|Out-Null
$checks=[Collections.Generic.List[string]]::new()
function Expect($Condition,[string]$Name){if(!$Condition){throw "FAILED: $Name"};$checks.Add($Name)}
function Call([string]$Op,[hashtable]$Params=@{}){
    $request=@{op=$Op;timeout_ms=30000;params=$Params}|ConvertTo-Json -Depth 20 -Compress
    try{return (& $receiver -Computer $Computer -RemotePath $RemotePath -Request $request -SshOption $SshOption -OutputDirectory $out|ConvertFrom-Json -AsHashtable)}
    finally{$request=$null;if($Params.ContainsKey('password')){$Params.password=$null}}
}
function Good($Reply,[string]$Name){if(!$Reply.ok){$Reply|ConvertTo-Json -Depth 18|Set-Content (Join-Path $out 'unlock-failure.json')};Expect $Reply.ok "$Name ($($Reply.error.message))";return $Reply}
$initial=Good (Call desktop_probe @{screenshot=$false;uia=$false}) 'SYSTEM desktop probe'
Expect ($initial.worker.integrity_level -eq 'system') 'actual SYSTEM worker token'
Expect ($initial.result.wts.locked -eq $false) 'desktop initially unlocked'
$profile=((& ssh.exe -T @SshOption $Computer 'cmd.exe /d /c echo %USERPROFILE%') -join "`n").Trim()
if($LASTEXITCODE){throw 'Remote profile probe failed'}
$name='ra-unlock-'+[guid]::NewGuid().ToString('N')+'.exe'
& scp @SshOption (Join-Path $root 'build/x64/Release/ra_gui_fixture.exe') ($Computer+':RemoteAgent/'+$name)
if($LASTEXITCODE){throw 'Lock fixture upload failed'}
try {
    $mode=if($DisplayOff){'--lock-display-off'}else{'--lock'}
    $launched=Good (Call launch @{exe="$profile\RemoteAgent\$name";args=@("$profile\RemoteAgent\unlock-lock-result.json",$mode)}) 'lock fixture launch'
    $locked=$null
    for($i=0;$i -lt 10;$i++){
        $locked=Good (Call desktop_probe @{screenshot=$false;uia=$false}) 'probe after lock'
        if($locked.result.wts.locked -eq $true){break};Start-Sleep -Milliseconds 300
    }
    Expect ($locked.result.wts.locked -eq $true) 'Windows confirms locked session'
    if($DisplayOff){Start-Sleep -Seconds 3}
    $blocked=Call windows
    Expect (!$blocked.ok -and $blocked.error.code -eq 'DESKTOP_LOCKED') 'ordinary worker refuses a locked desktop'
    $address=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
    try{$result=Good (Call unlock @{password=[Runtime.InteropServices.Marshal]::PtrToStringBSTR($address)}) 'password unlock'}
    finally{[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($address)}
    Expect ($result.result.unlocked -and $result.result.password_submitted -and !$result.result.already_unlocked) 'one password submission actually unlocks desktop'
    $result|ConvertTo-Json -Depth 12|Set-Content (Join-Path $out 'unlock-result.json')
    $after=Good (Call desktop_probe @{format='jpeg'}) 'post-unlock screenshot and state'
    Expect ($after.result.wts.locked -eq $false -and $after.result.input_desktop -eq 'Default' -and $after.image_bytes -gt 0) 'Windows state and desktop confirm unlock'
    $normal=Good (Call windows) 'normal desktop operation after unlock'
    Expect (!$normal.worker.elevated -and $normal.worker.integrity_level -eq 'medium') 'ordinary worker remains medium after SYSTEM request'
    $again=Good (Call unlock) 'already-unlocked no-op'
    Expect ($again.result.already_unlocked -and !$again.result.password_submitted) 'no credentials submitted to an unlocked desktop'
    $summaryName=if($DisplayOff){'unlock-display-off-summary.json'}else{'unlock-summary.json'}
    $reportedVersion=(Call status).result.version
    @{computer=$Computer;version=$reportedVersion;display_off_requested=[bool]$DisplayOff;checks=$checks;worker=$result.worker;session_id=$result.result.session_id;image=$after.image_path}|ConvertTo-Json -Depth 10|Tee-Object (Join-Path $out $summaryName)
} finally {
    # The lock fixture exits after five seconds; delete only our own test file.
    Start-Sleep -Seconds 1
    & ssh.exe -T @SshOption $Computer ('cmd.exe /d /c del /q "'+$profile+'\RemoteAgent\'+$name+'"')|Out-Null
}
