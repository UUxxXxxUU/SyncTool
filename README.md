# 远程 Release 同步工具

这是一个基于 `Qt Widgets` 的 Windows 桌面工具，用来把本地 Release 目录中的指定类型文件，以及相对路径规则选中的资源文件，自动同步到多台远程主机共享目录。支持 Visual Studio、CMake 和 qmake 构建。

## 已实现功能

- 可配置本地 `release` 目录
- 可新增、更新、删除远程主机
- 每台主机支持保存共享目录、账号、密码、是否启用
- 可选择同步类型，例如 `dll`、`exe`、`pdb`
- 附加同步规则支持 `./image`、`../Lang`、多级相对路径和单个文件，可独立筛选类型或同步全部文件
- 主窗口与自定义弹窗使用 `.ui` 布局，可在 Qt Designer 中编辑
- 支持手动“立即同步”
- 支持轮询本地目录变化后自动同步
- 每台主机都有独立进度条和状态颜色
- 同一轮实际复制完成后，在屏幕上方居中显示 3 秒半透明提示；多台主机合并显示，有失败时列出对应主机。托盘运行时同样有效，提示允许鼠标穿透并保持键盘焦点。
- “设置”菜单提供自动同步快捷开关、偏好设置和保存配置；偏好设置集中管理同步、启动和更新选项，保存后生效，取消不改变设置
- “帮助”菜单提供使用技巧（F1）和检查更新；底部显示自动同步状态与扫描间隔，仅发现新版本时显示可点击的更新提示
- 远程完全一致显示绿色
- 不一致按差异程度显示不同深浅红色
- 共享不可达显示断线灰色
- 配置自动保存到程序目录下的 `sync_config.json`
- 支持手动检查 GitHub 上的新版本，并打开发行版下载页面

## 构建方式

请确保本机已安装 Qt 5.12.3，并使用和 Qt 安装包匹配的 MSVC Kit。

例如如果你的 Qt 是 `msvc2017_64`，那就要用对应的 MSVC 2017 工具链，或者在更高版本 VS 中安装 `v141` 工具集后再编译。

### 方式一：Qt Creator / qmake

```powershell
qmake RemoteReleaseSync.pro
```

然后用 Qt Creator 打开 `RemoteReleaseSync.pro` 编译，或者在已经初始化好的 MSVC 开发者命令行里继续执行对应构建命令。

### 方式二：CMake

如果你的环境里有 `cmake`，也可以使用：

```powershell
cmake -S . -B build
cmake --build build --config Release
```

生成的可执行文件名通常为：

```text
RemoteReleaseSync.exe
```

### Windows 打包与草稿 Release 上传

`tools/package-release.ps1` 使用现有 Visual Studio 项目编译 `Release|x64`，收集运行依赖，生成 ZIP 运行包、EXE 安装程序及各自的 SHA256 校验文件；加 `-Upload` 后自动上传到 GitHub 草稿 Release。

准备环境：

- Windows PowerShell 5.1 或更高版本。
- Qt 5.12.3 的 `msvc2017_64` Kit，包含 `qmake.exe`、`windeployqt.exe`；脚本不更改依赖版本。
- Visual Studio 2022 C++ 工具、v142 工具集、Windows 10/11 SDK、CMake，以及 Qt VS Tools 提供的 Qt/MSBuild。Crashpad 使用 Visual Studio 2022 的 C++ 工具构建。
- 与当前 Qt 兼容的 OpenSSL 1.0.x x64 DLL：`libeay32.dll`、`ssleay32.dll`。文件名中的 `32` 不代表 DLL 是 32 位；脚本会检查 x64 架构。此 Qt Kit 使用 OpenSSL 1.0 接口，不能直接换成 OpenSSL 1.1 或 3；由使用者提供来源可信且依赖齐全的 DLL。
- Inno Setup 6.3 或更高版本（`winget install JRSoftware.InnoSetup`），用于生成安装程序；脚本自动查找 `ISCC.exe`，也可用 `-InnoSetupDir` 指定目录。
- 上传时还需 Git、GitHub CLI，以及对 `UUxxXxxUU/SyncTool` 的写入权限。

