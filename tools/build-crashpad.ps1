#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$CMakePath = 'cmake.exe',
    [string]$SourceDir,
    [string]$BuildDir
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$pin = 'e24b0f9e760e27464fe2ed30fdd7be45a27a67ad'
$artifactRoot = Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts\dependencies'
if (!$SourceDir) { $SourceDir = Join-Path $artifactRoot 'crashpad-source' }
if (!$BuildDir) { $BuildDir = Join-Path $artifactRoot 'crashpad-build' }
$SourceDir = [IO.Path]::GetFullPath($SourceDir)
$BuildDir = [IO.Path]::GetFullPath($BuildDir)

function Invoke-BuildCommand {
    param([string]$Command, [string[]]$Arguments)
    & $Command @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Command failed with exit code $LASTEXITCODE." }
}

if (!(Test-Path -LiteralPath (Join-Path $SourceDir '.git'))) {
    New-Item -ItemType Directory -Path $SourceDir -Force | Out-Null
    Invoke-BuildCommand git.exe @('-C', $SourceDir, 'init') | Out-Host
}
$head = & git.exe -C $SourceDir rev-parse --verify --quiet HEAD 2>$null
if ($LASTEXITCODE -ne 0) {
    Invoke-BuildCommand git.exe @('-C', $SourceDir, 'fetch', '--depth', '1',
        'https://github.com/getsentry/crashpad.git', $pin) | Out-Host
    Invoke-BuildCommand git.exe @('-C', $SourceDir, 'checkout', '--detach', $pin) | Out-Host
} elseif ($head -ne $pin) {
    throw "Crashpad source must be pinned to $pin. Current HEAD: $head."
}

$dirty = Invoke-BuildCommand git.exe @('-C', $SourceDir, 'status', '--porcelain', '--untracked-files=no')
if ($dirty) { throw 'Crashpad sources or their submodule pins contain local changes.' }
Invoke-BuildCommand git.exe @('-C', $SourceDir, 'submodule', 'update', '--init', '--depth', '1', '--',
    'third_party/mini_chromium/mini_chromium', 'third_party/zlib/zlib') | Out-Host
Invoke-BuildCommand $CMakePath @('-S', (Join-Path $PSScriptRoot 'crashpad'), '-B', $BuildDir,
    '-G', 'Visual Studio 17 2022', '-A', 'x64', "-DCRASHPAD_SOURCE_DIR=$SourceDir") | Out-Host
Invoke-BuildCommand $CMakePath @('--build', $BuildDir, '--config', 'Release', '--target', 'SyncToolCrashReporter', '--parallel') | Out-Host

$licenses = Join-Path $BuildDir 'LICENSES'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $SourceDir 'LICENSE') -Destination (Join-Path $licenses 'Crashpad.txt')
Copy-Item -LiteralPath (Join-Path $SourceDir 'third_party\mini_chromium\mini_chromium\LICENSE') -Destination (Join-Path $licenses 'Crashpad-mini-chromium.txt')
Copy-Item -LiteralPath (Join-Path $SourceDir 'third_party\zlib\zlib\LICENSE') -Destination (Join-Path $licenses 'Crashpad-zlib.txt')
Write-Host "Crashpad binaries: $(Join-Path $BuildDir 'Release')"
