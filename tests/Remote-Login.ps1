[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Computer,
    [Parameter(Mandatory)][securestring]$Password,
    [ValidateSet('Logout','Reboot')][string]$Mode='Logout',
    [string[]]$SshOption=@(),
    [string]$RemotePath='.\RemoteAgent\ra.exe',
    [switch]$RequireVirtualDisplay
)
# Explicitly logs out or reboots the authorized test host. No credential retries.
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$receiver=Join-Path $root 'skills/ssh-remote-desktop/scripts/Receive-Ra.ps1'
$out=Join-Path $root '.test-output'
[IO.Directory]::CreateDirectory($out)|Out-Null
$checks=[Collections.Generic.List[string]]::new()
function Expect($Condition,[string]$Name){if(!$Condition){throw "FAILED: $Name"};$checks.Add($Name)}
function Call([string]$Op,[hashtable]$Params=@{},[int]$Timeout=30000){
    $request=@{op=$Op;params=$Params;timeout_ms=$Timeout}|ConvertTo-Json -Depth 16 -Compress
    try{return (& $receiver -Computer $Computer -RemotePath $RemotePath -Request $request -SshOption $SshOption -OutputDirectory $out|ConvertFrom-Json -AsHashtable)}
    finally{$request=$null;if($Params.ContainsKey('password')){$Params.password=$null}}
}
function Good($Reply,[string]$Name){if(!$Reply.ok){$Reply|ConvertTo-Json -Depth 20|Set-Content (Join-Path $out 'login-failure.json')};Expect $Reply.ok "$Name ($($Reply.error.message))";return $Reply}
$before=Good (Call status) 'initial SSH status'
Expect ($before.result.privileges.elevated -and $before.result.privileges.integrity_rid -ge 12288) 'administrator SSH caller'
Expect ($before.result.capabilities -contains 'experimental_local_password_login') 'login capability advertised'
$console=@($before.result.sessions|Where-Object id -eq $before.result.console_session_id)[0]
Expect ($console.same_user -and $console.state -eq 'active') 'test starts from the SSH account console'
if($Mode -eq 'Logout'){
    & ssh.exe -T @SshOption $Computer ('logoff '+$console.id)
    if($LASTEXITCODE){throw 'Console logout command failed'}
}else{
    & ssh.exe -T @SshOption $Computer 'shutdown.exe /r /t 3'
    if($LASTEXITCODE){throw 'Reboot command failed'}
}
$deadline=[DateTime]::UtcNow.AddSeconds(300)
$signedOut=$null;$lastError=$null
do{
    Start-Sleep -Seconds 2
    try{
        $state=Call status
        if(!$state.ok){continue}
        $candidate=@($state.result.sessions|Where-Object id -eq $state.result.console_session_id)|Select-Object -First 1
        $restarted=$Mode -ne 'Reboot' -or $state.result.uptime_ms -lt $before.result.uptime_ms
        if($candidate -and !$candidate.user -and $restarted){$signedOut=$state;break}
    }catch{$lastError=$_}
}while([DateTime]::UtcNow -lt $deadline)
Expect ($null -ne $signedOut) "SSH recovered with a signed-out console ($lastError)"
if($Mode -eq 'Reboot'){Expect ($signedOut.result.uptime_ms -lt $before.result.uptime_ms) 'Windows uptime proves actual reboot'}
$signedOut|ConvertTo-Json -Depth 14|Set-Content (Join-Path $out "login-$($Mode.ToLowerInvariant())-signed-out.json")
$probe=Good (Call desktop_probe @{session_id=$signedOut.result.console_session_id;format='jpeg'}) 'SYSTEM observes signed-out console with explicit session ID'
Expect ($probe.worker.integrity_level -eq 'system' -and $probe.worker.session_id -eq $signedOut.result.console_session_id) 'worker reaches the pre-login console'
if($RequireVirtualDisplay){
    $display=Good (Call display @{action='status'} 120000) 'virtual display before console login'
    Expect ($display.result.virtual_active -and $display.result.capture_ok -and $display.result.devices.Count -eq 1) 'virtual output and capture survive reboot or logout before login'
}
$normal=Call windows
Expect (!$normal.ok -and $normal.error.code -eq 'NO_INTERACTIVE_SESSION') 'ordinary worker cannot invent a user desktop'
$other=Call login @{user='RemoteAgent-NotTheSshAccount'}
Expect (!$other.ok -and $other.error.code -eq 'UNSUPPORTED_ACCOUNT' -and !$other.worker) 'different account rejected before worker or credentials'
$address=[Runtime.InteropServices.Marshal]::SecureStringToBSTR($Password)
try{$login=Good (Call login @{password=[Runtime.InteropServices.Marshal]::PtrToStringBSTR($address)} 120000) 'one password login'}
finally{[Runtime.InteropServices.Marshal]::ZeroFreeBSTR($address)}
Expect ($login.result.authenticated -and $login.result.password_submitted -and !$login.result.already_signed_in) 'Windows confirms a new graphical sign-in'
$login|ConvertTo-Json -Depth 14|Set-Content (Join-Path $out "login-$($Mode.ToLowerInvariant())-result.json")
$ready=$null
for($i=0;$i -lt 20;$i++){
    $ready=Good (Call desktop_probe @{screenshot=$false;uia=$false}) 'post-login desktop state'
    if($ready.result.input_desktop -eq 'Default' -and $ready.result.wts.locked -eq $false){break};Start-Sleep -Milliseconds 500
}
Expect ($ready.result.input_desktop -eq 'Default' -and $ready.result.wts.locked -eq $false) 'Default desktop is unlocked'
$after=Good (Call status) 'post-login account status'
$actual=@($after.result.sessions|Where-Object id -eq $after.result.console_session_id)[0]
Expect ($actual.same_user -and $actual.state -eq 'active') 'actual console SID matches the SSH account'
if($RequireVirtualDisplay){
    $display=Good (Call display @{action='status'} 120000) 'virtual display after console login'
    Expect ($display.result.virtual_active -and $display.result.capture_ok -and $display.result.desktop_ready -and $display.result.devices.Count -eq 1) 'virtual display stays usable after password login'
}
$windows=Good (Call windows) 'ordinary window enumeration after login'
Expect (!$windows.worker.elevated -and $windows.worker.integrity_level -eq 'medium') 'SYSTEM privilege does not persist into ordinary worker'
$noop=Good (Call login) 'already-signed-in no-op'
Expect ($noop.result.already_signed_in -and !$noop.result.password_submitted) 'no credentials submitted to an existing desktop'
$version=(& ssh.exe -T @SshOption $Computer 'pwsh -NoProfile -NonInteractive -Command "$PSVersionTable.PSVersion.ToString()"') -join ''
Expect ($LASTEXITCODE -eq 0 -and $version.Trim() -match '^7\.\d+\.\d+$') 'explicit pwsh works over SSH without WinRM or a default-shell change'
@{computer=$Computer;version=$after.result.version;mode=$Mode;checks=$checks;old_console=$before.result.console_session_id;new_console=$after.result.console_session_id;worker=$login.worker;login=$login.result;pwsh=$version.Trim()}|ConvertTo-Json -Depth 16|Tee-Object (Join-Path $out "login-$($Mode.ToLowerInvariant())-summary.json")