从项目根目录打包，替换示例中的 OpenSSL 目录：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -OpenSslDir "C:\Deps\openssl-1.0-x64\bin"
```

Qt 默认从 PATH 中的 `qmake.exe` 定位，Visual Studio 自动选择带 v142 的安装，Qt/MSBuild 优先使用 `QtMsBuild` 环境变量，否则使用 `%LOCALAPPDATA%\QtMsBuild`。需要指定工具目录时：

```powershell
.\tools\package-release.ps1 `
    -QtDir "C:\Qt\Qt5.12.3\5.12.3\msvc2017_64" `
    -OpenSslDir "C:\Deps\openssl-1.0-x64\bin" `
    -VisualStudioDir "C:\Program Files\Microsoft Visual Studio\2022\Enterprise" `
    -QtMsBuildDir "$env:LOCALAPPDATA\QtMsBuild"
```

每次打包在已被 Git 忽略的 `artifacts/release-版本-时间-随机标识/` 下新建目录，将工程及其引用的源码复制到其中构建，保留原工程的 moc 和对象文件路径，不修改现有工程设置。输出 `RemoteReleaseSync-版本-windows-x64.zip` 及同名 `.zip.sha256`。脚本只从新的部署目录收集文件，包含 Qt DLL、平台插件、v142 运行库 DLL、OpenSSL DLL、README 和 LICENSE，不包含个人 `sync_config.json` 或编译中间文件。Windows 10/11 x64 系统提供 Universal CRT；OpenSSL DLL 若另有运行依赖，需另行核实。

脚本不自动收集 OpenSSL 的额外运行依赖。例如本地测试使用的 OpenSSL 1.0.1g x64 DLL 依赖 `MSVCR100.dll`，目标电脑还需 Visual C++ 2010 x64 运行库；Universal CRT 和 v142 运行库不能替代它。正式分发前需补齐所选 DLL 的依赖并在干净电脑验证。

脚本会加入 Crashpad、spdlog 和 fmt 许可。通过 `-RuntimeNoticesDir` 提供所部署 Qt 及其第三方组件、OpenSSL 的许可证和版权声明。如果该目录的同级存在 `THIRD-PARTY-NOTICES.md`，也会加入运行包，可在此文件记录对应 Qt 和 OpenSSL 的源码获取信息。打包目录单独保留与 EXE 匹配的 `RemoteReleaseSync.pdb`，供维护人员分析 Dump。

安装 GitHub CLI 并首次登录：

```powershell
winget install --id GitHub.cli --exact
# 安装后重新打开终端。
gh auth login --hostname github.com
```

提交并推送版本对应的源码，确认工作区干净且本地 HEAD 与 GitHub `master` 一致，再执行打包并上传：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tools\package-release.ps1 -OpenSslDir "C:\Deps\openssl-1.0-x64\bin" -Upload
```

脚本核对 `src/main.cpp`、`CMakeLists.txt`、`update.json` 的版本，使用 `v版本` 标签、本次提交和 `update.json` 中的更新说明创建草稿，并上传 ZIP、SHA256 文件。已有同名 Release 会报错，已有标签必须指向同一提交；不会覆盖附件或正式发布。若上传途中失败，保留本地产物，GitHub 可能已留下草稿，需在网页检查后处理再重试。

