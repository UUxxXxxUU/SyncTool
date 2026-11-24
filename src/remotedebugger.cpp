#pragma execution_character_set("utf-8")
#include "remotedebugger.h"
#include "diagnostics.h"
#include "downloadprogress.h"
#include "ui_remotedebugger.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeySequence>
#include <QMimeData>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QSslSocket>
#include <QSslError>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTextCodec>
#include <QTimer>
#include <QtEndian>
#include <QUuid>
#include <algorithm>

#ifdef Q_OS_WIN
#include <windows.h>
#include <softpub.h>
#include <wincrypt.h>
#include <wintrust.h>
#ifdef _MSC_VER
#pragma comment(lib, "Version.lib")
#pragma comment(lib, "Wintrust.lib")
#pragma comment(lib, "Crypt32.lib")
#endif
#endif

namespace
{
constexpr qint64 maxInstallerSize = 256 * 1024 * 1024;
constexpr int pathColumn = 3;

QString deploymentErrorText(const QByteArray &bytes)
{
    QTextCodec::ConverterState state;
    const QString utf8 = QTextCodec::codecForName("UTF-8")->toUnicode(bytes.constData(), bytes.size(), &state);
    if (!state.invalidChars) return utf8;
#ifdef Q_OS_WIN
    if (auto *codec = QTextCodec::codecForName("CP" + QByteArray::number(GetOEMCP()))) return codec->toUnicode(bytes);
#endif
    return QString::fromLocal8Bit(bytes);
}

struct Installation
{
    QString path;
    QString version;
    QString architecture;
};

QString debuggerArchitecture(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(0x3c)) return {};
    const QByteArray offset = file.read(4);
    if (offset.size() != 4 || !file.seek(qFromLittleEndian<quint32>(offset.constData()))) return {};
    const QByteArray signature = file.read(6);
    if (signature.size() != 6 || signature.left(4) != QByteArray("PE\0\0", 4)) return {};
    switch (qFromLittleEndian<quint16>(signature.constData() + 4)) {
    case 0x014c: return "x86";
    case 0x8664: return "x64";
    case 0xaa64: return "ARM64";
    case 0x01c4: return "ARM";
    default: return {};
    }
}

QString debuggerVersion(const QString &path)
{
#ifdef Q_OS_WIN
    const auto native = path.toStdWString();
    const DWORD size = GetFileVersionInfoSizeW(native.c_str(), nullptr);
    if (size > 0 && size <= 1024 * 1024) {
        QByteArray data(static_cast<int>(size), Qt::Uninitialized);
        void *root = nullptr;
        UINT rootSize = 0;
        if (GetFileVersionInfoW(native.c_str(), 0, size, data.data())
            && VerQueryValueW(data.data(), L"\\", &root, &rootSize) && rootSize >= sizeof(VS_FIXEDFILEINFO)) {
            const auto *version = static_cast<VS_FIXEDFILEINFO *>(root);
            return QString("%1.%2.%3.%4").arg(HIWORD(version->dwFileVersionMS)).arg(LOWORD(version->dwFileVersionMS))
                .arg(HIWORD(version->dwFileVersionLS)).arg(LOWORD(version->dwFileVersionLS));
        }
    }
#endif
    return QStringLiteral("未知");
}

void findDebuggers(const QDir &root, QSet<QString> *seen, QVector<Installation> *results)
{
    const QStringList subdirs{"", "x64", "x86", "arm64", "arm"};
    for (const QString &subdir : subdirs) {
        const QString file = subdir.isEmpty() ? root.filePath("msvsmon.exe")
                                             : root.filePath(subdir + "/msvsmon.exe");
        const QFileInfo info(file);
        if (!info.isFile()) continue;
        const QString path = info.canonicalFilePath();
        if (path.isEmpty() || seen->contains(path.toCaseFolded())) continue;
        const QString architecture = debuggerArchitecture(path);
        if (architecture.isEmpty()) continue;
        seen->insert(path.toCaseFolded());
        results->append({path, debuggerVersion(path), architecture});
    }
}

QStringList programRoots()
{
    QStringList paths;
    for (const char *name : {"ProgramW6432", "ProgramFiles", "ProgramFiles(x86)"}) {
        const QString path = qEnvironmentVariable(name);
        if (!path.isEmpty() && !paths.contains(path, Qt::CaseInsensitive)) paths.append(path);
    }
    return paths;
}

QStringList remoteToolsInstallations()
{
    QStringList paths;
#ifdef Q_OS_WIN
    for (const QString &hive : {QString("HKEY_LOCAL_MACHINE"), QString("HKEY_CURRENT_USER")}) {
        for (const auto format : {QSettings::Registry64Format, QSettings::Registry32Format}) {
            QSettings uninstall(hive + "\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall", format);
            for (const QString &key : uninstall.childGroups()) {
                uninstall.beginGroup(key);
                const QString name = uninstall.value("DisplayName").toString();
                if ((name.contains("Visual Studio", Qt::CaseInsensitive) &&
                     (name.contains("Remote", Qt::CaseInsensitive) || name.contains("远程")))
                    || name.contains("Microsoft Remote Tools", Qt::CaseInsensitive)) {
                    const QString path = uninstall.value("InstallLocation").toString();
                    if (!path.isEmpty()) paths.append(path);
                }
                uninstall.endGroup();
            }
        }
    }
#endif
    return paths;
}

