$ErrorActionPreference = 'Stop'
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$request = $null
$scheduler = $null
$folder = $null
$job = $null
$drive = $null
$stage = $null
$runRequested = $false
$keepStage = $false
$remoteContactStarted = $false
$jobName = 'RemoteReleaseSync_Deployment'
$startupName = 'RemoteReleaseSync_RemoteDebugger'

function Send-Status([string]$type, [string]$message, [bool]$success = $false, [object]$details = $null) {
    [Console]::WriteLine((@{ type = $type; message = $message; success = $success; details = $details;
        time = [DateTime]::UtcNow.ToString('o'); remote_contact = $remoteContactStarted;
        run = if ($null -ne $request) { $request.run } else { '' } } | ConvertTo-Json -Compress -Depth 8))
}

function Get-OwnedTask([string]$name) {
    try { $item = $folder.GetTask($name) }
    catch {
        if (($_.Exception.GetBaseException().HResult -band 0xffff) -eq 2) { return $null }
        throw
    }
    if ($item.Definition.RegistrationInfo.Author -ne 'RemoteReleaseSync') {
        throw "同名任务 $name 不属于本程序，已停止操作。"
    }
    return $item
}

function Remove-Stage([string]$path) {
    $pattern = '^' + [regex]::Escape($driveName) + ':\\Temp\\(RemoteReleaseSync-[0-9a-f]{32})$'
    if ($path -notmatch $pattern) { throw '临时目录不在本次管理员共享范围内。' }
    $leaf = $Matches[1]
    if (-not (Test-Path -LiteralPath $path)) { return }
    $item = Get-Item -LiteralPath $path -Force
    $expected = [System.IO.Path]::GetFullPath(('\\' + $request.server + '\ADMIN$\Temp\' + $leaf))
    $actual = [System.IO.Path]::GetFullPath($item.FullName)
    if ($actual -ine $expected -or -not $item.PSIsContainer -or ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint)) {
        throw '临时目录的绝对路径或类型异常，已停止递归清理。'
    }
    Remove-Item -LiteralPath $path -Recurse -Force
}

try {
    $request = [Console]::In.ReadToEnd() | ConvertFrom-Json
    if ($null -eq $request -or $request.operation -notin @('install', 'enable', 'disable')) { throw '远程操作参数无效。' }
    if ($request.server -notmatch '^[a-zA-Z0-9_.-]{1,253}$' -or $request.server.Contains('..')) { throw '主机名或 IPv4 地址无效。' }
    if ([string]::IsNullOrWhiteSpace($request.username)) { throw '请在远程主机编辑中保存管理员账号。' }
    if ($request.major -notin @(15, 16, 17, 18) -or $request.architecture -notin @('x86', 'x64', 'ARM64')) { throw '版本或架构无效。' }
    if ($request.operation -ne 'disable' -and ($request.startup_user -isnot [string] -or
        [string]::IsNullOrWhiteSpace($request.startup_user) -or $request.startup_user.Length -gt 256 -or
        $request.startup_user -match '[\x00-\x1f]' -or $request.stop_legacy_startup -isnot [bool])) {
        throw '远端桌面账号或迁移选项无效。'
    }

    $account = [string]$request.username
    $user = $account
    $domain = [string]$request.server
    if ($account.Contains('\')) {
        $parts = $account.Split('\', 2)
        $domain = if ($parts[0] -eq '.') { [string]$request.server } else { $parts[0] }
        $user = $parts[1]
    } elseif ($account.Contains('@')) { $domain = '' }
    $remoteContactStarted = $true
    Send-Status 'progress' '正在使用已保存凭据连接远程任务计划…'
    $scheduler = New-Object -ComObject 'Schedule.Service'
    $scheduler.Connect([string]$request.server, $user, $domain, [string]$request.password)
    if (-not $scheduler.Connected) { throw '无法建立远程任务计划连接。' }
    $folder = $scheduler.GetFolder('\')

    if ($request.operation -eq 'disable') {
        $startup = Get-OwnedTask $startupName
        if ($null -eq $startup) { Send-Status 'result' '未配置本程序管理的调试器自启，无需关闭。' $true }
        else {
            $startup.Enabled = $false
            if ($startup.Enabled) { throw '关闭自启后状态回读不一致。' }
            Send-Status 'result' '已关闭调试器自启。当前运行的调试器继续运行。' $true
        }
        return
    }

    $hash = ''
    if ($request.operation -eq 'install') {
        Send-Status 'progress' '正在检查本机安装包的微软签名、工具版本和文件摘要…'
        $package = Get-Item -LiteralPath ([string]$request.installer)
        if ($package.PSIsContainer -or $package.Extension -ne '.exe' -or $package.Length -le 0 -or $package.Length -gt 268435456) {
            throw '请选择有效的微软 Remote Tools EXE 安装包（最大 256 MiB）。'
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $package.FullName
        if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.GetNameInfo([System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false) -ne 'Microsoft Corporation') {
            throw '安装包未通过 Microsoft Corporation 数字签名校验。'
        }
        $description = $package.VersionInfo.ProductName + ' ' + $package.VersionInfo.FileDescription
        if ($description -notmatch 'Visual Studio' -or $description -notmatch 'Remote|远程' -or $package.VersionInfo.ProductMajorPart -ne $request.major) {
            throw '安装包不是所选版本的微软 Remote Tools，请重新选择版本或安装包。'
        }
        $hash = (Get-FileHash -LiteralPath $package.FullName -Algorithm SHA256).Hash
        Send-Status 'progress' '本机安装包检查通过。' $true @{ path = $package.FullName; size = $package.Length; sha256 = $hash; version = $package.VersionInfo.ProductVersion }
    }

    $secret = if ([string]::IsNullOrEmpty($request.password)) { New-Object System.Security.SecureString }
              else { ConvertTo-SecureString -String ([string]$request.password) -AsPlainText -Force }
    $credentialAccount = if ([string]::IsNullOrEmpty($domain)) { $user } else { $domain + '\' + $user }
    $credential = New-Object System.Management.Automation.PSCredential($credentialAccount, $secret)
    Send-Status 'progress' '正在检查管理员共享和远程任务创建权限…'
    $driveName = 'rrs' + [guid]::NewGuid().ToString('N').Substring(0, 8)
    # 只创建本次操作自己的临时盘，不断开用户或同步任务已有的共享连接。
    $drive = New-PSDrive -Name $driveName -PSProvider FileSystem -Root ('\\' + $request.server + '\ADMIN$') -Credential $credential -Scope Script

    $previous = Get-OwnedTask $jobName
    if ($null -ne $previous) {
        if ($previous.State -in @(2, 4) -or $previous.GetInstances(0).Count -gt 0) { throw '该主机已有远程部署任务正在运行，请等待完成，不要重复安装。' }
        $oldDirectory = [string]$previous.Definition.Actions.Item(1).WorkingDirectory
        if ($oldDirectory -notmatch '^%SystemRoot%\\Temp\\(RemoteReleaseSync-[0-9a-f]{32})$') {
            throw '上次部署任务路径异常，已停止清理。'
        }
        $oldStage = Join-Path ($driveName + ':\Temp') $Matches[1]
        if (-not (Test-Path -LiteralPath (Join-Path $oldStage 'result.json'))) {
            # 从未运行的任务可能属于另一个仍在复制文件的客户端，不能当成已结束的遗留任务清理。
            if ($previous.LastRunTime.Year -lt 2000 -or $previous.LastTaskResult -in @(267009, 267011)) {
                throw '该主机正在准备部署，或上次部署未完整结束。尚未确认完成，不会清理临时任务；请稍后重试或检查 RemoteReleaseSync_Deployment。'
            }
            Send-Status 'progress' '上次任务已退出但没有写入结果，本次先清理其临时文件。' $false @{ task_exit = $previous.LastTaskResult; last_run = $previous.LastRunTime.ToString('o') }
        }
        Remove-Stage $oldStage
        $folder.DeleteTask($jobName, 0)
        Send-Status 'result' '上次远程部署任务已经结束，已清理其临时文件。请再次点击需要的操作。' $false
        return
    }

    $stageName = 'RemoteReleaseSync-' + [guid]::NewGuid().ToString('N')
    $stage = Join-Path ($driveName + ':\Temp') $stageName
    $remoteDirectory = '%SystemRoot%\Temp\' + $stageName
    $definition = $scheduler.NewTask(0)
    $definition.RegistrationInfo.Author = 'RemoteReleaseSync'
    $definition.RegistrationInfo.Description = 'RemoteReleaseSync 临时部署任务，完成后自动清理。'
    $definition.Principal.UserId = 'S-1-5-18'
    $definition.Principal.LogonType = 5
    $definition.Principal.RunLevel = 1
    $definition.Settings.ExecutionTimeLimit = 'PT0S'
    $definition.Settings.Enabled = $true
    $definition.Settings.AllowDemandStart = $true
    $definition.Settings.DisallowStartIfOnBatteries = $false
    $definition.Settings.StopIfGoingOnBatteries = $false
    $action = $definition.Actions.Create(0)
    $action.Path = '%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe'
    $action.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "' + $remoteDirectory + '\deploy.ps1"'
    $action.WorkingDirectory = $remoteDirectory
    # TASK_CREATE 防止并发客户端覆盖正在准备的部署；注册成功才说明有 SYSTEM 任务创建权限。
    $job = $folder.RegisterTaskDefinition($jobName, $definition, 2, 'S-1-5-18', $null, 5)
    if ($null -eq $job) { throw '远程任务注册没有返回有效结果。' }
    Send-Status 'progress' '远程 SYSTEM 任务创建权限检查通过。' $true @{ task = $jobName; stage = $remoteDirectory }
    New-Item -ItemType Directory -Path $stage | Out-Null
    $acl = New-Object System.Security.AccessControl.DirectorySecurity
    $acl.SetAccessRuleProtection($true, $false)
    foreach ($sidText in @('S-1-5-18', 'S-1-5-32-544')) {
        $sid = New-Object System.Security.Principal.SecurityIdentifier($sidText)
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule($sid, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
        $acl.AddAccessRule($rule)
    }
    $stageUnc = '\\' + $request.server + '\ADMIN$\Temp\' + $stageName
    # PowerShell 5.1 的 Set-Acl 会错误重组 UNC 临时盘路径，直接使用原生 UNC 路径设置目录权限。
    [System.IO.Directory]::SetAccessControl($stageUnc, $acl)
    $utf8 = New-Object System.Text.UTF8Encoding($true)
    $bootstrap = [System.Text.Encoding]::UTF8.GetString([Convert]::FromBase64String([string]$request.bootstrap)).TrimStart([char]0xfeff)
    [System.IO.File]::WriteAllText((Join-Path $stageUnc 'deploy.ps1'), $bootstrap, $utf8)
    $parameters = @{ operation = $request.operation; major = $request.major; architecture = $request.architecture
        sha256 = $hash; run = $request.run; startup_user = $request.startup_user; stop_legacy_startup = $request.stop_legacy_startup }
    [System.IO.File]::WriteAllText((Join-Path $stageUnc 'parameters.json'), ($parameters | ConvertTo-Json -Compress), $utf8)
    if ($request.operation -eq 'install') {
        Send-Status 'progress' '权限检查通过，正在向远程管理员临时目录复制安装包…'
        Copy-Item -LiteralPath $package.FullName -Destination (Join-Path $stage 'RemoteTools.exe')
    }
    Send-Status 'progress' '正在远程执行，请等待配置结果…'
    $runRequested = $true
    $running = $job.Run($null)
    if ($null -eq $running) { throw '远程任务未返回运行实例，执行状态未知。' }
    $resultPath = Join-Path $stage 'result.json'
    $deadline = [DateTime]::UtcNow.AddMinutes(20)
    $started = [DateTime]::UtcNow
    $lastProgress = ''
    while ([DateTime]::UtcNow -lt $deadline) {
        $progressPath = Join-Path $stage 'progress.json'
        if (Test-Path -LiteralPath $progressPath) {
            try {
                $progressText = Get-Content -LiteralPath $progressPath -Encoding UTF8 -Raw
                if ($progressText -ne $lastProgress) {
                    $progress = $progressText | ConvertFrom-Json
                    Send-Status 'progress' ([string]$progress.message) $true $progress
                    $lastProgress = $progressText
                }
            } catch { }
        }
        if (Test-Path -LiteralPath $resultPath) {
            $result = Get-Content -LiteralPath $resultPath -Encoding UTF8 -Raw | ConvertFrom-Json
            if ($null -eq $result -or $result.success -isnot [bool] -or $result.message -isnot [string] -or [string]::IsNullOrWhiteSpace($result.message)) {
                throw '远程结果文件无效，请检查主机状态。'
            }
            if (-not $result.success -and $result.legacy_stop_confirmation_required -ne $true) {
                $keepStage = $true
                $result.message += ' 已保留临时诊断目录：' + $stageUnc
            }
            Send-Status 'result' ([string]$result.message) ([bool]$result.success) $result
            return
        }
        if (([DateTime]::UtcNow - $started).TotalSeconds -gt 15 -and $job.State -notin @(2, 4)) {
            throw ('远程任务已退出但未写入结果，任务返回码：0x{0:X8}。请检查安装器或主机策略。' -f [uint32]$job.LastTaskResult)
        }
        Start-Sleep -Milliseconds 750
    }
    throw '等待超过 20 分钟，远程任务可能仍在运行。已保留任务及临时文件，请勿重复安装。'
} catch {
    $errorRecord = $_
    $failure = $errorRecord.Exception.GetBaseException()
    if ($runRequested) { $keepStage = $true }
    $message = if ($null -eq $request) { '无法读取有效的远程操作参数。' } else { $failure.Message }
    if ($null -ne $request -and -not [string]::IsNullOrEmpty($request.password)) { $message = $message.Replace([string]$request.password, '[已隐藏]') }
    $hint = switch ($failure.HResult -band 0xffff) {
        5 { ' 请检查账号的管理员权限，以及远程主机的 UAC 限制。' }
        1326 { ' 请检查已保存的账号、密码及账号所属主机或域。' }
        53 { ' 请检查主机名解析、ADMIN$ 共享和文件共享防火墙规则。' }
        1722 { ' 请检查远程任务计划服务及其防火墙规则。' }
        default { '' }
    }
    if ($keepStage -and $stageUnc) { $message += ' 已保留临时诊断目录：' + $stageUnc }
    $details = @{ error_code = $failure.HResult; script_line = $errorRecord.InvocationInfo.ScriptLineNumber;
        command = $errorRecord.InvocationInfo.MyCommand.Name; script_stack = $errorRecord.ScriptStackTrace; stage = $stageUnc; kept_stage = $keepStage }
    if ($null -ne $job) {
        try { $details.task_state = $job.State; $details.task_exit = $job.LastTaskResult }
        catch { $details.task_query_error = $_.Exception.GetBaseException().HResult }
    }
    Send-Status 'result' ('远程操作失败：' + $message + $hint) $false $details
} finally {
    if ($null -ne $job) {
        $safeToClean = -not $runRequested
        if ($runRequested) {
            try {
                for ($cleanupAttempt = 0; $cleanupAttempt -lt 10 -and ($job.State -in @(2, 4) -or $job.GetInstances(0).Count -gt 0); ++$cleanupAttempt) {
                    Start-Sleep -Milliseconds 500
                }
                $safeToClean = $job.State -notin @(2, 4) -and $job.GetInstances(0).Count -eq 0
                if ($safeToClean) { Send-Status 'progress' '远程部署任务已退出。' $true @{ task_exit = $job.LastTaskResult; state = $job.State } }
            } catch { $safeToClean = $false }
        }
        if ($safeToClean -and -not $keepStage) {
            try {
                if ($null -ne $stage) { Remove-Stage $stage }
                $folder.DeleteTask($jobName, 0)
            } catch { Send-Status 'progress' '部署结果已返回；临时任务或文件暂未清理，下次操作时可重新检查。' }
        }
    }
    if ($null -ne $drive) { Remove-PSDrive -Name $drive.Name -ErrorAction SilentlyContinue }
    $request = $null
    $credential = $null
    $secret = $null
}
