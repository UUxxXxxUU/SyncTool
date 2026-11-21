# RemoteReleaseSync 1.1.2 回归记录

验证日期：2026-11-21。环境：Windows 10 19044 x64、Qt 5.12.3 msvc2017_64、v142、Windows SDK 10.0.26100.0。

## 结果

| 范围 | 实际结果 |
| --- | --- |
| CMake Debug / Release 构建 | 应用与测试程序均通过 |
| Visual Studio 工程 Release 打包构建 | 通过；第三方窗口组件存在未使用参数警告，无编译错误 |
| Debug 常规 CTest | 40 通过、0 失败、2 跳过 |
| Release 常规 CTest | 40 通过、0 失败、2 跳过 |
| 本机调试器发现、路径打开与 Windows 文件剪贴板复制 | Debug、Release 分别以独立进程补跑，通过 |
| 微软 Remote Tools 真实下载 | Debug、Release 均通过；验证保存到系统“下载”、文件大小、签名校验与最终进度，删除测试下载文件 |
| 150% 原生界面回归 | 6 通过、0 失败、0 跳过，含进度边界、中文示例、更新窗口重开、最大化/还原 |
| 最终便携包启动 | 移除 Qt PATH 与插件路径后通过；5 个 Qt 模块从包内加载，崩溃采集 DLL 加载成功 |
| 最终安装载荷回归 | 隔离 AppId、名称与目录下安装、覆盖安装、配置保留及卸载通过；正式 1.1.1 登记和程序摘要未变 |
| 发行包完整性 | ZIP / EXE 的 SHA256 与校验文件一致；37 个 PE 文件的非系统运行库依赖检查通过 |
| 编码和界面 | 100% / 150% 截图中中文示例、标题栏、状态列和下载大小显示正常；UTF-8、BOM、换行及空白检查通过 |

常规回归中的两项跳过是需要显式环境标志的系统集成用例：`SYNC_QA_DEBUGGER_INSTALLED` 与 `SYNC_QA_REMOTE_DOWNLOAD`，均已分开验证。更新窗口重开使用受控的待下载状态注入，覆盖已知大小、未知大小、主界面提示和连续重开；不访问或安装线上生产更新。

测试覆盖原有同步、规则、排除、取消、配置保存、通知、真实 UNC 管理共享、PDB 锁定警告及新下载进度逻辑。UNC 数据位于本机临时目录，通过 `localhost` 管理共享访问。

## 运行记录与限制

- Qt 5.12.3 的 Windows 消息框在 `offscreen` 后端访问原生接口时会异常；Windows CTest 已改用 `windows` 后端，并固定保存文本和 xUnit 日志。
- 一次含网络下载的 CTest 启动超过超时；以显式日志直接运行后，Debug / Release 全套真实下载回归均通过。
- 剪贴板集成在整套进程中曾遇到系统占用 `0x800401d0`；拆为独立进程后 Debug / Release 均通过，没有修改原复制业务。
- 桌面自动化运行时返回 Windows `os error 216`。界面由原生 Qt 测试保存截图，并检查文字、布局边界、圆角像素和最大化/还原状态。
- 未在 Windows 11 实机或全新 Windows 安装上验收；不将去 Qt PATH 启动和隔离安装描述为全新系统验证。
- 圆角方案不提供原生贴边分屏和系统阴影。

本机详细日志和截图位于 `artifacts/qa-1.1.2-20261121-1d6043ef/`，不作为发布附件上传。测试用独立 AppId 安装器也不上传。

## 正式附件

| 文件 | SHA256 |
| --- | --- |
| `RemoteReleaseSync-1.1.2-windows-x64.zip` | `3a1f6c0887122043ad7c6e7777f70f7fef60aae54ec01920ab34e7af2439fff3` |
| `RemoteReleaseSync-1.1.2-windows-x64-setup.exe` | `0c223c816f9d54a6f4fc82c71b9eef8ab5363d1f731ef29fdec8f8658e58926e` |

OpenSSL 二进制沿用 1.1.1 的 1.0.2u x64，SHA256 分别为 `e2908dec495cc6e621358eb7c5d41403f25eb4bdbf3802866eadea378422d412` 与 `2250edf8968f04eefc1b10502a7a6f5a70461127e892c9f8cdb460d16065b01b`。Qt、OpenSSL 和既有依赖未升级或降级。
