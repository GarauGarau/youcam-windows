param([string]$BuildDirectory = 'build')

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$executable = Join-Path $repoRoot "$BuildDirectory/Release/YouCam.exe"
if (-not (Test-Path -LiteralPath $executable)) {
    throw 'Build the Release executable before packaging.'
}

$destination = Join-Path $repoRoot 'dist'
New-Item -ItemType Directory -Path $destination -Force | Out-Null
Copy-Item -LiteralPath $executable -Destination (Join-Path $destination 'YouCam.exe')
Copy-Item -LiteralPath (Join-Path $repoRoot 'LICENSE'), (Join-Path $repoRoot 'README.md') -Destination $destination

$files = @('YouCam.exe', 'LICENSE', 'README.md') | ForEach-Object { Join-Path $destination $_ }
Compress-Archive -LiteralPath $files -DestinationPath (Join-Path $destination 'YouCam-windows-x64.zip') -Force
$checksums = foreach ($name in @('YouCam.exe', 'YouCam-windows-x64.zip')) {
    $hash = (Get-FileHash -LiteralPath (Join-Path $destination $name) -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $name"
}
$checksums | Set-Content -LiteralPath (Join-Path $destination 'SHA256SUMS.txt') -Encoding ascii
Write-Output "Ready: $destination"
