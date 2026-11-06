[CmdletBinding()]
param()
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$version='0.4.0'
$dest=Join-Path $root "dist/$version"
[IO.Directory]::CreateDirectory($dest)|Out-Null
$hashes=[Collections.Generic.List[string]]::new()
foreach($arch in @(@{build='x64';name='windows-x64'},@{build='Win32';name='windows-x86'})) {
    $source=Join-Path $root "build/$($arch.build)/Release/ra.exe"
    if(!(Test-Path -LiteralPath $source)){throw "Build missing: $source"}
    $folder=Join-Path $dest $arch.name
    [IO.Directory]::CreateDirectory($folder)|Out-Null
    $target=Join-Path $folder 'ra.exe'
    Copy-Item -LiteralPath $source -Destination $target
    $hash=(Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant()
    $hashes.Add("$hash  $($arch.name)/ra.exe")
    @{version=$version;protocol=1;architecture=$arch.name;sha256=$hash;runtime='static MSVC /MT';compatibility='See docs/TEST_RESULTS.md; old Windows versions are not yet validated.'}|ConvertTo-Json|Set-Content (Join-Path $folder 'release.json')
}
$hashes|Set-Content (Join-Path $dest 'SHA256SUMS.txt')
[IO.Directory]::CreateDirectory((Join-Path $dest 'skills'))|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'skills/remote-windows') -Destination (Join-Path $dest 'skills') -Recurse -Force
foreach($name in @('README.md','LICENSE','CHANGELOG.md','CONTRIBUTING.md')) {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination $dest
}
[IO.Directory]::CreateDirectory((Join-Path $dest 'docs'))|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'docs/TEST_RESULTS.md') -Destination (Join-Path $dest 'docs')
Copy-Item -LiteralPath (Join-Path $root 'docs/IMPLEMENTATION_PLAN.md') -Destination (Join-Path $dest 'docs')
Copy-Item -LiteralPath (Join-Path $root 'docs/USAGE.md') -Destination (Join-Path $dest 'docs')
Copy-Item -LiteralPath (Join-Path $root 'docs/evidence') -Destination (Join-Path $dest 'docs') -Recurse -Force
[IO.Directory]::CreateDirectory((Join-Path $dest 'third_party'))|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'third_party/README.md') -Destination (Join-Path $dest 'third_party')
[IO.Directory]::CreateDirectory((Join-Path $dest 'licenses'))|Out-Null
Copy-Item -LiteralPath (Join-Path $root 'third_party/nlohmann/LICENSE.MIT') -Destination (Join-Path $dest 'licenses/nlohmann-json.txt')
$archive=Join-Path $root "dist/remote-windows-ssh-skill-$version.zip"
Compress-Archive -Path (Join-Path $dest '*') -DestinationPath $archive -Force
$archiveHash=(Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
"$archiveHash  $(Split-Path $archive -Leaf)" | Set-Content -LiteralPath "$archive.sha256"
Get-Item -LiteralPath $archive|Select-Object FullName,Length
