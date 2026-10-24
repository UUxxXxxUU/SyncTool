#requires -Version 5.1
[CmdletBinding()]
param(
    [string]$QtDir,
    [string]$OpenSslDir,
    [string]$VisualStudioDir,
    [string]$QtMsBuildDir,
    [switch]$Upload
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-File {
    param([string]$FilePath)
    if ([string]::IsNullOrWhiteSpace($FilePath) -or !(Test-Path -LiteralPath $FilePath -PathType Leaf)) {
        throw "Required file not found: $FilePath"
    }
}

function Resolve-Directory {
    param([string]$DirectoryPath)
    if ([string]::IsNullOrWhiteSpace($DirectoryPath) -or !(Test-Path -LiteralPath $DirectoryPath -PathType Container)) {
        throw "Required directory not found: $DirectoryPath"
    }
    return (Resolve-Path -LiteralPath $DirectoryPath).Path
}

function Invoke-Native {
    param([string]$FilePath, [string[]]$Arguments)
    Assert-File $FilePath
    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "Command failed with exit code ${LASTEXITCODE}: $FilePath"
    }
}

function Assert-X64Binary {
    param([string]$FilePath)
    Assert-File $FilePath
    $reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($FilePath))
    try {
        if ($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5A4D) {
            throw "Invalid Windows binary: $FilePath"
        }
        $reader.BaseStream.Position = 60
        $headerOffset = $reader.ReadInt32()
        # Check the PE header boundary before reading its machine type.
        if ($headerOffset -lt 64 -or $headerOffset -gt ($reader.BaseStream.Length - 6)) {
            throw "Invalid PE header: $FilePath"
        }
        $reader.BaseStream.Position = $headerOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "An x64 Windows binary is required: $FilePath"
        }
    } finally {
        $reader.Dispose()
    }
}

if ([System.Environment]::OSVersion.Platform -ne [System.PlatformID]::Win32NT) {
    throw 'This script requires Windows.'
}
$projectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$repository = 'UUxxXxxUU/SyncTool'
$manifestPath = Join-Path $projectRoot 'update.json'
$projectPath = Join-Path $projectRoot 'RemoteReleaseSync.vcxproj'
Assert-File $manifestPath
Assert-File $projectPath
[xml]$projectXml = Get-Content -LiteralPath $projectPath -Raw -Encoding UTF8
$projectToolset = $projectXml.SelectSingleNode("//*[local-name()='PropertyGroup' and @Label='Configuration' and contains(@Condition, 'Release|x64')]/*[local-name()='PlatformToolset']")
if (!$projectToolset -or $projectToolset.InnerText -ne 'v142') {
    throw 'The project must use the existing v142 toolset. Toolset changes require updating the packaging configuration.'
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw -Encoding UTF8 | ConvertFrom-Json
foreach ($field in @('version', 'notes', 'downloadUrl')) {
    if ($manifest.PSObject.Properties.Name -notcontains $field -or
        $manifest.$field -isnot [string] -or [string]::IsNullOrWhiteSpace($manifest.$field)) {
        throw "update.json must contain a non-empty string: $field"
    }
}
$version = $manifest.version
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw 'The release version must use the format major.minor.patch.'
}
$mainSource = Get-Content -LiteralPath (Join-Path $projectRoot 'src\main.cpp') -Raw -Encoding UTF8
$cmakeSource = Get-Content -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt') -Raw -Encoding UTF8
$appVersion = [regex]::Match($mainSource, 'setApplicationVersion\("([^"]+)"\)')
$cmakeVersion = [regex]::Match($cmakeSource, 'project\(RemoteReleaseSync\s+VERSION\s+(\S+)')
if (!$appVersion.Success -or !$cmakeVersion.Success -or
    $appVersion.Groups[1].Value -ne $version -or $cmakeVersion.Groups[1].Value -ne $version) {
    throw 'Versions in main.cpp, CMakeLists.txt and update.json must match.'
}
$tag = "v$version"

