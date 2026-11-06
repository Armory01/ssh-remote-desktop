[CmdletBinding()]
param([Parameter(Mandatory)][string]$Computer, [string[]]$SshOption=@(), [string]$FixtureExecutable, [string]$RemotePath='.\RemoteAgent\ra.exe')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$receiver=Join-Path $root 'skills/ssh-remote-desktop/scripts/Receive-Ra.ps1'
$out=Join-Path $root '.test-output'
[IO.Directory]::CreateDirectory($out) | Out-Null
$checks=[Collections.Generic.List[string]]::new()
function Expect($Condition,[string]$Name) { if(!$Condition){throw "FAILED: $Name"};$checks.Add($Name) }
function Call([string]$Op,[hashtable]$Params=@{},[int]$Timeout=30000) {
    $request=@{op=$Op;params=$Params;timeout_ms=$Timeout}|ConvertTo-Json -Depth 30 -Compress
    $raw=& $receiver -Computer $Computer -Request $request -SshOption $SshOption -OutputDirectory $out -RemotePath $RemotePath
    return ($raw|ConvertFrom-Json -AsHashtable)
}
function Good($Result,[string]$Name) { Expect $Result.ok "$Name ($($Result.error.message))";return $Result }
function NativeSsh([string]$Command) { $r=& ssh -T @SshOption $Computer $Command;if($LASTEXITCODE){throw 'SSH failed'};return ($r -join "`n").Trim() }
$profilePath=NativeSsh 'cmd.exe /d /c echo %USERPROFILE%'
$run=[guid]::NewGuid().ToString('N')
$remoteFile="$profilePath\RemoteAgent\smoke-$run 中文.txt"
$fixtureName="ra-fixture-$run.exe"
$remoteFixture="$profilePath\RemoteAgent\$fixtureName"
if(!$FixtureExecutable){$FixtureExecutable=Join-Path $root 'build/x64/Release/ra_gui_fixture.exe'}
& scp @SshOption $FixtureExecutable ($Computer+':RemoteAgent/'+$fixtureName)
if($LASTEXITCODE){throw 'Fixture upload failed'}
$state=Good (Call status) 'SSH status'
Expect ($state.result.process_session_id -eq 0) 'SSH runs outside interactive desktop'
$started=Good (Call launch @{exe=$remoteFixture;args=@($remoteFile)}) 'detached GUI launch'
$window=$null
for($attempt=0;$attempt -lt 10 -and !$window;$attempt++) {
    $windows=Good (Call windows) 'enumerate windows'
    $window=@($windows.result.windows|Where-Object pid -eq $started.result.pid)|Select-Object -First 1
    if(!$window){Start-Sleep -Milliseconds 200}
}
Expect ($null -ne $window) 'launched process survives worker and SSH exit'
$hwnd=$window.hwnd
try {
    $observed=Good (Call observe @{window=$hwnd}) 'PNG screenshot and UIA'
    $png=[IO.File]::ReadAllBytes($observed.image_path)
    Expect ($png.Length -eq $observed.image_bytes -and $png[0] -eq 137 -and $png[1] -eq 80) 'binary-safe PNG receive'
    Expect ($observed.result.uia.available -and !$observed.result.uia.truncated) 'UIA tree from remote session'
    $stale=Call act @{window=$hwnd;expected=@{pid=0};actions=@(@{type='focus'})}
    Expect (!$stale.ok -and $stale.error.code -eq 'STALE_TARGET') 'stale target rejects input'
    $text="UIA 中文、English、单引号 ' 和空格。"
    $set=Good (Call act @{window=$hwnd;actions=@(@{type='set_value';selector=@{automation_id='101'};text=$text},@{type='invoke';selector=@{automation_id='102'}},@{type='wait';ms=100});observe=$true}) 'UIA SetValue and Invoke'
    & scp @SshOption ($Computer+':RemoteAgent/smoke-'+$run+' 中文.txt') (Join-Path $out 'smoke-downloaded.txt')
    if($LASTEXITCODE){throw 'Download failed'}
    Expect (([IO.File]::ReadAllText((Join-Path $out 'smoke-downloaded.txt'))) -ceq $text) 'SCP Chinese and spaced path roundtrip'
    # Prepare the foreground for ordinary-input regression. Some test machines
    # have an unrelated elevated foreground window which a medium worker cannot
    # attach to. This preparation is explicit and does not elevate later calls.
    if($state.result.privileges.elevated -and $state.result.capabilities -contains 'elevated_worker') {
        $null=Good (Call act @{elevated=$true;window=$hwnd;actions=@(@{type='focus'})}) 'prepare ordinary foreground'
    }
    $edit=@($set.result.uia.elements|Where-Object automation_id -eq '101')[0].bounds
    $save=@($set.result.uia.elements|Where-Object automation_id -eq '102')[0].bounds
    $typed='鼠标键盘测试 Chinese 123'
    $physical=Good (Call act @{window=$hwnd;actions=@(
        @{type='click';x=($edit.x+40);y=($edit.y+30)},
        @{type='key';keys='Ctrl+Home'},@{type='key';keys='Ctrl+Shift+End'},
        @{type='type';text=$typed},@{type='click';x=($save.x+40);y=($save.y+20)},@{type='wait';ms=100});observe=$true}) 'mouse keyboard Unicode batch'
    $value=@($physical.result.uia.elements|Where-Object automation_id -eq '101')[0].value
    Expect ($value -ceq $typed) 'physical input changes actual UI value'
    $jpeg=Good (Call observe @{window=$hwnd;format='jpeg';uia=$false}) 'JPEG capture'
    $bytes=[IO.File]::ReadAllBytes($jpeg.image_path)
    Expect ($bytes[0] -eq 255 -and $bytes[1] -eq 216 -and $bytes.Length -eq $jpeg.image_bytes) 'binary-safe JPEG receive'
    $partial=Call act @{window=$hwnd;actions=@(@{type='wait';ms=1},@{type='not-an-action'},@{type='type';text='must not run'})}
    Expect (!$partial.ok -and $partial.result.completed.Count -eq 1 -and $partial.result.failed_index -eq 1) 'partial failure stops remaining actions'
    $missing=Call windows @{session_id=4294967294}
    Expect (!$missing.ok -and $missing.error.code -eq 'NO_INTERACTIVE_SESSION') 'explicit absent session error'
    $observed | ConvertTo-Json -Depth 30 | Set-Content (Join-Path $out 'smoke-observe.json')
    $physical | ConvertTo-Json -Depth 30 | Set-Content (Join-Path $out 'smoke-physical.json')
    $summary=@{computer=$Computer;hostname=$state.result.hostname;checks=$checks;png=$physical.image_path;jpeg=$jpeg.image_path;fixture_pid=$started.result.pid;remote_test_file=$remoteFile}
    $summary|ConvertTo-Json -Depth 8|Tee-Object -FilePath (Join-Path $out 'smoke-summary.json')
} finally {
    # Only this invocation's fixture is closed; no unrelated desktop application.
    $null=Call act @{window=$hwnd;actions=@(@{type='key';keys='Alt+F4'})}
    $null=NativeSsh ('cmd.exe /d /c del /q RemoteAgent\'+$fixtureName)
}