bool microsoftUrl(const QUrl &url)
{
    return url.scheme() == "https" && url.userInfo().isEmpty() && url.port(443) == 443
        && (url.host() == "aka.ms" || url.host() == "download.visualstudio.microsoft.com"
            || url.host() == "download.microsoft.com");
}

QString microsoftSignatureError(const QString &path)
{
#ifdef Q_OS_WIN
    const auto native = QDir::toNativeSeparators(path).toStdWString();
    WINTRUST_FILE_INFO file = {};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = native.c_str();
    WINTRUST_DATA trust = {};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const auto noWindow = reinterpret_cast<HWND>(INVALID_HANDLE_VALUE);
    bool verified = false;
    const LONG status = WinVerifyTrust(noWindow, &policy, &trust);
    if (status == ERROR_SUCCESS) {
        auto *provider = WTHelperProvDataFromStateData(trust.hWVTStateData);
        auto *signer = provider ? WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0) : nullptr;
        auto *cert = signer ? WTHelperGetProvCertFromChain(signer, 0) : nullptr;
        if (cert) {
            wchar_t subject[256] = {};
            CertGetNameStringW(cert->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, subject, 256);
            verified = QString::fromWCharArray(subject) == "Microsoft Corporation";
        }
    }
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(noWindow, &policy, &trust);
    if (verified) return {};
    if (status == ERROR_SUCCESS) return QStringLiteral("安装包的签名发布者不是 Microsoft Corporation。");
    return QString("安装包未通过 Authenticode 校验（0x%1）。").arg(static_cast<quint32>(status), 8, 16, QLatin1Char('0'));
#else
    Q_UNUSED(path);
    return QStringLiteral("当前系统无法验证 Windows 安装包签名。");
#endif
}
}

RemoteDebugger::RemoteDebugger(QWidget *parent)
    : RoundedDialog(parent)
    , m_ui(std::make_unique<Ui::RemoteDebugger>())
    , m_network(new QNetworkAccessManager(this))
    , m_timeout(new QTimer(this))
{
    m_ui->setupUi(this);
    QScreen *screen = parent ? QGuiApplication::screenAt(parent->frameGeometry().center())
                             : QGuiApplication::primaryScreen();
    if (screen) {
        resize(size().boundedTo(screen->availableGeometry().size() - QSize(32, 48)));
    }
    for (int column : {0, 1, 2}) {
        m_ui->debuggerPaths->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_ui->debuggerPaths->horizontalHeader()->setSectionResizeMode(pathColumn, QHeaderView::Stretch);
    m_ui->debuggerPaths->setMinimumHeight(m_ui->debuggerPaths->horizontalHeader()->sizeHint().height()
        + m_ui->debuggerPaths->verticalHeader()->defaultSectionSize() * 4 + 4);
    m_ui->debuggerPaths->setMouseTracking(true);
    connect(m_ui->debuggerPaths, &QTableWidget::itemClicked, this, [this](QTableWidgetItem *item) {
        if (item->column() == pathColumn) openDebuggerFolder(item->row());
    });
    connect(m_ui->debuggerPaths, &QTableWidget::cellEntered, this, [this](int, int column) {
        auto *viewport = m_ui->debuggerPaths->viewport();
        if (column == pathColumn) viewport->setCursor(Qt::PointingHandCursor);
        else viewport->unsetCursor();
    });
    connect(m_ui->debuggerPaths, &QTableWidget::viewportEntered, this, [this]() {
        m_ui->debuggerPaths->viewport()->unsetCursor();
    });
    auto *openFolder = new QAction("打开所在目录", m_ui->debuggerPaths);
    openFolder->setShortcuts({QKeySequence(Qt::Key_Return), QKeySequence(Qt::Key_Enter)});
    openFolder->setShortcutContext(Qt::WidgetShortcut);
    m_ui->debuggerPaths->addAction(openFolder);
    connect(openFolder, &QAction::triggered, this, [this]() {
        openDebuggerFolder(m_ui->debuggerPaths->currentRow());
    });

    QFont commandFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    commandFont.setPointSizeF(m_ui->argumentsEdit->font().pointSizeF());
    m_ui->argumentsEdit->setFont(commandFont);
    m_ui->argumentsEdit->setText("/noauth /anyuser /nosecuritywarn /port 4026 /timeout 999999999");
    connect(m_ui->copyArgumentsButton, &QPushButton::clicked, this, [this]() {
        const QString arguments = m_ui->argumentsEdit->text();
        QClipboard *clipboard = QApplication::clipboard();
        clipboard->setText(arguments);
        m_ui->debuggerStatus->setText(clipboard->text() == arguments
            ? "已复制启动参数。" : "剪贴板暂时不可用，请重试。");
    });

    m_ui->debuggerVersion->addItem("Visual Studio 2026", 18);
    m_ui->debuggerVersion->addItem("Visual Studio 2022", 17);
    m_ui->debuggerVersion->addItem("Visual Studio 2019", 16);
    m_ui->debuggerVersion->addItem("Visual Studio 2017", 15);
    updateArchitectures();
    connect(m_ui->debuggerVersion, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &RemoteDebugger::updateArchitectures);
    connect(m_ui->debuggerVersion, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RemoteDebugger::updateRemoteActions);
    connect(m_ui->debuggerArchitecture, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RemoteDebugger::updateRemoteActions);
    connect(m_ui->copyFoldersButton, &QPushButton::clicked, this, [this]() { copyPaths(true); });
    connect(m_ui->copyPathsButton, &QPushButton::clicked, this, [this]() { copyPaths(false); });
    connect(m_ui->rescanButton, &QPushButton::clicked, this, &RemoteDebugger::scan);
    connect(m_ui->downloadDebuggerButton, &QPushButton::clicked, this, &RemoteDebugger::download);
    connect(m_ui->installRemoteButton, &QPushButton::clicked, this, [this]() { deploy("install"); });
    connect(m_ui->browseRemoteInstallerButton, &QPushButton::clicked, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(this, "选择已下载的微软 Remote Tools 安装包",
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), "Remote Tools (*.exe)");
        if (path.isEmpty()) return;
        const QString name = QString("VS_RemoteTools_%1_%2").arg(m_ui->debuggerVersion->currentText().section(' ', -1),
            m_ui->debuggerArchitecture->currentText());
        QSettings().setValue("remoteTools/installers/" + name, path);
        setRemoteStatus("已选择安装包：" + QDir::toNativeSeparators(path) + "\n安装时会检查微软签名、工具版本及目标架构。");
        Diagnostics::event("remote.installer_selected", {{"path", path}, {"major", m_ui->debuggerVersion->currentData().toInt()}, {"arch", m_ui->debuggerArchitecture->currentText()}});
    });
    connect(m_ui->enableRemoteStartupButton, &QPushButton::clicked, this, [this]() { deploy("enable"); });
    connect(m_ui->disableRemoteStartupButton, &QPushButton::clicked, this, [this]() { deploy("disable"); });
    connect(m_ui->remoteHostCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &RemoteDebugger::updateRemoteActions);
    connect(m_ui->officialPageButton, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl("https://learn.microsoft.com/zh-cn/visualstudio/debugger/remote-debugging"));
    });
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(60000);
    connect(m_timeout, &QTimer::timeout, this, [this]() { finishDownload("下载超时，请重试。"); });
    scan();
    updateRemoteActions();
}