if ([string]::IsNullOrWhiteSpace($QtDir)) {
    $qmakeCommand = Get-Command qmake.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$qmakeCommand) { throw 'qmake.exe was not found. Provide -QtDir.' }
    $QtDir = ((Invoke-Native $qmakeCommand.Source @('-query', 'QT_INSTALL_PREFIX')) -join "`n").Trim()
}
$QtDir = Resolve-Directory $QtDir
$qtBinDir = Join-Path $QtDir 'bin'
$qmakePath = Join-Path $qtBinDir 'qmake.exe'
$deployPath = Join-Path $qtBinDir 'windeployqt.exe'
Assert-File $deployPath
$qtVersion = ((Invoke-Native $qmakePath @('-query', 'QT_VERSION')) -join "`n").Trim()
if ($qtVersion -ne '5.12.3') {
    throw "This project uses Qt 5.12.3; found $qtVersion. Dependency versions are not changed automatically."
}
$qtConfig = Get-Content -LiteralPath (Join-Path $QtDir 'mkspecs\qconfig.pri') -Raw -Encoding UTF8
$networkConfig = Get-Content -LiteralPath (Join-Path $QtDir 'include\QtNetwork\qtnetwork-config.h') -Raw -Encoding UTF8
if ($qtConfig -notmatch '(?m)^QT_ARCH\s*=\s*x86_64\s*$' -or
    $qtConfig -notmatch '(?m)^QT_MSVC_MAJOR_VERSION\s*=\s*19\s*$' -or
    $networkConfig -notmatch '(?m)^#define QT_FEATURE_opensslv11 -1\s*$') {
    throw 'The Qt kit must be Qt 5.12.3 MSVC x64 with the OpenSSL 1.0 interface.'
}

if ([string]::IsNullOrWhiteSpace($OpenSslDir)) { $OpenSslDir = $qtBinDir }
$OpenSslDir = Resolve-Directory $OpenSslDir
$sslFiles = @('libeay32.dll', 'ssleay32.dll')
foreach ($name in $sslFiles) {
    $sslPath = Join-Path $OpenSslDir $name
    if (!(Test-Path -LiteralPath $sslPath -PathType Leaf)) {
        throw "Missing $name. Provide -OpenSslDir containing compatible OpenSSL 1.0.x x64 DLLs."
    }
    Assert-X64Binary $sslPath
    $sslVersion = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($sslPath).FileVersion
    if ($sslVersion -and $sslVersion -notmatch '^1\.0\.') {
        throw "An OpenSSL 1.0.x DLL is required: $sslPath ($sslVersion)"
    }
}

