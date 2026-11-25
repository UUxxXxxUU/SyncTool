$ErrorActionPreference = 'Stop'
$startupName = 'RemoteReleaseSync_RemoteDebugger'
$phase = '检查远程环境'
$reboot = $false
$result = @{ success = $false; message = '' }
$elapsed = [System.Diagnostics.Stopwatch]::StartNew()

function Set-Phase([string]$message) {
    $script:phase = $message
    try {
        $state = @{ message = $message; time = [DateTime]::UtcNow.ToString('o'); elapsed_ms = $elapsed.ElapsedMilliseconds }
        $temporary = Join-Path $PSScriptRoot 'progress.partial'
        [System.IO.File]::WriteAllText($temporary, ($state | ConvertTo-Json -Compress), (New-Object System.Text.UTF8Encoding($true)))
        Move-Item -LiteralPath $temporary -Destination (Join-Path $PSScriptRoot 'progress.json') -Force
    } catch {
        # 进度文件写入失败不影响主操作，最终结果通过独立文件返回。
    }
}

function Find-Debugger([int]$major, [string]$architecture) {
    $roots = @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ } | Select-Object -Unique
    $candidates = @()
    $year = @{ 15 = 2017; 16 = 2019; 17 = 2022; 18 = 2026 }[$major]
    foreach ($root in $roots) {
        $candidates += Join-Path $root "Microsoft Visual Studio $major.0\Common7\IDE\Remote Debugger\$architecture\msvsmon.exe"
        $candidates += Join-Path $root "Microsoft Visual Studio\$year\Remote Debugger\$architecture\msvsmon.exe"
    }
    foreach ($registry in @('HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall', 'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')) {
        if (-not (Test-Path -LiteralPath $registry)) { continue }
        foreach ($key in Get-ChildItem -LiteralPath $registry) {
            $item = Get-ItemProperty -LiteralPath $key.PSPath
            if ($item.DisplayName -match 'Visual Studio|Microsoft Remote Tools' -and $item.DisplayName -match 'Remote|远程' -and $item.InstallLocation) {
                $candidates += Join-Path $item.InstallLocation "$architecture\msvsmon.exe"
                $candidates += Join-Path $item.InstallLocation "Common7\IDE\Remote Debugger\$architecture\msvsmon.exe"
                $candidates += Join-Path $item.InstallLocation 'msvsmon.exe'
            }
        }
    }
    foreach ($path in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { continue }
        $version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        if ($version.FileMajorPart -ne $major -or $version.OriginalFilename -ine 'msvsmon.exe') { continue }
        $stream = [System.IO.File]::OpenRead($path)
        $reader = New-Object System.IO.BinaryReader($stream)
        try {
            if ($stream.Length -lt 64) { continue }
            $stream.Position = 0x3c
            $offset = $reader.ReadUInt32()
            if ($offset -gt $stream.Length - 6) { continue }
            $stream.Position = $offset
            if ($reader.ReadUInt32() -ne 0x4550) { continue }
            $expected = @{ x86 = 0x14c; x64 = 0x8664; ARM64 = 0xaa64 }[$architecture]
            if ($reader.ReadUInt16() -eq $expected) {
                $signature = Get-AuthenticodeSignature -LiteralPath $path
                if ($signature.Status -eq 'Valid' -and $null -ne $signature.SignerCertificate -and
                    $signature.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -eq 'Microsoft Corporation') {
                    return [System.IO.Path]::GetFullPath($path)
                }
            }
        } finally { $reader.Dispose() }
    }
    throw '没有找到通过微软签名校验且符合所选版本、架构的 msvsmon.exe；请检查安装结果及是否需要重启。'
}

function Test-StartupTaskActive($task) {
    return $task.State -in @(2, 4) -or $task.GetInstances(0).Count -gt 0
}