void RemoteDebugger::setHosts(const QVector<HostEntry> &hosts, int selectedRow)
{
    if (isDeploying()) return;
    const int previous = m_ui->remoteHostCombo->currentIndex();
    const QString previousId = previous >= 0 && previous < m_hosts.size() ? m_hosts.at(previous).id : QString();
    m_hosts = hosts;
    m_ui->remoteHostCombo->clear();
    int selection = selectedRow;
    for (int row = 0; row < m_hosts.size(); ++row) {
        const auto &host = m_hosts.at(row);
        m_ui->remoteHostCombo->addItem(QString("%1 — %2").arg(host.name, host.sharePath));
        m_ui->remoteHostCombo->setItemData(row, host.sharePath, Qt::ToolTipRole);
        if ((selection < 0 || selection >= hosts.size()) && host.id == previousId) selection = row;
    }
    if (selection >= 0 && selection < m_hosts.size()) m_ui->remoteHostCombo->setCurrentIndex(selection);
    else if (!previousId.isEmpty()) m_ui->remoteHostCombo->setCurrentIndex(-1);
    updateRemoteActions();
}

bool RemoteDebugger::isDeploying() const
{
    return m_deployment != nullptr;
}

void RemoteDebugger::closeEvent(QCloseEvent *event)
{
    if (isDeploying()) {
        setRemoteStatus("远程操作正在进行，请等待结果后再关闭窗口。");
        event->ignore();
        return;
    }
    RoundedDialog::closeEvent(event);
}

void RemoteDebugger::reject()
{
    if (isDeploying()) {
        setRemoteStatus("远程操作正在进行，请等待结果后再关闭窗口。");
        return;
    }
    RoundedDialog::reject();
}

void RemoteDebugger::updateRemoteActions()
{
    m_ui->remoteGroup->setTitle(QString("远程主机安装与开机自启 — %1 / %2")
        .arg(m_ui->debuggerVersion->currentText(), m_ui->debuggerArchitecture->currentText()));
    const bool busy = isDeploying() || bool(m_download);
    const int row = m_ui->remoteHostCombo->currentIndex();
    const bool available = row >= 0 && row < m_hosts.size() && !busy;
    m_ui->remoteHostCombo->setEnabled(!busy);
    m_ui->installRemoteButton->setEnabled(available);
    m_ui->browseRemoteInstallerButton->setEnabled(!busy);
    m_ui->enableRemoteStartupButton->setEnabled(available);
    m_ui->disableRemoteStartupButton->setEnabled(available);
    m_ui->debuggerVersion->setEnabled(!busy);
    m_ui->debuggerArchitecture->setEnabled(!busy);
    m_ui->downloadDebuggerButton->setEnabled(!busy);
}