$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if ([string]::IsNullOrWhiteSpace($VisualStudioDir)) {
    $installations = ((Invoke-Native $vswherePath @('-all', '-products', '*', '-format', 'json')) -join "`n") | ConvertFrom-Json
    $installation = $installations | Where-Object {
        (Test-Path -LiteralPath (Join-Path $_.installationPath 'MSBuild\Current\Bin\MSBuild.exe')) -and
        (Test-Path -LiteralPath (Join-Path $_.installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v142.default.txt'))
    } | Sort-Object { [version]$_.installationVersion } -Descending | Select-Object -First 1
    if (!$installation) { throw 'Install Visual Studio C++ build tools, the v142 toolset and the project Windows SDK.' }
    $VisualStudioDir = $installation.installationPath
}
$VisualStudioDir = Resolve-Directory $VisualStudioDir
$msbuildPath = Join-Path $VisualStudioDir 'MSBuild\Current\Bin\MSBuild.exe'
Assert-File $msbuildPath
$toolsetVersion = (Get-Content -LiteralPath (Join-Path $VisualStudioDir 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.v142.default.txt') -Raw -Encoding UTF8).Trim()
if ($toolsetVersion -notmatch '^14\.\d+\.\d+$') { throw 'Invalid v142 toolset version.' }
Assert-File (Join-Path $VisualStudioDir "VC\Tools\MSVC\$toolsetVersion\bin\Hostx64\x64\cl.exe")
$runtimeDir = Resolve-Directory (Join-Path $VisualStudioDir "VC\Redist\MSVC\$toolsetVersion\x64\Microsoft.VC142.CRT")
foreach ($name in @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
    Assert-X64Binary (Join-Path $runtimeDir $name)
}
if ([string]::IsNullOrWhiteSpace($QtMsBuildDir)) {
    if ($env:QtMsBuild) { $QtMsBuildDir = $env:QtMsBuild }
    else { $QtMsBuildDir = Join-Path $env:LOCALAPPDATA 'QtMsBuild' }
}
$QtMsBuildDir = Resolve-Directory $QtMsBuildDir
foreach ($name in @('Qt.props', 'qt.targets', 'qt_defaults.props')) {
    Assert-File (Join-Path $QtMsBuildDir $name)
}

$commit = $null
$ghPath = $null
$gitPath = $null
if ($Upload) {
    $ghCommand = Get-Command gh.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $gitCommand = Get-Command git.exe -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if (!$ghCommand -or !$gitCommand) { throw 'Upload requires Git and GitHub CLI (gh). Run gh auth login first.' }
    $ghPath = $ghCommand.Source
    $gitPath = $gitCommand.Source
    Invoke-Native $ghPath @('auth', 'status', '--hostname', 'github.com') | Out-Host
    $status = @(Invoke-Native $gitPath @('-C', $projectRoot, 'status', '--porcelain'))
    if ($status.Count -ne 0) { throw 'Commit and push all changes before using -Upload.' }
    $commit = ((Invoke-Native $gitPath @('-C', $projectRoot, 'rev-parse', 'HEAD')) -join '').Trim()
    $remoteCommit = ((Invoke-Native $ghPath @('api', "repos/$repository/commits/master", '--jq', '.sha')) -join '').Trim()
    if ($commit -notmatch '^[0-9a-f]{40}$' -or $commit -ne $remoteCommit) {
        throw "Local HEAD must match $repository master. Push your commit before uploading."
    }
    $releases = ((Invoke-Native $ghPath @('release', 'list', '--repo', $repository, '--limit', '100', '--json', 'tagName')) -join "`n") | ConvertFrom-Json
    if (@($releases | Where-Object { $_.tagName -eq $tag }).Count -ne 0) {
        throw "Release $tag already exists. Existing releases and assets are never overwritten."
    }
    $tagRefs = @(Invoke-Native $gitPath @('ls-remote', "https://github.com/$repository.git", "refs/tags/$tag", "refs/tags/$tag^{}"))
    if ($tagRefs.Count -gt 0) {
        $peeledRef = $tagRefs | Where-Object { $_.EndsWith('^{}') } | Select-Object -First 1
        if (!$peeledRef) { $peeledRef = $tagRefs[0] }
        if (($peeledRef -split '\s+')[0] -ne $commit) {
            throw "Existing tag $tag points to a different commit."
        }
    }
}

$packageName = "RemoteReleaseSync-$version-windows-x64"
$runName = 'release-{0}-{1}-{2}' -f $version, (Get-Date -Format 'yyyyMMdd-HHmmss'), ([guid]::NewGuid().ToString('N').Substring(0, 8))
$runDir = Join-Path $projectRoot "artifacts\$runName"
$buildDir = Join-Path $runDir 'build'
$binDir = Join-Path $buildDir 'release'
$stageDir = Join-Path $runDir $packageName
New-Item -ItemType Directory -Path $buildDir, $stageDir | Out-Null

# The project fixes moc and object paths under Release; build an isolated source copy with those paths intact.
$buildProjectPath = Join-Path $buildDir 'RemoteReleaseSync.vcxproj'
Copy-Item -LiteralPath $projectPath -Destination $buildProjectPath
$sourceItems = @($projectXml.SelectNodes("//*[local-name()='ItemGroup']/*[@Include and (local-name()='ClCompile' or local-name()='ClInclude' or local-name()='QtMoc')]"))
if ($sourceItems.Count -eq 0) { throw 'No source files were found in the Visual Studio project.' }
$rootPrefix = $projectRoot.TrimEnd([char[]]@('\', '/')) + '\'
foreach ($item in $sourceItems) {
    $sourceInclude = $item.GetAttribute('Include')
    if ([string]::IsNullOrWhiteSpace($sourceInclude) -or [System.IO.Path]::IsPathRooted($sourceInclude) -or
        $sourceInclude.IndexOfAny([char[]]@('*', '?', '$', ';')) -ge 0) {
        throw "Project inputs must use explicit relative file paths: $sourceInclude"
    }
    $sourcePath = [System.IO.Path]::GetFullPath((Join-Path $projectRoot $sourceInclude))
    if (!$sourcePath.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Project inputs must be inside the repository: $sourcePath"
    }
    Assert-File $sourcePath
    $destinationPath = Join-Path $buildDir $sourcePath.Substring($rootPrefix.Length)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destinationPath) -Force | Out-Null
    Copy-Item -LiteralPath $sourcePath -Destination $destinationPath
}
# .qrc and .rc files reference icons by relative path, so copy each resource directory as a whole.
$resourceItems = @($projectXml.SelectNodes("//*[local-name()='ItemGroup']/*[@Include and (local-name()='QtRcc' or local-name()='ResourceCompile')]"))
$resourceDirs = @($resourceItems | ForEach-Object {
    Split-Path -Parent ([System.IO.Path]::GetFullPath((Join-Path $projectRoot $_.GetAttribute('Include'))))
} | Select-Object -Unique)
foreach ($resourceDir in $resourceDirs) {
    if (!$resourceDir.StartsWith($rootPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Resource files must be in a subdirectory of the repository: $resourceDir"
    }
    $destinationDir = Join-Path $buildDir $resourceDir.Substring($rootPrefix.Length)
    New-Item -ItemType Directory -Path (Split-Path -Parent $destinationDir) -Force | Out-Null
    Copy-Item -LiteralPath $resourceDir -Destination $destinationDir -Recurse
}

$previousPath = $env:PATH
try {
    $env:PATH = "$qtBinDir;$previousPath"
    $buildArguments = @(
        $buildProjectPath, '/t:Build', '/m', '/nologo', '/p:Configuration=Release', '/p:Platform=x64',
        '/p:PlatformToolset=v142', "/p:VCToolsVersion=$toolsetVersion", "/p:QtInstall=$QtDir", "/p:QtMsBuild=$QtMsBuildDir"
    )
    Write-Host "Building Release x64 with Qt $qtVersion and v142..."
    Push-Location -LiteralPath $buildDir
    try {
        Invoke-Native $msbuildPath $buildArguments | Out-Host
    } finally {
        Pop-Location
    }
    $builtExe = Join-Path $binDir 'RemoteReleaseSync.exe'
    Assert-X64Binary $builtExe
    $packageExe = Join-Path $stageDir 'RemoteReleaseSync.exe'
    Copy-Item -LiteralPath $builtExe -Destination $packageExe
    # Copy the actual v142 runtime DLLs below instead of bundling an installer that still needs to run.
    Invoke-Native $deployPath @('--release', '--no-compiler-runtime', '--no-translations', '--dir', $stageDir, $packageExe) | Out-Host
} finally {
    $env:PATH = $previousPath
}
Get-ChildItem -LiteralPath $runtimeDir -Filter '*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination $stageDir
}
foreach ($name in $sslFiles) {
    Copy-Item -LiteralPath (Join-Path $OpenSslDir $name) -Destination $stageDir
}
foreach ($name in @('LICENSE', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $stageDir
}
foreach ($name in @('Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Network.dll', 'platforms\qwindows.dll')) {
    Assert-File (Join-Path $stageDir $name)
}
Get-ChildItem -LiteralPath $stageDir -Recurse -File | Where-Object { $_.Extension -in @('.dll', '.exe') } | ForEach-Object {
    Assert-X64Binary $_.FullName
}
if (Get-ChildItem -LiteralPath $stageDir -Recurse -File -Filter 'sync_config.json') {
    throw 'The release package must not contain personal sync_config.json files.'
}

$zipPath = Join-Path $runDir "$packageName.zip"
$checksumPath = "$zipPath.sha256"
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stageDir, $zipPath, [System.IO.Compression.CompressionLevel]::Optimal, $true)
$checksum = (Get-FileHash -LiteralPath $zipPath -Algorithm SHA256).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText($checksumPath, "$checksum *$packageName.zip`n", [System.Text.Encoding]::ASCII)
Write-Host "ZIP:    $zipPath"
Write-Host "SHA256: $checksumPath"

if ($Upload) {
    # Recheck after the build so edits or a changed remote branch cannot be published under the original commit.
    $status = @(Invoke-Native $gitPath @('-C', $projectRoot, 'status', '--porcelain'))
    $currentCommit = ((Invoke-Native $gitPath @('-C', $projectRoot, 'rev-parse', 'HEAD')) -join '').Trim()
    $remoteCommit = ((Invoke-Native $ghPath @('api', "repos/$repository/commits/master", '--jq', '.sha')) -join '').Trim()
    if ($status.Count -ne 0 -or $currentCommit -ne $commit -or $remoteCommit -ne $commit) {
        throw 'The source or GitHub master changed during packaging. Upload was cancelled; local artifacts are retained.'
    }
    $notesPath = Join-Path $runDir 'release-notes.md'
    [System.IO.File]::WriteAllText($notesPath, $manifest.notes + "`n`nCommit: $commit`n", [System.Text.UTF8Encoding]::new($false))
    Write-Host "Creating draft release $tag in $repository..."
    Invoke-Native $ghPath @('release', 'create', $tag, $zipPath, $checksumPath, '--repo', $repository,
        '--target', $commit, '--title', "RemoteReleaseSync $version", '--notes-file', $notesPath, '--draft') | Out-Host
    Write-Host 'Draft uploaded. Verify the package on a Windows machine without Qt before publishing it on GitHub.'
}
