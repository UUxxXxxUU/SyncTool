$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$encoding = New-Object System.Text.UTF8Encoding($false)
[Console]::InputEncoding = $encoding
[Console]::OutputEncoding = $encoding
$OutputEncoding = $encoding
$scriptPath = [Environment]::GetEnvironmentVariable('RRS_DEPLOY_SCRIPT')
$run = [Environment]::GetEnvironmentVariable('RRS_DEPLOY_RUN')
$invoked = $false
try {
    if ([string]::IsNullOrWhiteSpace($scriptPath)) { throw '本机部署脚本路径为空。' }
    # 先按 PowerShell 的只读共享方式确认文件可读取，启动错误也能通过 UTF-8 协议返回。
    $stream = [System.IO.File]::Open($scriptPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    $stream.Dispose()
    [Console]::WriteLine((@{ type = 'progress'; phase = 'local_ready'; message = '本机部署脚本已准备，正在检查参数…';
        remote_contact = $false; run = $run; time = [DateTime]::UtcNow.ToString('o') } | ConvertTo-Json -Compress))
    $invoked = $true
    & $scriptPath
} catch {
    $failure = $_.Exception.GetBaseException()
    $message = if ($invoked) { '本机部署脚本执行异常，请按阶段日志检查执行状态：' } else { '本机部署脚本启动失败，未连接远程主机：' }
    [Console]::WriteLine((@{ type = 'result'; phase = if ($invoked) { 'controller_failure' } else { 'local_load' };
        success = $false; message = $message + $failure.Message; run = $run; time = [DateTime]::UtcNow.ToString('o');
        details = @{ error_code = $failure.HResult; script_line = $_.InvocationInfo.ScriptLineNumber; script = $scriptPath } } | ConvertTo-Json -Compress -Depth 5))
    exit 1
}