void RemoteDebugger::deploy(const QString &operation)
{
    if (isDeploying() || m_download) return;
    m_deploymentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_deploymentElapsed.start();
    Diagnostics::event("remote.request", {{"run", m_deploymentId}, {"action", Diagnostics::lastUserAction()}, {"operation", operation}});
    const int row = m_ui->remoteHostCombo->currentIndex();
    if (row < 0 || row >= m_hosts.size()) {
        setRemoteStatus("请先保存并选择需要管理的主机。");
        return;
    }
    const HostEntry host = m_hosts.at(row);
    Diagnostics::rememberSecret(host.password);
    Diagnostics::event("remote.target", {{"run", m_deploymentId}, {"host", host.id}, {"share", host.sharePath}, {"user", host.username},
        {"major", m_ui->debuggerVersion->currentData().toInt()}, {"arch", m_ui->debuggerArchitecture->currentText()}});
    QString share = host.sharePath;
    share.replace('/', '\\');
    const QString server = share.startsWith("\\\\") ? share.section('\\', 2, 2) : QString();
    static const QRegularExpression serverPattern("^[A-Za-z0-9_.-]{1,253}$");
    if (!serverPattern.match(server).hasMatch() || server.contains("..") || host.username.trimmed().isEmpty()) {
        setRemoteStatus("主机配置无效：请保存有效的共享路径，以及用于远程管理的管理员账号密码。");
        return;
    }
    QString installer;
    if (operation == "install") {
        const QString name = QString("VS_RemoteTools_%1_%2").arg(m_ui->debuggerVersion->currentText().section(' ', -1),
            m_ui->debuggerArchitecture->currentText());
        installer = QSettings().value("remoteTools/installers/" + name).toString();
        if (!QFileInfo(installer).isFile()) {
            installer.clear();
            const QString downloadsPath = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
            if (!downloadsPath.isEmpty()) {
                const auto files = QDir(downloadsPath).entryInfoList({name + "*.exe"}, QDir::Files, QDir::Time);
                if (!files.isEmpty()) installer = files.first().absoluteFilePath();
            }
        }
        if (installer.isEmpty()) installer = QFileDialog::getOpenFileName(this, "选择已下载的微软 Remote Tools 安装包",
            QStandardPaths::writableLocation(QStandardPaths::DownloadLocation), "Remote Tools (*.exe)");
        if (installer.isEmpty()) { setRemoteStatus("已取消选择安装包，本次未执行远程安装。"); return; }
        const QFileInfo info(installer);
        if (!info.isFile() || info.size() <= 0 || info.size() > maxInstallerSize) {
            setRemoteStatus("安装包不存在、为空或超过 256 MiB，请重新选择。");
            return;
        }
    }
    const int currentRow = m_ui->remoteHostCombo->currentIndex();
    if (currentRow < 0 || currentRow >= m_hosts.size() || m_hosts.at(currentRow).id != host.id
        || m_hosts.at(currentRow).sharePath != host.sharePath || m_hosts.at(currentRow).username != host.username
        || m_hosts.at(currentRow).password != host.password) {
        setRemoteStatus("主机配置在选择安装包期间发生变化，请核对目标主机后重新操作。");
        return;
    }
    QFile script(":/remote-deploy.ps1"), bootstrap(":/remote-bootstrap.ps1");
    if (!script.open(QIODevice::ReadOnly) || !bootstrap.open(QIODevice::ReadOnly)) {
        setRemoteStatus("无法读取远程部署资源。");
        return;
    }
    m_deploymentDirectory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/rrs-deploy-XXXXXX");
    if (!m_deploymentDirectory->isValid()) {
        setRemoteStatus("无法创建本机部署临时目录：" + m_deploymentDirectory->errorString());
        m_deploymentDirectory.reset();
        return;
    }
    QFile scriptFile(m_deploymentDirectory->filePath("deploy.ps1"));
    QByteArray scriptBytes = script.readAll();
    if (!scriptBytes.startsWith(QByteArray::fromHex("efbbbf"))) scriptBytes.prepend(QByteArray::fromHex("efbbbf"));
    if (!scriptFile.open(QIODevice::WriteOnly) || scriptFile.write(scriptBytes) != scriptBytes.size()
        || !scriptFile.flush()) {
        setRemoteStatus("无法准备本机部署脚本：" + scriptFile.errorString());
        scriptFile.close();
        m_deploymentDirectory.reset();
        return;
    }
    // QTemporaryFile::close() 在 Qt 5.12 仍保留写句柄；普通 QFile 关闭后 PowerShell 才能读取。
    scriptFile.close();
    QJsonObject request;
    request.insert("operation", operation);
    request.insert("run", m_deploymentId);
    request.insert("server", server);
    request.insert("username", host.username.trimmed());
    request.insert("password", host.password);
    request.insert("major", m_ui->debuggerVersion->currentData().toInt());
    request.insert("architecture", m_ui->debuggerArchitecture->currentText());
    request.insert("installer", QDir::toNativeSeparators(installer));
    request.insert("bootstrap", QString::fromLatin1(bootstrap.readAll().toBase64()));
    const QByteArray payload = QJsonDocument(request).toJson(QJsonDocument::Compact);
    startDeploymentProcess(scriptFile.fileName(), payload);
}

