[CmdletBinding()]
param([Parameter(Mandatory)][string]$Computer,[string[]]$SshOption=@(),[string]$FixtureExecutable,[string]$RemotePath='.\RemoteAgent\ra.exe')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$receiver=Join-Path $root 'skills/ssh-remote-desktop/scripts/Receive-Ra.ps1'
$out=Join-Path $root '.test-output'
[IO.Directory]::CreateDirectory($out)|Out-Null
$checks=[Collections.Generic.List[string]]::new()
function Expect($Condition,[string]$Name){if(!$Condition){throw "FAILED: $Name"};$checks.Add($Name)}
function Call([string]$Op,[hashtable]$Params=@{}){
    $request=@{op=$Op;params=$Params}|ConvertTo-Json -Depth 30 -Compress
    $raw=& $receiver -Computer $Computer -RemotePath $RemotePath -Request $request -SshOption $SshOption -OutputDirectory $out
    return ($raw|ConvertFrom-Json -AsHashtable)
}
function Good($Result,[string]$Name){Expect $Result.ok "$Name ($($Result.error.message))";return $Result}
function NativeSsh([string]$Command){$r=& ssh -T @SshOption $Computer $Command;if($LASTEXITCODE){throw "SSH failed: $Command"};return ($r -join "`n").Trim()}
$state=Good (Call status) 'caller status'
Expect ($state.result.privileges.elevated -and $state.result.privileges.integrity_rid -ge 12288) 'SSH already has administrative rights'
$ordinary=Good (Call windows) 'ordinary worker'
Expect (!$ordinary.worker.elevated -and $ordinary.worker.integrity_rid -eq 8192) 'default remains medium integrity under elevated SSH'
$high=Good (Call windows @{elevated=$true}) 'on-demand elevated worker'
Expect ($high.worker.elevated -and $high.worker.integrity_rid -ge 12288) 'worker elevation verified'
$profilePath=NativeSsh 'cmd.exe /d /c echo %USERPROFILE%'
$run=[guid]::NewGuid().ToString('N')
$fixtureName="ra-elevation-$run.exe"
$remoteFile="$profilePath\RemoteAgent\elevation-$run.txt"
if(!$FixtureExecutable){$FixtureExecutable=Join-Path $root 'build/x64/Release/ra_gui_fixture.exe'}
& scp @SshOption $FixtureExecutable ($Computer+':RemoteAgent/'+$fixtureName)
if($LASTEXITCODE){throw 'Fixture upload failed'}
$launched=$null;$window=$null;$failure=$null
try {
    $launched=Good (Call launch @{elevated=$true;exe="$profilePath\RemoteAgent\$fixtureName";args=@($remoteFile)}) 'detached elevated GUI launch'
    Expect ($launched.result.privileges.elevated -and $launched.result.privileges.integrity_rid -ge 12288) 'detached child retains high integrity'
    for($attempt=0;$attempt -lt 10 -and !$window;$attempt++){
        $windows=Good (Call windows @{elevated=$true}) 'elevated window enumeration'
        $window=@($windows.result.windows|Where-Object pid -eq $launched.result.pid)|Select-Object -First 1
        if(!$window){Start-Sleep -Milliseconds 200}
    }
    Expect ($null -ne $window) 'elevated application survives worker and SSH exit'
    Expect ($window.privileges.elevated -and $window.title -eq 'RemoteAgent Test - Elevated') 'actual target token and window title confirm elevation'
    $guard=@{pid=$window.pid;process_start=$window.process_start;bounds=$window.bounds}
    $blocked=Call act @{window=$window.hwnd;expected=$guard;actions=@(@{type='focus'})}
    Expect (!$blocked.ok -and $blocked.error.code -eq 'ELEVATION_REQUIRED' -and !$blocked.worker.elevated) 'ordinary worker rejects higher-integrity target before input'
    $seen=Good (Call observe @{elevated=$true;window=$window.hwnd;expected=$guard}) 'elevated screenshot and UIA'
    Expect ($seen.result.uia.available -and $seen.image_bytes -gt 0) 'high-integrity UIA and PNG available'
    $text='高权限 worker UIA 中文验证'
    $uia=Good (Call act @{elevated=$true;window=$window.hwnd;expected=$guard;actions=@(
        @{type='set_value';selector=@{automation_id='101'};text=$text},
        @{type='invoke';selector=@{automation_id='102'}},@{type='wait';ms=100});observe=$true}) 'high-integrity UIA SetValue and Invoke'
    $edit=@($uia.result.uia.elements|Where-Object automation_id -eq '101')[0].bounds
    $save=@($uia.result.uia.elements|Where-Object automation_id -eq '102')[0].bounds
    $typed='高权限鼠标键盘输入成功 123'
    $physical=Good (Call act @{elevated=$true;window=$window.hwnd;expected=$guard;actions=@(
        @{type='click';x=($edit.x+40);y=($edit.y+30)},@{type='key';keys='Ctrl+Home'},@{type='key';keys='Ctrl+Shift+End'},
        @{type='type';text=$typed},@{type='click';x=($save.x+40);y=($save.y+20)},@{type='wait';ms=200});observe=$true}) 'high-integrity mouse keyboard batch'
    $value=@($physical.result.uia.elements|Where-Object automation_id -eq '101')[0].value
    Expect ($value -ceq $typed) 'actual elevated edit value matches physical input'
    & scp @SshOption ($Computer+':RemoteAgent/elevation-'+$run+'.txt') (Join-Path $out 'elevated-downloaded.txt')
    if($LASTEXITCODE){throw 'Elevated output download failed'}
    Expect ([IO.File]::ReadAllText((Join-Path $out 'elevated-downloaded.txt')) -ceq $typed) 'saved elevated output matches downloaded file'
    $ordinaryAfter=Good (Call windows) 'ordinary request after elevated batch'
    Expect (!$ordinaryAfter.worker.elevated) 'elevation does not persist into later ordinary request'
    $physical|ConvertTo-Json -Depth 30|Set-Content (Join-Path $out 'elevated-observe.json')
    @{version=$state.result.version;computer=$Computer;checks=$checks;worker=$physical.worker;target=$window.privileges;image=$physical.image_path}|ConvertTo-Json -Depth 10|Tee-Object (Join-Path $out 'elevation-summary.json')
} catch {$failure=$_;throw}
finally {
    try {
        if($window){$closed=Call act @{elevated=$true;window=$window.hwnd;expected=@{pid=$window.pid;process_start=$window.process_start};actions=@(@{type='key';keys='Alt+F4'})};if(!$closed.ok){throw 'Could not close elevation test window'}}
        $null=NativeSsh ('cmd.exe /d /c del /q RemoteAgent\'+$fixtureName)
    } catch {if(!$failure){throw};Write-Warning $_.Exception.Message}
}
