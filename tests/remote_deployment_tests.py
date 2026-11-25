import hashlib
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
import uuid


ROOT = Path(__file__).resolve().parents[1]
POWERSHELL = Path(os.environ.get('SystemRoot', 'C:/Windows')) / 'System32/WindowsPowerShell/v1.0/powershell.exe'


@unittest.skipUnless(os.name == 'nt' and POWERSHELL.is_file(), 'Requires Windows PowerShell')
class RemoteDeploymentTests(unittest.TestCase):
    def setUp(self):
        base = Path(os.environ.get('SYNC_QA_DIR', tempfile.gettempdir())).resolve()
        base.mkdir(parents=True, exist_ok=True)
        self.temporary = tempfile.TemporaryDirectory(prefix='remote-negative-', dir=base)
        self.workspace = Path(self.temporary.name).resolve()
        self.assertEqual(self.workspace.parent, base)

    def tearDown(self):
        self.temporary.cleanup()

    def run_script(self, script, data=None):
        result = subprocess.run(
            [str(POWERSHELL), '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', str(script)],
            input=data, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30,
            env=dict(os.environ, SYNC_SOURCE_ROOT=str(ROOT)),
        )
        self.assertEqual(result.returncode, 0, result.stderr.decode('utf-8', errors='backslashreplace'))
        self.assertEqual(result.stderr, b'', result.stderr.decode('utf-8', errors='backslashreplace'))
        return result.stdout

    def helper_request(self, changes=None):
        request = dict(operation='install', server='QA-HOST', username='qa-admin', password='NeverLogThis-QA-Password',
                       major=17, architecture='x64', installer='', bootstrap='', run='qa-negative',
                       startup_user='qa-desktop', stop_legacy_startup=False)
        request.update(changes or {})
        output = self.run_script(ROOT / 'resources/remote-deploy.ps1', json.dumps(request).encode('utf-8'))
        self.assertNotIn(request['password'].encode('utf-8'), output)
        records = [json.loads(line) for line in output.decode('utf-8-sig').splitlines() if line.strip()]
        finals = [record for record in records if record['type'] == 'result']
        self.assertEqual(len(finals), 1)
        self.assertFalse(finals[0]['success'])
        return finals[0]['message']

    def bootstrap_request(self, parameters, package=None):
        script = self.workspace / 'deploy.ps1'
        shutil.copyfile(ROOT / 'resources/remote-bootstrap.ps1', script)
        staged = dict(startup_user=os.environ['USERDOMAIN'] + '\\' + os.environ['USERNAME'], stop_legacy_startup=False)
        staged.update(parameters)
        (self.workspace / 'parameters.json').write_text(json.dumps(staged), encoding='utf-8-sig')
        if package is not None:
            (self.workspace / 'RemoteTools.exe').write_bytes(package)
        self.run_script(script)
        result = json.loads((self.workspace / 'result.json').read_text(encoding='utf-8-sig'))
        self.assertFalse(result['success'])
        self.assertTrue(result['phase'])
        self.assertGreaterEqual(result['elapsed_ms'], 0)
        self.assertTrue(result['time'].endswith('Z'))
        return result['message']

    def local_helper(self, job_name, startup_name):
        text = (ROOT / 'resources/remote-deploy.ps1').read_text(encoding='utf-8-sig')
        replacements = {
            "$jobName = 'RemoteReleaseSync_Deployment'": "$jobName = '" + job_name + "'",
            "$startupName = 'RemoteReleaseSync_RemoteDebugger'": "$startupName = '" + startup_name + "'",
            '$scheduler.Connect([string]$request.server, $user, $domain, [string]$request.password)': '$scheduler.Connect()',
            '-Credential $credential -Scope Script': '-Scope Script',
        }
        for before, after in replacements.items():
            self.assertEqual(text.count(before), 1)
            text = text.replace(before, after, 1)
        script = self.workspace / 'local-helper.ps1'
        script.write_text(text, encoding='utf-8-sig')
        return script

    def cleanup_task(self, name):
        self.assertTrue(name.startswith('RemoteReleaseSync_QA_'))
        script = self.workspace / 'cleanup-task.ps1'
        script.write_text(r'''
$ErrorActionPreference = 'Stop'
$service = New-Object -ComObject 'Schedule.Service'; $service.Connect(); $root = $service.GetFolder('\')
$name = $env:SYNC_QA_TASK_NAME
if ($name -notmatch '^RemoteReleaseSync_QA_[A-Za-z]+_[0-9a-f]{32}$') { throw 'Unexpected QA task name.' }
$item = $null
try { $item = $root.GetTask($name) } catch { if (($_.Exception.GetBaseException().HResult -band 0xffff) -ne 2) { throw } }
if ($null -ne $item) {
    if ($item.Definition.RegistrationInfo.Author -ne 'RemoteReleaseSync') { throw 'Unexpected QA task owner.' }
    if ($item.State -in @(2,4)) { $item.Stop(0); Start-Sleep -Milliseconds 500 }
    if ($item.State -in @(2,4)) { throw 'QA task is still running.' }
    $directory = [string]$item.Definition.Actions.Item(1).WorkingDirectory
    if ($directory -match '^%SystemRoot%\\Temp\\(RemoteReleaseSync-[0-9a-f]{32})$') {
        $path = Join-Path (Join-Path $env:SystemRoot 'Temp') $Matches[1]
        $absolute = [IO.Path]::GetFullPath($path)
        $expectedParent = [IO.Path]::GetFullPath((Join-Path $env:SystemRoot 'Temp'))
        if ([IO.Path]::GetDirectoryName($absolute) -ine $expectedParent) { throw 'Unexpected cleanup path.' }
        if (Test-Path -LiteralPath $absolute) {
            $folder = Get-Item -LiteralPath $absolute -Force
            if ($folder.FullName -ine $absolute -or ($folder.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected QA folder type.' }
            Remove-Item -LiteralPath $absolute -Recurse -Force
        }
    }
    $root.DeleteTask($name,0)
}
''', encoding='utf-8-sig')
        old = os.environ.get('SYNC_QA_TASK_NAME')
        os.environ['SYNC_QA_TASK_NAME'] = name
        try:
            self.run_script(script)
        finally:
            if old is None:
                os.environ.pop('SYNC_QA_TASK_NAME', None)
            else:
                os.environ['SYNC_QA_TASK_NAME'] = old

    def test_invalid_requests_are_rejected_without_connecting(self):
        for changes in ({'operation': 'unknown'}, {'server': '../other'}, {'username': ''}, {'major': 99},
                        {'startup_user': ''}, {'stop_legacy_startup': 'false'}):
            with self.subTest(changes=changes):
                self.assertTrue(self.helper_request(changes).startswith('远程操作失败：'))

    def test_malformed_json_does_not_echo_the_input(self):
        output = self.run_script(ROOT / 'resources/remote-deploy.ps1', b'{"password":"NeverEchoMalformed",')
        self.assertNotIn(b'NeverEchoMalformed', output)
        record = json.loads(output.decode('utf-8-sig'))
        self.assertFalse(record['success'])
        self.assertIn('无法读取', record['message'])

    def test_unknown_architecture_is_rejected(self):
        message = self.bootstrap_request(dict(operation='install', major=17, architecture='unknown', sha256='0' * 64))
        self.assertIn('参数无效', message)

    def test_target_architecture_mismatch_is_rejected(self):
        architecture = 'x86' if os.environ.get('PROCESSOR_ARCHITECTURE', '').upper() in ('AMD64', 'ARM64') else 'x64'
        message = self.bootstrap_request(dict(operation='install', major=17, architecture=architecture, sha256='0' * 64))
        self.assertIn('请选择相同架构', message)

    def test_corrupt_transfer_is_rejected_before_installer_execution(self):
        architecture = 'ARM64' if os.environ.get('PROCESSOR_ARCHITECTURE', '').upper() == 'ARM64' else 'x64'
        message = self.bootstrap_request(dict(operation='install', major=17, architecture=architecture, sha256='0' * 64), b'QA unsigned fixture')
        self.assertIn('摘要不一致', message)

    def test_unsigned_package_is_rejected_before_installer_execution(self):
        package = b'QA unsigned fixture, never executable'
        architecture = 'ARM64' if os.environ.get('PROCESSOR_ARCHITECTURE', '').upper() == 'ARM64' else 'x64'
        message = self.bootstrap_request(dict(operation='install', major=17, architecture=architecture,
                                              sha256=hashlib.sha256(package).hexdigest()), package)
        self.assertIn('微软数字签名', message)

    @unittest.skipUnless(os.environ.get('SYNC_QA_DEBUGGER_ROOT'), 'Requires an existing signed Microsoft debugger')
    def test_existing_signed_debugger_is_located_without_running_it(self):
        script = self.workspace / 'find-signed-debugger.ps1'
        script.write_text(r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$source = Get-Content -LiteralPath (Join-Path $env:SYNC_SOURCE_ROOT 'resources/remote-bootstrap.ps1') -Raw -Encoding UTF8
$tokens = $null; $errors = $null
$ast = [Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
if ($errors.Count) { throw 'Production bootstrap script has syntax errors.' }
$function = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Find-Debugger' }, $true)
if ($null -eq $function) { throw 'Find-Debugger definition is missing.' }
Invoke-Expression $function.Extent.Text
$leaf = 'RemoteReleaseSync-QA-' + [guid]::NewGuid().ToString('N')
$key = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\' + $leaf
$expectedName = 'HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\' + $leaf
$created = $false
try {
    New-Item -Path $key | Out-Null
    $created = $true
    New-ItemProperty -LiteralPath $key -Name DisplayName -Value 'Microsoft Remote Tools QA lookup fixture' -PropertyType String | Out-Null
    New-ItemProperty -LiteralPath $key -Name InstallLocation -Value $env:SYNC_QA_DEBUGGER_ROOT -PropertyType String | Out-Null
    $path = Find-Debugger 17 'x64'
    [Console]::WriteLine((@{ path = $path; original = [Diagnostics.FileVersionInfo]::GetVersionInfo($path).OriginalFilename } | ConvertTo-Json -Compress))
} finally {
    if ($created) {
        $item = Get-Item -LiteralPath $key
        if ($item.Name -ine $expectedName) { throw 'Refusing to remove an unexpected QA registry key.' }
        Remove-Item -LiteralPath $key -Force
    }
}
''', encoding='utf-8-sig')
        result = json.loads(self.run_script(script).decode('utf-8-sig'))
        self.assertEqual(result['original'].lower(), 'msvsmon.exe')
        self.assertTrue(Path(result['path']).is_file())

    @unittest.skipUnless(os.environ.get('SYNC_QA_DEBUGGER_ROOT'), 'Requires an existing signed Microsoft debugger')
    def test_windows_validates_login_task_without_registering_or_running(self):
        script = self.workspace / 'validate-login-task.ps1'
        script.write_text(r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$source = Get-Content -LiteralPath (Join-Path $env:SYNC_SOURCE_ROOT 'resources/remote-bootstrap.ps1') -Raw -Encoding UTF8
$begin = $source.IndexOf('$task = $service.NewTask(0)')
$end = $source.IndexOf('if ($null -ne $existing)', $begin)
if ($begin -lt 0 -or $end -le $begin) { throw 'Production task definition block is missing.' }
$definition = $source.Substring($begin, $end - $begin)
if ($definition -match 'RegisterTaskDefinition|\.Run\s*\(') {
    throw 'Refusing to execute a task definition outside validation-only mode.'
}
$service = New-Object -ComObject 'Schedule.Service'
$service.Connect()
$root = $service.GetFolder('\')
$startupName = 'RemoteReleaseSync_QA_Validate_' + [guid]::NewGuid().ToString('N')
$startupSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$debugger = Join-Path $env:SYNC_QA_DEBUGGER_ROOT 'Common7\IDE\Remote Debugger\x64\msvsmon.exe'
Invoke-Expression $definition
$null = $root.RegisterTaskDefinition($startupName, $task, 1, $startupSid, $null, 3)
$persisted = $false
try { $unexpected = $root.GetTask($startupName); $persisted = $true }
catch { if (($_.Exception.GetBaseException().HResult -band 0xffff) -ne 2) { throw } }
if ($persisted) { $root.DeleteTask($startupName, 0) }
[Console]::WriteLine((@{ accepted = $true; persisted = $persisted; xml = $task.XmlText;
    user_sid = $startupSid; principal_user = $task.Principal.UserId; logon_type = $task.Principal.LogonType } | ConvertTo-Json -Compress))
''', encoding='utf-8-sig')
        result = json.loads(self.run_script(script).decode('utf-8-sig'))
        self.assertTrue(result['accepted'])
        self.assertFalse(result['persisted'])
        document = ET.fromstring(result['xml'])
        ns = {'t': 'http://schemas.microsoft.com/windows/2004/02/mit/task'}
        self.assertIsNone(document.find('t:Triggers/t:BootTrigger', ns))
        self.assertEqual(document.findtext('t:Triggers/t:LogonTrigger/t:UserId', namespaces=ns), result['user_sid'])
        self.assertEqual(result['principal_user'], result['user_sid'])
        self.assertEqual(result['logon_type'], 3)
        self.assertIsNone(document.find('t:Settings/t:RestartOnFailure', ns))
        arguments = document.findtext('t:Actions/t:Exec/t:Arguments', namespaces=ns)
        self.assertIn('/noauth /anyuser', arguments)
        self.assertNotIn('/silent', arguments)

    @unittest.skipUnless(os.environ.get('SYNC_QA_SYSTEM_TASKS') == '1', 'Requires explicit local SYSTEM-task QA mode')
    def test_disable_updates_only_the_owned_local_task(self):
        name = 'RemoteReleaseSync_QA_Disable_' + uuid.uuid4().hex
        creator = self.workspace / 'create-disabled-fixture.ps1'
        creator.write_text(r'''
$ErrorActionPreference='Stop'
$service=New-Object -ComObject 'Schedule.Service'; $service.Connect(); $root=$service.GetFolder('\')
$definition=$service.NewTask(0); $definition.RegistrationInfo.Author='RemoteReleaseSync'
$definition.Principal.UserId='S-1-5-18'; $definition.Principal.LogonType=5
$action=$definition.Actions.Create(0); $action.Path=Join-Path $env:SystemRoot 'System32\cmd.exe'; $action.Arguments='/c exit 0'
$definition.Settings.Enabled=$true
$item=$root.RegisterTaskDefinition($env:SYNC_QA_TASK_NAME,$definition,2,'S-1-5-18',$null,5)
if ($null -eq $item -or -not $item.Enabled) { throw 'Unable to create an enabled QA task.' }
''', encoding='utf-8-sig')
        os.environ['SYNC_QA_TASK_NAME'] = name
        try:
            self.run_script(creator)
            helper = self.local_helper('RemoteReleaseSync_QA_Job_' + uuid.uuid4().hex, name)
            request = dict(operation='disable', server=os.environ['COMPUTERNAME'], username='qa-current-session',
                           password='Unused-QA-Secret', major=17, architecture='x64', run='qa-disable')
            output = self.run_script(helper, json.dumps(request).encode('utf-8'))
            result = [json.loads(line) for line in output.decode('utf-8-sig').splitlines() if line.strip()]
            self.assertTrue(next(item for item in result if item['type'] == 'result')['success'])
            verifier = self.workspace / 'verify-disabled.ps1'
            verifier.write_text(r'''
$service=New-Object -ComObject 'Schedule.Service'; $service.Connect()
$item=$service.GetFolder('\').GetTask($env:SYNC_QA_TASK_NAME)
if ($item.Enabled -or $item.State -eq 4) { throw 'QA task was not disabled or was unexpectedly run.' }
''', encoding='utf-8-sig')
            self.run_script(verifier)
        finally:
            self.cleanup_task(name)
            os.environ.pop('SYNC_QA_TASK_NAME', None)

    @unittest.skipUnless(os.environ.get('SYNC_QA_SYSTEM_TASKS') == '1' and os.environ.get('SYNC_QA_REMOTE_PACKAGE'),
                         'Requires explicit local SYSTEM-task QA mode and a signed Microsoft package')
    def test_admin_share_transport_runs_safe_receipt_as_system_and_cleans_up(self):
        job = 'RemoteReleaseSync_QA_Transport_' + uuid.uuid4().hex
        helper = self.local_helper(job, 'RemoteReleaseSync_QA_Unused_' + uuid.uuid4().hex)
        receipt = r'''
$ErrorActionPreference='Stop'
$parameters=Get-Content -LiteralPath (Join-Path $PSScriptRoot 'parameters.json') -Raw -Encoding UTF8 | ConvertFrom-Json
$identity=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$acl=Get-Acl -LiteralPath $PSScriptRoot
$sids=@($acl.GetAccessRules($true,$true,[Security.Principal.SecurityIdentifier]) | ForEach-Object { $_.IdentityReference.Value })
$protected=$acl.AreAccessRulesProtected -and @($sids | Where-Object { $_ -notin @('S-1-5-18','S-1-5-32-544') }).Count -eq 0
$hash=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'RemoteTools.exe') -Algorithm SHA256).Hash
$result=@{ success=($identity -eq 'S-1-5-18' -and $protected -and $hash -eq $parameters.sha256);
    message='QA 系统回执'; identity=$identity; protected=$protected; hash=$hash; stage=$PSScriptRoot;
    startup_user=$parameters.startup_user; stop_legacy_startup=$parameters.stop_legacy_startup }
$file=Join-Path $PSScriptRoot 'result.partial'
[IO.File]::WriteAllText($file,($result | ConvertTo-Json -Compress),(New-Object Text.UTF8Encoding($true)))
Move-Item -LiteralPath $file -Destination (Join-Path $PSScriptRoot 'result.json') -Force
'''
        request = dict(operation='install', server=os.environ['COMPUTERNAME'], username='qa-current-session',
                       password='Unused-QA-Secret', major=18, architecture='x64', run='qa-transport',
                       startup_user='QA-Desktop', stop_legacy_startup=False,
                       installer=os.environ['SYNC_QA_REMOTE_PACKAGE'], bootstrap=base64.b64encode(receipt.encode('utf-8')).decode('ascii'))
        try:
            output = self.run_script(helper, json.dumps(request).encode('utf-8'))
            self.assertNotIn(b'Unused-QA-Secret', output)
            records = [json.loads(line) for line in output.decode('utf-8-sig').splitlines() if line.strip()]
            final = next(item for item in records if item['type'] == 'result')
            self.assertTrue(final['success'], json.dumps(final, ensure_ascii=False))
            self.assertEqual(final['details']['identity'], 'S-1-5-18')
            self.assertTrue(final['details']['protected'])
            self.assertEqual(final['details']['startup_user'], request['startup_user'])
            self.assertIs(final['details']['stop_legacy_startup'], False)
            self.assertEqual(final['details']['hash'].lower(), hashlib.sha256(Path(request['installer']).read_bytes()).hexdigest())
            stage = Path(final['details']['stage']).resolve()
            self.assertEqual(stage.parent, (Path(os.environ['SystemRoot']) / 'Temp').resolve())
            self.assertFalse(stage.exists(), 'The deployment stage was not cleaned up.')
        finally:
            self.cleanup_task(job)

    @unittest.skipUnless(os.environ.get('SYNC_QA_SYSTEM_TASKS') == '1' and os.environ.get('SYNC_QA_REMOTE_PACKAGE'),
                         'Requires explicit local SYSTEM-task QA mode and a signed Microsoft package')
    def test_crashed_worker_keeps_diagnostics_and_next_request_recovers(self):
        job = 'RemoteReleaseSync_QA_Crash_' + uuid.uuid4().hex
        helper = self.local_helper(job, 'RemoteReleaseSync_QA_Unused_' + uuid.uuid4().hex)
        request = dict(operation='install', server=os.environ['COMPUTERNAME'], username='qa-current-session',
                       password='Unused-QA-Secret', major=18, architecture='x64', run='qa-worker-crash',
                       startup_user='QA-Desktop', stop_legacy_startup=False,
                       installer=os.environ['SYNC_QA_REMOTE_PACKAGE'], bootstrap=base64.b64encode(b'exit 19').decode('ascii'))
        try:
            output = self.run_script(helper, json.dumps(request).encode('utf-8'))
            records = [json.loads(line) for line in output.decode('utf-8-sig').splitlines() if line.strip()]
            final = next(item for item in records if item['type'] == 'result')
            self.assertFalse(final['success'])
            self.assertTrue(final['details']['kept_stage'])
            self.assertEqual(final['details']['task_exit'], 19)
            output = self.run_script(helper, json.dumps(request).encode('utf-8'))
            records = [json.loads(line) for line in output.decode('utf-8-sig').splitlines() if line.strip()]
            final = next(item for item in records if item['type'] == 'result')
            self.assertFalse(final['success'])
            self.assertIn('已清理其临时文件', final['message'])
        finally:
            self.cleanup_task(job)


if __name__ == '__main__':
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
    unittest.main(testRunner=unittest.TextTestRunner(stream=sys.stdout, verbosity=2))