void RemoteDebugger::startDeploymentProcess(const QString &scriptPath, const QByteArray &payload)
{
    if (m_deployment || !QDir::isAbsolutePath(scriptPath) || payload.isEmpty()) return;
    const QString powershell = qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
    if (!QFileInfo(powershell).isFile()) {
        setRemoteStatus("未找到 Windows PowerShell，无法执行远程操作。");
        m_deploymentDirectory.reset();
        return;
    }
    QFile launcher(":/remote-launch.ps1");
    if (!launcher.open(QIODevice::ReadOnly)) {
        setRemoteStatus("无法读取本机部署启动器。");
        m_deploymentDirectory.reset();
        return;
    }
    QString command = QString::fromUtf8(launcher.readAll());
    if (command.startsWith(QChar(0xfeff))) command.remove(0, 1);
    QByteArray encoded(command.size() * 2, '\0');
    for (int index = 0; index < command.size(); ++index)
        qToLittleEndian<quint16>(command.at(index).unicode(), reinterpret_cast<uchar *>(encoded.data()) + index * 2);
    m_deploymentOutput.clear();
    m_deploymentErrors.clear();
    m_deploymentHasResult = false;
    m_remoteContactStarted = false;
    m_deployment = new QProcess(this);
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert("RRS_DEPLOY_SCRIPT", QDir::toNativeSeparators(scriptPath));
    environment.insert("RRS_DEPLOY_RUN", m_deploymentId);
    m_deployment->setProcessEnvironment(environment);
    m_deployment->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_deployment, &QProcess::started, this, [this, payload]() {
        // 凭据仅通过子进程标准输入传递，避免出现在命令行、脚本文件或诊断日志中。
        if (m_deployment->write(payload) != payload.size()) {
            setRemoteStatus("无法向部署进程传递操作参数。");
            m_deployment->kill();
        }
        m_deployment->closeWriteChannel();
    });
    connect(m_deployment, &QProcess::readyReadStandardOutput, this, &RemoteDebugger::readDeploymentOutput);
    connect(m_deployment, &QProcess::readyReadStandardError, this, [this]() {
        m_deploymentErrors += m_deployment->readAllStandardError();
        m_deploymentErrors.truncate(8192);
    });
    connect(m_deployment, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this](int, QProcess::ExitStatus) {
        finishDeployment();
    });
    connect(m_deployment, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) finishDeployment();
    });
    setRemoteStatus("正在启动本机部署脚本…");
    Diagnostics::event("remote.process_start", {{"run", m_deploymentId}, {"script", scriptPath}, {"powershell", powershell}});
    updateRemoteActions();
    // 启动器先统一输出编码，再读取脚本；路径通过进程环境传递，不拼接进 PowerShell 代码。
    m_deployment->start(powershell, {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-InputFormat", "Text", "-OutputFormat", "Text", "-EncodedCommand",
        QString::fromLatin1(encoded.toBase64())});
}

void RemoteDebugger::readDeploymentOutput()
{
    if (!m_deployment) return;
    m_deploymentOutput += m_deployment->readAllStandardOutput();
    if (m_deploymentOutput.size() > 1024 * 1024) { m_deploymentOutput.clear(); return; }
    for (int newline = m_deploymentOutput.indexOf('\n'); newline >= 0; newline = m_deploymentOutput.indexOf('\n')) {
        const QByteArray line = m_deploymentOutput.left(newline).trimmed();
        m_deploymentOutput.remove(0, newline + 1);
        const auto object = QJsonDocument::fromJson(line).object();
        if (object.value("remote_contact").toBool()) m_remoteContactStarted = true;
        const QString type = object.value("type").toString();
        if (type == "result" && object.value("success").isBool() && object.value("message").isString()
            && !object.value("message").toString().trimmed().isEmpty()) {
            m_deploymentHasResult = true;
            Diagnostics::event("remote.result", {{"run", m_deploymentId}, {"response", object}, {"elapsed_ms", double(m_deploymentElapsed.elapsed())}}, !object.value("success").toBool());
            setRemoteStatus(object.value("message").toString());
        } else if (type == "progress") {
            Diagnostics::event("remote.stage", {{"run", m_deploymentId}, {"response", object}, {"elapsed_ms", double(m_deploymentElapsed.elapsed())}});
            if (m_deploymentHasResult) setRemoteStatus(m_ui->remoteStatus->text() + '\n' + object.value("message").toString());
            else setRemoteStatus(object.value("message").toString());
        }
    }
}

void RemoteDebugger::finishDeployment()
{
    if (!m_deployment) return;
    readDeploymentOutput();
    if (!m_deploymentOutput.trimmed().isEmpty()) {
        m_deploymentOutput.append('\n');
        readDeploymentOutput();
    }
    m_deploymentErrors += m_deployment->readAllStandardError();
    m_deploymentErrors.truncate(8192);
    const QString detail = deploymentErrorText(m_deploymentErrors).trimmed();
    if (!m_deploymentHasResult) {
        setRemoteStatus((m_remoteContactStarted ? QString("部署进程未返回有效结果：") : QString("本机部署进程未返回有效结果："))
            + (detail.isEmpty() ? m_deployment->errorString() : detail)
            + (m_remoteContactStarted ? "\n已进入远程连接阶段，执行状态未知，请检查主机状态后再操作。" : "\n未进入远程连接阶段。"));
    } else if (m_remoteContactStarted && (m_deployment->exitStatus() != QProcess::NormalExit || m_deployment->exitCode() != 0)) {
        setRemoteStatus(m_ui->remoteStatus->text() + "\n部署进程异常退出，临时任务或文件可能未完成清理。");
    }
    Diagnostics::event("remote.process_finished", {{"run", m_deploymentId}, {"exit", m_deployment->exitCode()},
        {"exit_status", int(m_deployment->exitStatus())}, {"process_error", m_deployment->exitStatus() == QProcess::NormalExit && m_deployment->exitCode() == 0 ? QString() : m_deployment->errorString()},
        {"stderr", detail}, {"has_result", m_deploymentHasResult}, {"remote_contact", m_remoteContactStarted},
        {"elapsed_ms", double(m_deploymentElapsed.elapsed())}}, !m_deploymentHasResult || m_deployment->exitCode() != 0);
    m_deployment->deleteLater();
    m_deployment = nullptr;
    m_deploymentDirectory.reset();
    m_deploymentErrors.clear();
    updateRemoteActions();
    emit deploymentFinished();
}