function Assert-StartupTaskCanBeUpdated($task, [bool]$legacy, [bool]$stopLegacy) {
    if (-not (Test-StartupTaskActive $task)) { return }
    if (-not $legacy) { throw '调试器正在运行，请先在远端窗口退出，再重新配置自启。' }
    if (-not $stopLegacy) {
        $result.legacy_stop_confirmation_required = $true
        throw '旧后台调试器仍在运行。勾选“迁移时结束旧后台调试器”后重试。此操作可能中断当前调试。'
    }
}

try {
    Set-Phase '检查远程环境'
    $parameters = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'parameters.json') -Encoding UTF8 -Raw | ConvertFrom-Json
    if ($parameters.major -notin @(15, 16, 17, 18) -or $parameters.architecture -notin @('x86', 'x64', 'ARM64') -or
        $parameters.operation -notin @('install', 'enable')) { throw '远程部署参数无效。' }
    $nativeArchitecture = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'ARM64' }
                          elseif ([Environment]::Is64BitOperatingSystem) { 'x64' } else { 'x86' }
    if ($parameters.architecture -ne $nativeArchitecture) {
        throw "目标电脑为 $nativeArchitecture，请选择相同架构的 Remote Tools。"
    }
    if ($parameters.startup_user -isnot [string] -or [string]::IsNullOrWhiteSpace($parameters.startup_user) -or
        $parameters.startup_user.Length -gt 256 -or $parameters.startup_user -match '[\x00-\x1f]' -or
        $parameters.stop_legacy_startup -isnot [bool]) {
        throw '远端桌面账号或迁移选项无效。'
    }
    $startupUser = $parameters.startup_user.Trim()
    if ($startupUser.StartsWith('.\')) {
        $startupUser = $env:COMPUTERNAME + '\' + $startupUser.Substring(2)
    } elseif ($startupUser -notmatch '[\\@]') {
        $startupUser = $env:COMPUTERNAME + '\' + $startupUser
    }
    try {
        $account = New-Object System.Security.Principal.NTAccount($startupUser)
        $startupSid = $account.Translate([System.Security.Principal.SecurityIdentifier]).Value
    } catch {
        throw "远端无法识别桌面账号 $startupUser，请填写实际登录 Windows 的账号。"
    }
    if ($startupSid -in @('S-1-5-18', 'S-1-5-19', 'S-1-5-20') -or $startupSid -like 'S-1-5-32-*') {
        throw '请选择实际登录 Windows 的用户账号。'
    }
    $service = New-Object -ComObject 'Schedule.Service'
    $service.Connect()
    $root = $service.GetFolder('\')
    $existing = $null
    $legacyStartup = $false
    try { $existing = $root.GetTask($startupName) }
    catch { if (($_.Exception.GetBaseException().HResult -band 0xffff) -ne 2) { throw } }
    if ($null -ne $existing) {
        $oldDefinition = $existing.Definition
        if ($oldDefinition.RegistrationInfo.Author -ne 'RemoteReleaseSync' -or $oldDefinition.Actions.Count -ne 1 -or
            $oldDefinition.Actions.Item(1).Type -ne 0 -or (Split-Path -Leaf $oldDefinition.Actions.Item(1).Path) -ine 'msvsmon.exe') {
            throw '同名自启任务不属于本程序，已停止操作。'
        }
        $legacyStartup = $oldDefinition.Principal.LogonType -eq 5 -and
            $oldDefinition.Principal.UserId -in @('S-1-5-18', 'SYSTEM', 'NT AUTHORITY\SYSTEM') -and
            $oldDefinition.Triggers.Count -eq 1 -and $oldDefinition.Triggers.Item(1).Type -eq 8
        Assert-StartupTaskCanBeUpdated $existing $legacyStartup $parameters.stop_legacy_startup
    }

    if ($parameters.operation -eq 'install') {
        Set-Phase '静默安装'
        $installer = Join-Path $PSScriptRoot 'RemoteTools.exe'
        if ($parameters.sha256 -notmatch '^[0-9A-Fa-f]{64}$' -or
            (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash -ne $parameters.sha256) { throw '复制后的安装包摘要不一致。' }
        $signature = Get-AuthenticodeSignature -LiteralPath $installer
        if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -ne 'Microsoft Corporation') {
            throw '远程安装包未通过微软数字签名检查。'
        }
        $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($installer)
        if ($info.ProductMajorPart -ne $parameters.major -or ($info.ProductName + ' ' + $info.FileDescription) -notmatch 'Remote|远程') {
            throw '远程安装包版本或产品信息不匹配。'
        }
        $log = Join-Path $PSScriptRoot 'install.log'
        $process = Start-Process -FilePath $installer -ArgumentList ('/install /quiet /norestart /log "' + $log + '"') -Wait -PassThru -WindowStyle Hidden
        if ($null -eq $process) { throw '安装器没有返回有效进程。' }
        $result.installer_exit = $process.ExitCode
        $result.installer_log = $log
        if ($process.ExitCode -notin @(0, 3010)) {
            throw ('安装器失败，返回码：' + $process.ExitCode + '。未配置登录自启。')
        }
        $reboot = $process.ExitCode -eq 3010
    }

    Set-Phase $(if ($parameters.operation -eq 'install') { '安装后配置自启' } else { '配置自启' })
    $debugger = Find-Debugger ([int]$parameters.major) ([string]$parameters.architecture)
    $result.debugger = $debugger
    $result.debugger_version = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($debugger).FileVersion
    $firewallProfiles = @(Get-NetFirewallProfile -PolicyStore ActiveStore)
    $result.firewall_profiles = @($firewallProfiles | ForEach-Object { @{ name = $_.Name; enabled = [bool]$_.Enabled } })
    # 只放行本程序调试器的必要端口，限制为域/专用网络及本地子网，不关闭 Windows 防火墙。
    $debuggerRoot = Split-Path -Parent (Split-Path -Parent $debugger)
    foreach ($arch in @('x86', 'x64', 'ARM64')) {
        $program = Join-Path $debuggerRoot "$arch\msvsmon.exe"
        if (-not (Test-Path -LiteralPath $program -PathType Leaf)) { continue }
        $ruleName = 'RemoteReleaseSync_Debugger_' + $arch
        $oldRule = Get-NetFirewallRule -Name $ruleName -ErrorAction SilentlyContinue
        if ($null -ne $oldRule) {
            if ($oldRule.Group -ne 'RemoteReleaseSync') { throw '存在不属于本程序的同名防火墙规则。' }
            Remove-NetFirewallRule -Name $ruleName
        }
        New-NetFirewallRule -Name $ruleName -DisplayName "RemoteReleaseSync 远程调试器 $arch" -Group 'RemoteReleaseSync' -Direction Inbound -Action Allow -Program $program -Protocol TCP -LocalPort 4025,4026 -Profile Domain,Private -RemoteAddress LocalSubnet | Out-Null
    }

    $task = $service.NewTask(0)
    $task.RegistrationInfo.Author = 'RemoteReleaseSync'
    $task.RegistrationInfo.Description = '微软远程调试器：指定用户登录时在桌面打开，无认证模式，仅限可信局域网。'
    $task.Principal.UserId = $startupSid
    $task.Principal.LogonType = 3
    $task.Principal.RunLevel = 0
    $task.Settings.Enabled = $true
    $task.Settings.ExecutionTimeLimit = 'PT0S'
    $task.Settings.DisallowStartIfOnBatteries = $false
    $task.Settings.StopIfGoingOnBatteries = $false
    $task.Settings.MultipleInstances = 2
    $trigger = $task.Triggers.Create(9)
    $trigger.UserId = $startupSid
    $action = $task.Actions.Create(0)
    $action.Path = $debugger
    $action.Arguments = '/noauth /anyuser /nosecuritywarn /port 4026 /timeout 999999999'
    $action.WorkingDirectory = Split-Path -Parent $debugger
    if ($null -ne $existing) {
        Assert-StartupTaskCanBeUpdated $existing $legacyStartup $parameters.stop_legacy_startup
        if ($legacyStartup) {
            # 先校验新任务定义，再禁用旧触发器，避免停止旧实例后被计划任务重新拉起。
            $null = $root.RegisterTaskDefinition($startupName, $task, 1, $startupSid, $null, 3)
            Assert-StartupTaskCanBeUpdated $existing $legacyStartup $parameters.stop_legacy_startup
            $existing.Enabled = $false
            if ($existing.Enabled) { throw '无法关闭旧后台自启。' }
            $result.legacy_startup_disabled = $true
            if (Test-StartupTaskActive $existing) {
                Assert-StartupTaskCanBeUpdated $existing $legacyStartup $parameters.stop_legacy_startup
                $existing.Stop(0)
                for ($attempt = 0; $attempt -lt 20 -and (Test-StartupTaskActive $existing); ++$attempt) {
                    Start-Sleep -Milliseconds 250
                }
                if (Test-StartupTaskActive $existing) { throw '旧后台调试器尚未退出，请在远端检查任务后重试。' }
            }
        }
    }
    $registrationFlags = if ($null -eq $existing) { 2 } else { 4 }
    $registered = $root.RegisterTaskDefinition($startupName, $task, $registrationFlags, $startupSid, $null, 3)
    if ($null -eq $registered -or -not $registered.Enabled -or $registered.Definition.Principal.LogonType -ne 3 -or
        $registered.Definition.Triggers.Count -ne 1 -or $registered.Definition.Triggers.Item(1).Type -ne 9) {
        throw '登录自启任务未正确注册或启用。'
    }
    $result.success = $true
    $result.message = "已为 $startupUser 配置登录自启。该账号下次登录远端 Windows 时会打开调试器窗口，可在窗口中确认 TCP 4026。"
    if ($parameters.operation -eq 'install') { $result.message = '静默安装成功。' + $result.message }
    if (Get-NetTCPConnection -LocalPort 4026 -State Listen -ErrorAction SilentlyContinue) {
        $result.message += ' 当前 TCP 4026 仍被占用，请在远端检查占用进程。'
    }
    if ($reboot) { $result.message += ' 安装器提示需要重启，请选择合适时间自行重启。' }
    if (@($firewallProfiles | Where-Object { -not [bool]$_.Enabled }).Count -gt 0) {
        $result.message += ' 注意：目标主机存在已关闭的防火墙配置，新增规则无法在关闭状态下限制连接来源。请仅在可信测试网络使用。'
    }
    $profiles = @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)
    if (@($profiles | Where-Object NetworkCategory -eq 'Public').Count -gt 0) {
        $result.message += ' 注意：存在公用网络；本程序防火墙规则仅放行域/专用网络的本地子网。'
    }
} catch {
    $result.success = $false
    $result.message = $phase + '失败：' + $_.Exception.Message
    $result.error_code = $_.Exception.GetBaseException().HResult
    if ($result.legacy_startup_disabled) { $result.message += ' 旧后台自启已关闭。' }
    if ($reboot) { $result.message += ' 安装器提示需要重启。' }
} finally {
    $result.phase = $phase
    $result.elapsed_ms = $elapsed.ElapsedMilliseconds
    $result.time = [DateTime]::UtcNow.ToString('o')
    $utf8 = New-Object System.Text.UTF8Encoding($true)
    $temporary = Join-Path $PSScriptRoot 'result.partial'
    [System.IO.File]::WriteAllText($temporary, ($result | ConvertTo-Json -Compress), $utf8)
    Move-Item -LiteralPath $temporary -Destination (Join-Path $PSScriptRoot 'result.json') -Force
}