在未安装 Qt 的 Windows 10/11 x64 电脑中，将 ZIP 解压到有写入权限的目录，检查软件启动、HTTPS 更新检测及断网时 Ctrl 连续点击共享目录的行为。确认后打开 [GitHub Releases](https://github.com/UUxxXxxUU/SyncTool/releases)，编辑草稿并点击发布。草稿只有有权限的用户可见；公开发布完成后再更新线上 `update.json`。

## 使用说明

1. 在“Release 目录”里选择本地编译输出目录。
2. 在“同步类型”里勾选需要参与同步的扩展名。
3. 在“远程主机配置”里输入共享目录、账号、密码并新增主机。
4. 勾选主机表格中的“启用”即可参与同步。
5. 点击“刷新状态”可以只检查一致性，不执行复制。
   后台周期检查会复用可用的共享连接，结果不变时保留上次完成状态和进度；实际复制、差异变化、断线和错误正常更新。首次检查和手动刷新显示检查过程，只有调用建立连接接口时才显示“正在连接共享目录”。
6. 点击“立即同步”会把勾选主机与所选类型执行同步。
   在「设置 → 偏好设置 → 快捷键」的「同步」分组中，可分别为「手动同步」和「停止所有同步」录入组合键，保存后生效。停止快捷键和主界面的停止按钮执行同一操作，会取消当前及排队任务，并关闭自动同步。「仅主窗口」在本程序主窗口中生效，「全局」也可在其他程序或托盘中触发。发生冲突时仍可保存，已有绑定优先。Windows 占用的全局组合键会标为不可用，可在占用解除后重新保存设置。「触发快捷键时显示提示」默认开启。
7. 在“设置”菜单启用自动同步后，程序按扫描间隔检测本地变化并同步；点击“停止全部”会关闭自动同步。
8. 窗口标题显示当前软件版本。程序每 2 小时自动检查更新；“偏好设置”中的“启动时检查更新”默认关闭，启用后会在下次启动 3 秒后额外检查一次。有新版时，通过更新入口查看分类说明，选择“更新”在程序内下载并安装，也可前往发布页或下载历史版本。
9. “偏好设置”中的“无感更新”默认关闭。启用后，发现新版本会在后台下载并校验安装包，在没有同步/检查任务运行、没有弹窗且主窗口不在前台时，自动安装到当前程序所在目录并重启程序（原来缩在托盘的仍保持在托盘）。

### 下载进度与窗口外观（1.1.2 起）

在“工具”菜单打开远程调试器，选择版本和架构后点击“下载到‘下载’”，安装包保存到系统“下载”目录。下载区显示进度条、已下载与总大小、百分比和校验状态；校验通过后才显示完成。

软件更新的安装包和 SHA256 校验文件采用相同的大小显示方式。后台更新时，主界面更新入口显示当前进度；下载仍在继续时，重新打开更新窗口可恢复当前显示。服务器未提供总大小时显示动态进度条和已下载大小。历史版本由浏览器下载，使用浏览器自己的下载进度。

主窗口和普通弹窗采用统一小圆角及自定义标题栏，最大化时恢复直角。此方案不提供原生贴边分屏和系统阴影。

1.1.3 起按窗口所在显示器的工作区确定最大化范围，并保留还原时的窗口位置和尺寸。

## 附加同步规则（1.0.8 起）

Release 目录是两端路径的基准。例如本地 `D:\test sync\a\Release`、远端 `\\SERVER\b\Release`：

| 规则路径 | 文件类型 | 本地路径 | 远端路径 |
| --- | --- | --- | --- |
| `./image` | `png;jpg` | `a\Release\image` | `b\Release\image` |
| `./Lang` | `.qm` | `a\Release\Lang` | `b\Release\Lang` |
| `../Lang` | `.qm` | `a\Lang` | `b\Lang` |
| `../resources/theme` | 留空 | `a\resources\theme` | `b\resources\theme` |
| `../config.ini` | 留空 | `a\config.ini` | `b\config.ini` |

在“附加同步规则”Tab 中点击“添加规则”，填写相对路径及可选的文件类型。可浏览选择目录或单个文件，软件会自动转换为相对 Release 的路径。`png`、`.png` 和 `*.png` 都可填写，多种类型用分号分隔；留空表示全部文件，包括无扩展名文件。目录规则递归复制子目录中的文件，不单独创建空目录。

选择主机后可查看对应的目标路径预览。规则配置应用于所有启用主机，与基本同步一起执行；切换 Tab 不影响同步范围。小屏幕或高缩放下，上方配置区可滚动，下方主机状态仍保持可见。重叠规则按并集选择文件，同一文件只复制一次。取消全部基本同步类型后也可仅运行附加规则。切换 Release 目录后，规则以新的目录重新解析。

`../` 可以进入远端 Release 的上级，但不能超出共享根目录，例如 `\\SERVER\b`。路径不存在、文件与目录冲突、包含符号链接或目录联接时，会显示错误。远端多余文件保留，差异检查只覆盖选中的类型和规则范围。旧配置中没有 `syncRules` 时保持原来的同步位置和筛选；新配置请配合 1.0.8 或更新版本使用。

## 界面维护与验证

### 远程调试与诊断

- 在“工具 → 远程调试器”查看本机的 `msvsmon.exe` 版本、架构和路径。单击路径可在资源管理器中打开所在目录。“复制参数”可将启动参数复制到剪贴板。勾选后可复制路径文本，也可将完整架构目录粘贴到目标共享。来自不同版本的同名架构目录请分批粘贴。
- 选择远程电脑的 Visual Studio 版本及 Windows 架构后，可以后台下载微软 Remote Tools。通过签名校验的安装包会保存到 Windows 的“下载”目录，完成后会收到提示。历史安装程序可从微软官方文档获取。[Microsoft Learn 远程调试文档](https://learn.microsoft.com/zh-cn/visualstudio/debugger/remote-debugging)
- 密码输入框旁的小眼睛按住显示、松开隐藏；切换主机、隐藏窗口或失去焦点时恢复掩码。状态栏显示本次运行时长。
- 「远程主机编辑 → 扫描设备」扫描启用网卡的 IPv4 网段及已发现的 IPv6 邻居。结果可按网卡查看，支持主机名或 IP 搜索，并可筛选有主机名、文件共享端口可连接或已配置的设备。已配置设备有单独标识。双击主机名会打开共享根目录，未取得名称时使用 IP。点击「填入主机」会替换共享路径中的主机部分并保留目录后缀，再点「添加／更新」保存。
- 「扫描设备」旁的圆形帮助按钮提供 Windows 10、11 查看设备名称和使用 `hostname` 命令的图文说明。
- 在远程调试器窗口选择已保存主机、版本和架构，填写实际登录远端 Windows 的桌面账号，再点击“远程静默安装”。优先使用记录的下载路径或“下载”目录中匹配的安装包，找不到时弹框选择。程序复用保存的管理员凭据，通过 `ADMIN$` 和远程任务计划部署，检查微软签名、版本、复制摘要及目标 Windows 架构。需要管理员权限，以及文件共享和远程计划任务管理通道。
- 安装完成后为指定账号登记登录自启。该账号下次登录远端 Windows 时，调试器在桌面打开，可在窗口中退出并确认 TCP 4026。已安装工具可直接点击“开启自启”。“关闭自启”只关闭后续自启。旧后台任务仍在运行时，确认可以中断调试后，勾选“迁移时结束旧后台调试器”再配置。无认证模式仅适合可信局域网，程序添加的防火墙规则仅放行域／专用网络的本地子网。安装器提示重启时由用户安排。
- 远程部署时，密码通过子进程标准输入传递，不写入命令行或部署脚本。超时或异常退出不代表远程安装已经停止，请先检查远程任务状态；失败时保留管理员专用诊断目录，后续操作可清理已结束的部署任务。
- 本机启动器先以 UTF-8 初始化输入输出，再读取已关闭写句柄的部署脚本。错误信息和日志会区分本机脚本启动失败、远程连接以及执行状态未知；临时目录在子进程结束后清理。
- “排除文件”按文件名忽略大小写匹配，可用分号或换行分隔多个排除项。`*` 匹配零个或多个字符，例如 `log*.txt` 匹配 `log.txt` 和 `log01.txt`。`?` 匹配恰好一个字符，例如 `log?.txt` 匹配 `log1.txt`。排除名单同时应用于基本同步、附加规则、状态检查和自动扫描。点击“查看示例图”可查看排除后的同步结果。“反选类型”会反转基本类型的勾选状态。
- 复制结束后，会校验本轮计划复制的 PDB。目标 PDB 仍缺失、与本地有差异或无法完成校验时，会按主机提示。处理文件占用或共享故障后，可以手动同步重试。
- “工具 → 打开诊断目录”可查看 `Logs` 和 `CrashDumps`。日志由 spdlog 按 UTC 日期和大小滚动，默认保留 30 天，可在偏好设置设定。程序启动、修改保留设置及运行期间每小时检查一次。
- 详细日志记录按钮、菜单、选项切换、输入确认、表格操作、弹窗结果、窗口状态及业务流程。输入在确认或离焦时记录，高频进度按阶段采样；密码、令牌和已知凭据脱敏。日志包含主机名、IP、共享路径及账号名，请仅向可信维护人员提供。
- 每条记录带 UTC 时间、`session`、`seq`、`pid`、`thread`。按 `action` 查找操作来源，按 `run` 串联同步、扫描、下载或部署的阶段、文件、耗时和结果；`qt.message` 保留 Qt 警告与错误。每日日志按原有大小上限轮转，遇到问题后请及时保留 `Logs`，并说明发生时间和目标主机。
- 后台周期检查在结果不变时保持静默；无目标、无类型／规则和无需复制不会每轮重复写流水。手动操作及实际复制保留详细记录；同一主机、同一配置的重复异常每五分钟汇总次数，状态变化、恢复和退出时立即补记。新异常和配置变化会重新记录上下文。重复复制失败的诊断缓存有大小限制，持续较长的复制重试仍保留实时线索。
- 崩溃时由独立的 Crashpad 进程生成 SyncTool 的本地 minidump。部署包附带采集组件。手动构建的应用可用 `tools/build-crashpad.ps1` 构建它，再把输出的 `SyncToolCrashReporter.dll`、`crashpad_handler.exe` 和 `crashpad_wer.dll` 放在应用 EXE 同目录。使用匹配版本的应用 PDB 和 WinDbg 查看异常堆栈。[Crashpad 设计说明](https://chromium.googlesource.com/crashpad/crashpad/+/HEAD/doc/overview_design.md)

固定布局保存在 `src/mainwindow.ui`、`syncruledialog.ui`、`usagetipsdialog.ui`、`updatedialog.ui`、`recentcopypreview.ui`。请用 Qt Designer 编辑 `.ui`，不要编辑构建时生成的 `ui_*.h`。信号连接、主机行、进度条及按更新数据生成的分类内容仍在 C++ 中处理。

回归测试使用独立临时目录，覆盖真实 robocopy 复制、类型筛选、相对路径、去重、配置兼容、自动同步、取消和界面操作。Windows 的真实 UNC 测试使用当前账号访问本机管理共享，需具有相应权限：

```powershell
cmake -S . -B build -DSYNC_BUILD_TESTS=ON
cmake --build build --config Release
# Qt 的 bin 应在 PATH 中，plugins 可通过 QT_PLUGIN_PATH 指定。
ctest --test-dir build -C Release --output-on-failure
```

## 软件更新发布

每次启动后，程序都会开启独立的 2 小时更新检查定时器，首次在连续运行满 2 小时后检查。“启动时检查更新”选项默认关闭，旧配置未保存该选项时也保持关闭；选项会自动保存，勾选后在下次启动 3 秒后额外检查一次。运行中切换选项只影响下次启动，不立即检查，也不重置定时器；手动检查同样不重置 2 小时周期。

更新信息来自 GitHub 公开仓库 `UUxxXxxUU/SyncTool` 的 `master` 分支根目录 `update.json`，优先通过匿名 GitHub API 读取，失败时尝试 raw 地址，每个地址最多等待 10 秒：

```text
https://api.github.com/repos/UUxxXxxUU/SyncTool/contents/update.json?ref=master
https://raw.githubusercontent.com/UUxxXxxUU/SyncTool/master/update.json
```

自动检查不弹窗；发现新版后，按钮显示新版标记并缓慢变换背景颜色，文字保持清晰。点击按钮打开更新对话框：显示新版本号、“前往发布页”、按“修复问题 / 新增功能 / 优化改进”等分类折叠的更新说明、“查看更多”和“历史版本下载”链接、下载进度及“取消 / 更新”按钮。点击“更新”后程序下载安装包并校验 SHA256，随后退出并由安装程序更新当前程序所在目录，完成后自动重启。点击“取消”会在本次运行期间停止同一版本的呼吸提示，保留新版标记；发现更高版本时再次提示。窗口隐藏时暂停动画，重新显示时恢复。

两个地址都失败后，自动检查静默等待下个 2 小时周期，不额外安排短间隔重试；手动检查失败显示中文原因。未开启“无感更新”时，软件只在用户点击“更新”后下载安装包。

安装程序和程序内更新都会安装到指定目录（程序内更新固定为当前 EXE 所在目录）：先删除该目录中旧版的程序文件、Qt DLL 和插件目录，保留 `sync_config.json`；若之前用安装程序装在其他目录，会静默卸载那一份。程序目录需要管理员权限才能写入时，无感更新不会执行，手动更新会弹出 UAC 提示。需要旧版本时，可通过更新对话框中的“历史版本下载”进入 [GitHub Releases](https://github.com/UUxxXxxUU/SyncTool/releases) 下载。

`update.json` 格式如下，版本号使用三段数字（例如 `1.10.0`）。`version`、`notes`、`downloadUrl` 为必填字符串，旧版客户端只读取这三项；`downloadUrl` 填写发布列表页，用作历史版本下载入口。`releaseUrl`、`installerUrl`、`sections` 为 1.0.6 起支持的可选字段，缺省时分别使用 `v版本` 发布页和该版本的 `RemoteReleaseSync-版本-windows-x64-setup.exe` 下载地址；安装包同目录必须有同名 `.sha256` 文件，校验失败不会安装。

```json
{
  "version": "1.0.6",
  "notes": "本次更新的简要说明。",
  "downloadUrl": "https://github.com/UUxxXxxUU/SyncTool/releases",
  "releaseUrl": "https://github.com/UUxxXxxUU/SyncTool/releases/tag/v1.0.6",
  "installerUrl": "https://github.com/UUxxXxxUU/SyncTool/releases/download/v1.0.6/RemoteReleaseSync-1.0.6-windows-x64-setup.exe",
  "sections": [
    { "title": "修复问题", "items": ["修复说明一"] },
    { "title": "新增功能", "items": ["新增说明一"] },
    { "title": "优化改进", "items": ["优化说明一"] }
  ]
}
```

发布步骤：

1. 更新 `src/main.cpp` 中 `setApplicationVersion` 的版本号，并同步修改 CMake 项目版本。
2. 生成完整发布包，包含 EXE、Qt DLL、平台插件及与当前 Qt 配套的 HTTPS/TLS 运行库；新增功能需要 Qt Network 模块。不要打包个人 `sync_config.json`。
3. 在 GitHub 仓库的 Releases 页面（`https://github.com/UUxxXxxUU/SyncTool/releases`）创建对应版本的发行版，填写说明并上传 ZIP；确认使用者可以访问下载页面。
4. 发行包就绪后，将 `update.json` 的版本号、说明及 HTTPS 下载地址更新并推送到 GitHub 的 `master`。仅修改本地文件、Gitee 仓库或其他分支不会生效。
5. 1.0.6 及以后的版本可在程序内更新；更早的版本需要用户手动下载安装程序或 ZIP。使用 ZIP 时先通过托盘菜单“退出”关闭程序，再替换程序文件，保留自己的 `sync_config.json`。

没有更新检测入口或仍使用 Gitee 更新地址的旧版，需要先手动安装一次使用 GitHub 更新地址的版本。网络失败、超时或版本信息格式错误不会被视为“当前已是最新版本”，也不会清除已经发现的新版信息；手动检查时会提示“检查更新失败”。

## 状态说明

- `完全一致`：本地和远端在所选类型下完全一致
- `不一致`：存在缺失、过期或远端多余文件
- `断线`：共享目录不可达或账号密码无效
- `同步失败`：执行复制时出现错误

## 说明

- 当前版本不会自动删除远端多余文件，只会补齐或更新文件。
- 密码保存在 `sync_config.json` 中，为明文保存，建议只在可信环境中使用。