void RemoteDebugger::setRemoteStatus(const QString &message)
{
    m_ui->remoteStatus->setText(message);
    Diagnostics::event("remote.status", {{"run", m_deploymentId}, {"message", message}});
}

RemoteDebugger::~RemoteDebugger()
{
    for (QObject *child : findChildren<QObject *>()) {
        disconnect(child, nullptr, this, nullptr);
    }
    for (QProcess *process : findChildren<QProcess *>()) {
        if (process == m_deployment || process->state() == QProcess::NotRunning) continue;
        Diagnostics::event("debugger.discovery_cancel", {{"pid", double(process->processId())}});
        process->kill();
        if (!process->waitForFinished(1000)) Diagnostics::event("debugger.discovery_cancel_timeout", {}, true);
    }
    if (m_reply) m_reply->abort();
}

void RemoteDebugger::scan()
{
    Diagnostics::event("debugger.discovery_start", {{"action", Diagnostics::lastUserAction()}});
    m_ui->rescanButton->setEnabled(false);
    QString vswhere;
    for (const QString &root : programRoots()) {
        const QString path = QDir(root).filePath("Microsoft Visual Studio/Installer/vswhere.exe");
        if (QFileInfo(path).isFile()) {
            vswhere = path;
            break;
        }
    }
    if (vswhere.isEmpty()) {
        showPaths({});
        return;
    }
    m_ui->debuggerStatus->setText("正在检测本机安装…");
    auto *process = new QProcess(this);
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this, [this, process]() {
        showPaths(process->readAllStandardOutput());
        process->deleteLater();
    });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            showPaths({});
            process->deleteLater();
        }
    });
    QTimer::singleShot(5000, process, [process]() {
        if (process->state() != QProcess::NotRunning) process->kill();
    });
    process->start(vswhere, {"-all", "-products", "*", "-prerelease", "-utf8", "-format", "json"});
}

