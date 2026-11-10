; 远程 Release 同步工具安装程序（Inno Setup 6.3 及以上）。
; 由 tools\package-release.ps1 调用：ISCC.exe /DAppVersion=x.y.z /DSourceDir=<运行包目录> /DOutputDir=<输出目录> RemoteReleaseSync.iss
; 程序内更新会通过 /DIR 指定当前程序所在目录，并通过 /RELAUNCH=show|hidden 指定安装后的启动方式。

#ifndef AppVersion
  #error "AppVersion is required."
#endif
#ifndef SourceDir
  #error "SourceDir is required."
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

#define AppName "远程 Release 同步工具"
#define AppExeName "RemoteReleaseSync.exe"

[Setup]
AppId={{DBF10DBE-3058-4595-9931-BB0C219D0137}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=LocalTools
AppPublisherURL=https://github.com/UUxxXxxUU/SyncTool
AppSupportURL=https://github.com/UUxxXxxUU/SyncTool/releases
AppUpdatesURL=https://github.com/UUxxXxxUU/SyncTool/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\RemoteReleaseSync
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
DirExistsWarning=no
UsePreviousAppDir=yes
; 默认按当前用户安装，避免写入 Program Files 时无法保存 sync_config.json；需要时可用 /ALLUSERS 提升权限。
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=commandline dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; 程序由自身的互斥量检测是否退出，不使用重启管理器强制关闭。
CloseApplications=no
RestartApplications=no
SetupIconFile=..\resources\app.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
OutputDir={#OutputDir}
OutputBaseFilename=RemoteReleaseSync-{#AppVersion}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Languages]
#if FileExists(AddBackslash(CompilerPath) + "Languages\ChineseSimplified.isl")
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"
#else
Name: "english"; MessagesFile: "compiler:Default.isl"
#endif

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; GroupDescription: "附加任务："; Flags: unchecked

[InstallDelete]
; 安装到旧版程序目录时先移除旧版程序文件，保留 sync_config.json 等用户数据。
Type: files; Name: "{app}\{#AppExeName}"; Check: IsExistingProgramDir
Type: files; Name: "{app}\*.dll"; Check: IsExistingProgramDir
Type: files; Name: "{app}\LICENSE"; Check: IsExistingProgramDir
Type: files; Name: "{app}\README.md"; Check: IsExistingProgramDir
Type: filesandordirs; Name: "{app}\bearer"; Check: IsExistingProgramDir
Type: filesandordirs; Name: "{app}\iconengines"; Check: IsExistingProgramDir
Type: filesandordirs; Name: "{app}\imageformats"; Check: IsExistingProgramDir
Type: filesandordirs; Name: "{app}\platforms"; Check: IsExistingProgramDir
Type: filesandordirs; Name: "{app}\styles"; Check: IsExistingProgramDir
Type: filesandordirs; Name: "{app}\translations"; Check: IsExistingProgramDir

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Excludes: "sync_config.json"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExeName}"; Parameters: "--updated"; Check: IsRelaunchMode('show'); Flags: nowait runasoriginaluser
Filename: "{app}\{#AppExeName}"; Parameters: "--startup --updated"; Check: IsRelaunchMode('hidden'); Flags: nowait runasoriginaluser
Filename: "{app}\{#AppExeName}"; Description: "启动 {#AppName}"; Check: IsRelaunchMode(''); Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
const
  RunningMutex = 'LocalTools.RemoteReleaseSync.Running';
  UninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{DBF10DBE-3058-4595-9931-BB0C219D0137}_is1';

var
  ExistingProgramDir: Boolean;

function RelaunchMode(): String;
begin
  Result := Lowercase(ExpandConstant('{param:RELAUNCH|}'));
end;

function IsRelaunchMode(Mode: String): Boolean;
begin
  Result := RelaunchMode() = Mode;
end;

function IsExistingProgramDir(): Boolean;
begin
  Result := ExistingProgramDir;
end;

function InitializeSetup(): Boolean;
var
  Attempts: Integer;
begin
  Result := True;
  // 由程序内更新启动时，等待原程序停止后台任务并退出。
  if RelaunchMode() <> '' then
  begin
    Attempts := 0;
    while CheckForMutexes(RunningMutex) and (Attempts < 240) do
    begin
      Sleep(500);
      Attempts := Attempts + 1;
    end;
  end;
  while CheckForMutexes(RunningMutex) do
  begin
    if SuppressibleMsgBox('远程 Release 同步工具正在运行，请从托盘菜单退出程序后点击“重试”。',
      mbError, MB_RETRYCANCEL, IDCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// 卸载登记在其他目录的旧版安装，避免同一程序残留多份。
procedure RemovePreviousInstallation();
var
  Location: String;
  Uninstaller: String;
  ResultCode: Integer;
begin
  if not RegQueryStringValue(HKA, UninstallKey, 'InstallLocation', Location) then
    Exit;
  if CompareText(RemoveBackslashUnlessRoot(Location), RemoveBackslashUnlessRoot(ExpandConstant('{app}'))) = 0 then
    Exit;
  if not RegQueryStringValue(HKA, UninstallKey, 'UninstallString', Uninstaller) then
    Exit;
  Uninstaller := RemoveQuotes(Uninstaller);
  if FileExists(Uninstaller) then
    Exec(Uninstaller, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
  begin
    ExistingProgramDir := FileExists(ExpandConstant('{app}\{#AppExeName}'));
    RemovePreviousInstallation();
  end;
end;
