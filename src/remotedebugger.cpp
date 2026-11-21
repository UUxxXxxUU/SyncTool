#pragma execution_character_set("utf-8")
#include "remotedebugger.h"
#include "diagnostics.h"
#include "downloadprogress.h"
#include "ui_remotedebugger.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
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
#include <QProcess>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QTimer>
#include <QtEndian>
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
    connect(m_ui->copyFoldersButton, &QPushButton::clicked, this, [this]() { copyPaths(true); });
    connect(m_ui->copyPathsButton, &QPushButton::clicked, this, [this]() { copyPaths(false); });
    connect(m_ui->rescanButton, &QPushButton::clicked, this, &RemoteDebugger::scan);
    connect(m_ui->downloadDebuggerButton, &QPushButton::clicked, this, &RemoteDebugger::download);
    connect(m_ui->officialPageButton, &QPushButton::clicked, this, [this]() {
        QDesktopServices::openUrl(QUrl("https://learn.microsoft.com/zh-cn/visualstudio/debugger/remote-debugging"));
    });
    m_timeout->setSingleShot(true);
    m_timeout->setInterval(60000);
    connect(m_timeout, &QTimer::timeout, this, [this]() { finishDownload("下载超时，请重试。"); });
    scan();
}

RemoteDebugger::~RemoteDebugger()
{
    for (QObject *child : findChildren<QObject *>()) {
        disconnect(child, nullptr, this, nullptr);
    }
    if (m_reply) m_reply->abort();
}

void RemoteDebugger::scan()
{
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
    if (m_download) return;
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
    m_redirects = 0;
    m_ui->downloadDebuggerButton->setEnabled(false);
    m_ui->debuggerStatus->setText("正在后台下载微软 Remote Tools…");
    Diagnostics::record("debugger.download_start", QString("vs=%1 arch=%2").arg(version).arg(architecture));
    request(QUrl(QString("https://aka.ms/vs/%1/release/RemoteTools.%2ret.chs.exe").arg(version).arg(architecture)));
}

void RemoteDebugger::request(const QUrl &url)
{
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
        DownloadProgress::apply(m_ui->downloadProgressBar, 100);
        m_ui->downloadProgressLabel->setText(QString("下载完成：%1").arg(DownloadProgress::size(QFileInfo(path).size())));
    } else {
        DownloadProgress::apply(m_ui->downloadProgressBar, 0);
        m_ui->downloadProgressLabel->setText(QString("下载失败（已下载 %1）").arg(DownloadProgress::size(m_downloadReceived)));
    }
    m_ui->debuggerStatus->setText(error.isEmpty() ? "下载完成：" + path : error);
    Diagnostics::record(error.isEmpty() ? "debugger.download_done" : "debugger.download_failed", {}, !error.isEmpty());
    emit downloadFinished(path, error);
}