void RemoteDebugger::showPaths(const QByteArray &vswhereOutput)
{
    QStringList roots = remoteToolsInstallations();
    for (const auto &entry : QJsonDocument::fromJson(vswhereOutput).array()) {
        const QString root = entry.toObject().value("installationPath").toString();
        if (!root.isEmpty()) roots.append(root);
    }
    for (const QString &path : programRoots()) {
        const QDir program(path);
        for (const QString &name : program.entryList({"Microsoft Visual Studio*"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QDir vs(program.filePath(name));
            roots.append(vs.path());
            for (const QString &year : vs.entryList({"20??"}, QDir::Dirs | QDir::NoDotAndDotDot)) {
                const QDir editions(vs.filePath(year));
                for (const QString &edition : editions.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    roots.append(editions.filePath(edition));
                }
            }
        }
    }
    constexpr const char *locations[] = {"", "Remote Debugger", "Common7/IDE/Remote Debugger"};
    QSet<QString> seen;
    QVector<Installation> installations;
    roots.removeDuplicates();
    for (const QString &root : roots) {
        for (const char *location : locations) {
            findDebuggers(QDir(location[0] ? QDir(root).filePath(QString::fromLatin1(location)) : root), &seen, &installations);
        }
    }
    std::sort(installations.begin(), installations.end(), [](const Installation &left, const Installation &right) {
        return left.path.compare(right.path, Qt::CaseInsensitive) < 0;
    });
    m_ui->debuggerPaths->setRowCount(installations.size());
    for (int row = 0; row < installations.size(); ++row) {
        const auto &install = installations.at(row);
        auto *selected = new QTableWidgetItem();
        selected->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        selected->setCheckState(Qt::Unchecked);
        m_ui->debuggerPaths->setItem(row, 0, selected);
        m_ui->debuggerPaths->setItem(row, 1, new QTableWidgetItem(install.version));
        m_ui->debuggerPaths->setItem(row, 2, new QTableWidgetItem(install.architecture));
        auto *path = new QTableWidgetItem(QDir::toNativeSeparators(install.path));
        QFont linkFont = m_ui->debuggerPaths->font();
        linkFont.setUnderline(true);
        path->setFont(linkFont);
        path->setForeground(QColor("#145a9c"));
        path->setToolTip("打开所在目录\n" + path->text());
        path->setData(Qt::AccessibleDescriptionRole, "单击路径或按回车打开所在目录");
        m_ui->debuggerPaths->setItem(row, pathColumn, path);
    }
    m_ui->rescanButton->setEnabled(true);
    m_ui->copyFoldersButton->setEnabled(!installations.isEmpty());
    m_ui->copyPathsButton->setEnabled(!installations.isEmpty());
    m_ui->debuggerStatus->setText(installations.isEmpty() ? "未检测到 msvsmon.exe。"
                                                         : QString("检测到 %1 个 msvsmon.exe。").arg(installations.size()));
    Diagnostics::record("debugger.scan", QString("count=%1").arg(installations.size()));
}

void RemoteDebugger::openDebuggerFolder(int row)
{
    const auto *path = m_ui->debuggerPaths->item(row, pathColumn);
    if (!path) return;
    const QDir folder = QFileInfo(path->text()).absoluteDir();
    if (!folder.exists()) {
        m_ui->debuggerStatus->setText("所在目录已不存在，请重新检测。");
        return;
    }
    if (!QDesktopServices::openUrl(QUrl::fromLocalFile(folder.absolutePath()))) {
        m_ui->debuggerStatus->setText("无法打开所在目录。");
        return;
    }
    m_ui->debuggerStatus->setText("已打开所在目录。");
}

void RemoteDebugger::copyPaths(bool directories)
{
    QStringList paths;
    QList<QUrl> urls;
    QSet<QString> folderNames;
    for (int row = 0; row < m_ui->debuggerPaths->rowCount(); ++row) {
        if (m_ui->debuggerPaths->item(row, 0)->checkState() != Qt::Checked) continue;
        const QString path = m_ui->debuggerPaths->item(row, pathColumn)->text();
        paths.append(path);
        if (!directories) continue;
        const QDir folder = QFileInfo(path).dir();
        const QString folderName = folder.dirName().toCaseFolded();
        if (folderNames.contains(folderName)) {
            m_ui->debuggerStatus->setText("选中了来自不同版本的同名架构目录。请分批复制并粘贴到各自的目标文件夹。");
            return;
        }
        if (!folder.exists()) {
            m_ui->debuggerStatus->setText("所选目录已不存在，请重新检测。");
            return;
        }
        folderNames.insert(folderName);
        urls.append(QUrl::fromLocalFile(folder.absolutePath()));
    }
    if (paths.isEmpty()) {
        m_ui->debuggerStatus->setText("请先勾选要复制的版本。");
        return;
    }
    if (directories) {
        auto *mime = new QMimeData;
        mime->setUrls(urls);
        QApplication::clipboard()->setMimeData(mime);
        m_ui->debuggerStatus->setText("已复制目录，可在资源管理器中粘贴到目标共享。");
    } else {
        QApplication::clipboard()->setText(paths.join("\r\n"));
        m_ui->debuggerStatus->setText("已复制 msvsmon.exe 路径。");
    }
}

void RemoteDebugger::updateArchitectures()
{
    const QString selected = m_ui->debuggerArchitecture->currentText();
    m_ui->debuggerArchitecture->clear();
    m_ui->debuggerArchitecture->addItem("x64", "amd64");
    m_ui->debuggerArchitecture->addItem("x86", "x86");
    if (m_ui->debuggerVersion->currentData().toInt() >= 17) m_ui->debuggerArchitecture->addItem("ARM64", "arm64");
    const int index = m_ui->debuggerArchitecture->findText(selected);
    if (index >= 0) m_ui->debuggerArchitecture->setCurrentIndex(index);
}

void RemoteDebugger::download()
{
    if (m_download || isDeploying()) return;
    m_downloadId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_downloadElapsed.start();
    m_downloadReceived = 0;
    m_downloadTotal = -1;
    m_ui->downloadProgressArea->show();
    DownloadProgress::apply(m_ui->downloadProgressBar, -1);
    m_ui->downloadProgressLabel->setText("正在准备下载...");
    const int version = m_ui->debuggerVersion->currentData().toInt();
    const QString architecture = m_ui->debuggerArchitecture->currentData().toString();
    const QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (downloads.isEmpty() || !QDir().mkpath(downloads)) {
        finishDownload("无法打开 Windows 的“下载”文件夹。");
        return;
    }
    m_downloadName = QString("VS_RemoteTools_%1_%2").arg(m_ui->debuggerVersion->currentText().section(' ', -1),
                                                        m_ui->debuggerArchitecture->currentText());
    m_download = std::make_unique<QTemporaryFile>(QDir(downloads).filePath(m_downloadName + ".partial-XXXXXX"));
    if (!m_download->open()) {
        finishDownload("无法在“下载”文件夹写入文件：" + m_download->errorString());
        return;
    }
    updateRemoteActions();
    m_redirects = 0;
    m_ui->downloadDebuggerButton->setEnabled(false);
    m_ui->debuggerStatus->setText("正在后台下载微软 Remote Tools…");
    Diagnostics::event("debugger.download_start", {{"run", m_downloadId}, {"action", Diagnostics::lastUserAction()}, {"vs", version}, {"arch", architecture},
        {"directory", downloads}, {"ssl_supported", QSslSocket::supportsSsl()}, {"ssl_build", QSslSocket::sslLibraryBuildVersionString()},
        {"ssl_runtime", QSslSocket::sslLibraryVersionString()}, {"proxy_type", int(m_network->proxy().type())}});
    request(QUrl(QString("https://aka.ms/vs/%1/release/RemoteTools.%2ret.chs.exe").arg(version).arg(architecture)));
}

void RemoteDebugger::request(const QUrl &url)
{
    Diagnostics::event("debugger.download_request", {{"run", m_downloadId}, {"url", url.toString()}, {"redirect", m_redirects}});
    if (!microsoftUrl(url) || m_redirects > 5) {
        finishDownload("下载跳转未通过微软来源检查。");
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    m_reply = m_network->get(request);
    m_reply->setReadBufferSize(1024 * 1024);
    DownloadProgress::apply(m_ui->downloadProgressBar, -1);
    m_ui->downloadProgressLabel->setText("正在连接下载服务器...");
    QNetworkReply *reply = m_reply.data();
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError> &errors) {
        QStringList messages;
        for (const auto &error : errors) messages.append(error.errorString());
        Diagnostics::event("debugger.download_tls_errors", {{"run", m_downloadId}, {"url", reply->url().toString()},
            {"errors", QJsonArray::fromStringList(messages)}}, true);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (m_reply != reply || !m_download
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        updateDownloadProgress(received, total);
    });
    connect(m_reply, &QNetworkReply::readyRead, this, &RemoteDebugger::readDownload);
    connect(m_reply, &QNetworkReply::finished, this, &RemoteDebugger::completeRequest);
    m_timeout->start();
}

void RemoteDebugger::readDownload()
{
    if (!m_reply) return;
    const QByteArray data = m_reply->readAll();
    const int http = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    m_timeout->start();
    if (http != 200 || data.isEmpty()) return;
    if (m_download->size() + data.size() > maxInstallerSize || m_download->write(data) != data.size()) {
        finishDownload("写入下载文件失败，或文件大小超过限制。");
    }
}

void RemoteDebugger::updateDownloadProgress(qint64 received, qint64 total)
{
    m_downloadReceived = qMax<qint64>(0, received);
    m_downloadTotal = total > 0 ? total : -1;
    DownloadProgress::apply(m_ui->downloadProgressBar, DownloadProgress::percentage(m_downloadReceived, m_downloadTotal));
    const QString stage = m_downloadTotal > 0 && m_downloadReceived >= m_downloadTotal
        ? QString("正在完成下载与校验：%1") : QString("正在下载：%1");
    m_ui->downloadProgressLabel->setText(stage.arg(DownloadProgress::text(m_downloadReceived, m_downloadTotal)));
}

void RemoteDebugger::completeRequest()
{
    if (!m_reply) return;
    Diagnostics::event("debugger.download_response", {{"run", m_downloadId}, {"url", m_reply->url().toString()},
        {"http", m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()}, {"network_error", int(m_reply->error())}, {"error", m_reply->error() == QNetworkReply::NoError ? QString() : m_reply->errorString()}});
    readDownload();
    if (!m_reply) return;
    auto *reply = m_reply.data();
    m_reply.clear();
    reply->deleteLater();
    const QUrl redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (reply->error() == QNetworkReply::NoError && !redirect.isEmpty()) {
        ++m_redirects;
        request(reply->url().resolved(redirect));
        return;
    }
    if (reply->error() != QNetworkReply::NoError ||
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200 || m_download->size() == 0) {
        finishDownload("无法下载微软 Remote Tools：" + reply->errorString());
        return;
    }
    const QString temporaryPath = m_download->fileName();
    m_downloadReceived = m_download->size();
    DownloadProgress::apply(m_ui->downloadProgressBar, -1);
    m_ui->downloadProgressLabel->setText(QString("正在校验安装包（%1）...").arg(DownloadProgress::size(m_downloadReceived)));
    if (!m_download->flush()) {
        finishDownload("无法完成安装包写入。");
        return;
    }
    // QTemporaryFile keeps its Windows file handle until destruction.
    m_download->setAutoRemove(false);
    m_download.reset();
    finishDownload(microsoftSignatureError(temporaryPath), temporaryPath);
}

void RemoteDebugger::finishDownload(const QString &reason, const QString &temporaryPath)
{
    m_timeout->stop();
    if (m_download) m_downloadReceived = qMax<qint64>(0, m_download->size());
    if (auto *reply = m_reply.data()) {
        disconnect(reply, nullptr, this, nullptr);
        m_reply.clear();
        reply->abort();
        reply->deleteLater();
    }
    QString path;
    QString error = reason;
    if (error.isEmpty() && !temporaryPath.isEmpty()) {
        const QDir directory(QFileInfo(temporaryPath).absolutePath());
        for (int index = 0; index < 1000; ++index) {
            const QString suffix = index ? QString(" (%1)").arg(index) : QString();
            const QString candidate = directory.filePath(m_downloadName + suffix + ".exe");
            if (QFile::rename(temporaryPath, candidate)) {
                path = QDir::toNativeSeparators(candidate);
                break;
            }
            if (!QFileInfo::exists(candidate)) break;
        }
        if (path.isEmpty()) error = "安装包已下载，但无法保存到“下载”文件夹。";
    }
    if (path.isEmpty() && !temporaryPath.isEmpty()) QFile::remove(temporaryPath);
    m_download.reset();
    m_ui->downloadDebuggerButton->setEnabled(true);
    if (error.isEmpty() && !path.isEmpty()) {
        QSettings().setValue("remoteTools/installers/" + m_downloadName, path);
        DownloadProgress::apply(m_ui->downloadProgressBar, 100);
        m_ui->downloadProgressLabel->setText(QString("下载完成：%1").arg(DownloadProgress::size(QFileInfo(path).size())));
    } else {
        DownloadProgress::apply(m_ui->downloadProgressBar, 0);
        m_ui->downloadProgressLabel->setText(QString("下载失败（已下载 %1）").arg(DownloadProgress::size(m_downloadReceived)));
    }
    m_ui->debuggerStatus->setText(error.isEmpty() ? "下载完成：" + path : error);
    Diagnostics::event(error.isEmpty() ? "debugger.download_done" : "debugger.download_failed", {{"run", m_downloadId}, {"path", path}, {"error", error},
        {"bytes", double(m_downloadReceived)}, {"elapsed_ms", double(m_downloadElapsed.isValid() ? m_downloadElapsed.elapsed() : 0)}}, !error.isEmpty());
    updateRemoteActions();
    emit downloadFinished(path, error);
}
