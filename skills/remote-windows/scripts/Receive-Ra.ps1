[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Computer,
    [Parameter(Mandatory)][string]$Request,
    [string]$RemotePath = '.\RemoteAgent\ra.exe',
    [string]$OutputDirectory = (Join-Path ([IO.Path]::GetTempPath()) 'RemoteAgent'),
    [string[]]$SshOption = @(),
    # Local execution is useful for development and protocol tests, not deployment.
    [string]$LocalExecutable
)
$ErrorActionPreference = 'Stop'
$requestObject = $Request | ConvertFrom-Json -AsHashtable
if (!$requestObject.ContainsKey('id')) { $requestObject.id = [guid]::NewGuid().ToString() }
if (!$requestObject.ContainsKey('protocol')) { $requestObject.protocol = 1 }
if (!$requestObject.ContainsKey('timeout_ms')) { $requestObject.timeout_ms = if ($requestObject.op -eq 'login') { 120000 } else { 30000 } }
$Request = $requestObject | ConvertTo-Json -Depth 40 -Compress
$budget = if ($requestObject.timeout_ms) { [int]$requestObject.timeout_ms } else { 30000 }
if ($budget -lt 1000 -or $budget -gt 120000) { throw 'timeout_ms must be between 1000 and 120000.' }
$deadline = [DateTime]::UtcNow.AddMilliseconds($budget + 30000) # SSH authentication/connection allowance
$process = [Diagnostics.Process]::new()
$start = $process.StartInfo
$start.UseShellExecute = $false
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
# Keep stderr attached to the terminal: native SSH owns password/host-key prompts.
$start.RedirectStandardError = $false
if ($LocalExecutable) {
    $start.FileName = (Resolve-Path -LiteralPath $LocalExecutable).Path
    $start.ArgumentList.Add('call')
} else {
    if ($Computer.StartsWith('-')) { throw 'Computer must be an SSH host alias or user@host.' }
    if ($RemotePath -match '["%&|<>^!\r\n]') { throw 'Unsupported metacharacter in RemotePath.' }
    $start.FileName = (Get-Command ssh -ErrorAction Stop).Source
    foreach ($arg in @('-T','-o','ConnectTimeout=10','-o','ServerAliveInterval=10','-o','ServerAliveCountMax=2') + $SshOption + @($Computer)) { $start.ArgumentList.Add($arg) }
    $start.ArgumentList.Add('cmd.exe /d /s /c ""' + $RemotePath + '" call"')
}
function Read-Bytes([IO.Stream]$Stream, [byte[]]$Buffer, [int]$Count) {
    $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if ([DateTime]::UtcNow -ge $deadline) { throw 'SSH response timed out; action outcome may be unknown. Observe before retrying.' }
    $read = $Stream.ReadAsync($Buffer, 0, $Count)
    if (!$read.Wait($remaining)) { throw 'SSH response timed out; action outcome may be unknown. Observe before retrying.' }
    return $read.GetAwaiter().GetResult()
}
$imagePath = $null
$complete = $false
$started = $false
try {
    if (!$process.Start()) { throw 'Unable to start transport process.' }
    $started = $true
    $inputBytes = [Text.Encoding]::UTF8.GetBytes($Request)
    $process.StandardInput.BaseStream.Write($inputBytes, 0, $inputBytes.Length)
    $process.StandardInput.Close()
    $stream = $process.StandardOutput.BaseStream
    # Avoid StreamReader: its read-ahead would consume bytes belonging to the image.
    $header = [IO.MemoryStream]::new()
    $one = [byte[]]::new(1)
    while ($true) {
        if ((Read-Bytes $stream $one 1) -eq 0) { throw 'SSH closed before a complete result header. Check stderr; action outcome may be unknown.' }
        if ($one[0] -eq 10) { break }
        $header.WriteByte($one[0])
        if ($header.Length -gt 1MB) { throw 'Result header exceeds 1 MiB.' }
    }
    $utf8 = [Text.UTF8Encoding]::new($false, $true)
    $result = $utf8.GetString($header.ToArray()) | ConvertFrom-Json -AsHashtable
    $header.Dispose()
    if ($result.protocol -ne 1 -or $result.id -cne $requestObject.id -or $result.ok -isnot [bool]) { throw 'Invalid protocol or response ID.' }
    $size = $result.image_bytes
    if ($null -eq $size -or $size -is [string] -or $size -lt 0 -or $size -gt 32MB -or [long]$size -ne $size) { throw 'Invalid image length.' }
    if ($size -gt 0) {
        $extension = switch ($result.mime_type) { 'image/png' { 'png' } 'image/jpeg' { 'jpg' } default { throw 'Unsupported image MIME type.' } }
        [IO.Directory]::CreateDirectory($OutputDirectory) | Out-Null
        $imagePath = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) (([guid]::NewGuid().ToString()) + '.' + $extension)
        $file = [IO.File]::Open($imagePath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
        try {
            $buffer = [byte[]]::new(65536)
            while ($size -gt 0) {
                $n = Read-Bytes $stream $buffer ([int][Math]::Min($size, $buffer.Length))
                if ($n -eq 0) { throw 'Image transfer interrupted; partial image discarded. Action outcome may be unknown.' }
                $file.Write($buffer, 0, $n)
                $size -= $n
            }
        } finally { $file.Dispose() }
    }
    if ((Read-Bytes $stream $one 1) -ne 0) { throw 'Unexpected bytes after result frame.' }
    $remaining = [int][Math]::Max(1, ($deadline - [DateTime]::UtcNow).TotalMilliseconds)
    if (!$process.WaitForExit($remaining)) { throw 'SSH did not exit; action outcome may be unknown.' }
    $result.transport_exit_code = $process.ExitCode
    if ($result.ok -and $process.ExitCode -ne 0) { throw "Successful header followed by transport exit $($process.ExitCode); verify state before retrying." }
    $result.image_path = $imagePath
    $complete = $true
    $result | ConvertTo-Json -Depth 40 -Compress
} finally {
    if ($started -and !$process.HasExited) { $process.Kill() }
    $process.Dispose()
    if (!$complete -and $imagePath -and [IO.File]::Exists($imagePath)) { [IO.File]::Delete($imagePath) }
}
