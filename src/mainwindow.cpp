#pragma execution_character_set("utf-8")
#include "mainwindow.h"

#include "configmanager.h"
#include "syncworker.h"
#include "syncplan.h"
#include "ui_mainwindow.h"
#include "ui_syncruledialog.h"
#include "ui_usagetipsdialog.h"
#include "ui_recentcopypreview.h"
#include "ui_updatedialog.h"
#include <QDialogButtonBox>

#include <algorithm>
#include <memory>
#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QColor>
#include <QComboBox>
#include <QCoreApplication>
#include <QMenu>
#include <QModelIndex>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QStyle>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QCursor>
#include <QDir>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPersistentModelIndex>
#include <QProcess>
#include <QPoint>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QVersionNumber>
#include <QUuid>
#include <qdatetime.h>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#pragma comment(lib, "Shell32.lib")
#endif

class RecentCopyPreview : public QFrame
{
public:
    explicit RecentCopyPreview(QWidget *parent)
        : QFrame(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowDoesNotAcceptFocus)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        Ui::RecentCopyPreview ui;
        ui.setupUi(this);
        m_title = ui.titleLabel;
        m_text = ui.textEdit;
        m_text->viewport()->setAutoFillBackground(false);
        m_text->setWordWrapMode(QTextOption::WrapAnywhere);
        m_showTimer.setSingleShot(true);
        m_showTimer.setInterval(100);
        connect(&m_showTimer, &QTimer::timeout, this, [this]() {
            if (!anchorContainsCursor()) {
                dismiss();
                return;
            }

            QScreen *screen = QGuiApplication::screenAt(QCursor::pos());
            if (screen == nullptr) {
                screen = QGuiApplication::primaryScreen();
            }
            if (screen == nullptr) {
                return;
            }

            updateContents();
            const QRect area = screen->availableGeometry();
            resize(qMin(520, area.width()), qMin(300, area.height()));
            // 紧贴悬停区域，避免移入浮窗时穿过下一行并切换到其他主机。
            const QRect anchor = anchorRect();
            QPoint position = m_anchor->mapToGlobal(QPoint(anchor.left(), anchor.bottom() + 1));
            if (position.y() + height() > area.bottom() + 1) {
                position.setY(m_anchor->mapToGlobal(anchor.topLeft()).y() - height());
            }
            position.setX(qBound(area.left(), position.x(), area.right() - width() + 1));
            position.setY(qBound(area.top(), position.y(), area.bottom() - height() + 1));
            move(position);
            QToolTip::hideText();
            show();
        });

        m_hideTimer.setSingleShot(true);
        m_hideTimer.setInterval(200);
        connect(&m_hideTimer, &QTimer::timeout, this, [this]() {
            const QPoint cursor = QCursor::pos();
            if ((isVisible() && rect().contains(mapFromGlobal(cursor)))
                || anchorContainsCursor()) {
                return;
            }
            dismiss();
        });
        parent->installEventFilter(this);
    }

    void watch(QProgressBar *bar, const QString &hostId)
    {
        bar->setProperty("copyPreviewHostId", hostId);
        bar->installEventFilter(this);
    }

    void watchDetails(QTableWidget *table)
    {
        m_table = table;
        table->viewport()->setMouseTracking(true);
        table->viewport()->installEventFilter(this);
        connect(table->verticalScrollBar(), &QScrollBar::valueChanged, this, [this]() { dismiss(); });
        connect(table->horizontalScrollBar(), &QScrollBar::valueChanged, this, [this]() { dismiss(); });
        connect(table->horizontalHeader(), &QHeaderView::sectionResized, this, [this]() { dismiss(); });
        connect(table->verticalHeader(), &QHeaderView::sectionResized, this, [this]() { dismiss(); });
    }

    void setRecentFiles(const QString &hostId, const QStringList &files, const QString &title)
    {
        m_files.insert(hostId, files);
        m_titles.insert(hostId, title);
        if (isVisible() && !m_detailsMode && m_hostId == hostId) {
            if (files.isEmpty()) {
                m_text->clear();
            }
            updateContents();
        }
    }

    void setDetails(const QString &hostId, const QString &text, bool remember)
    {
        if (remember) {
            m_details.insert(hostId, text);
        }
        if (isVisible() && m_detailsMode && m_hostId == hostId) {
            updateContents();
        }
    }

    void removeHost(const QString &hostId)
    {
        m_files.remove(hostId);
        m_titles.remove(hostId);
        m_details.remove(hostId);
    }

    void dismiss()
    {
        m_showTimer.stop();
        m_hideTimer.stop();
        hide();
        m_anchor.clear();
        m_hostId.clear();
        m_detailIndex = QPersistentModelIndex();
        m_detailsMode = false;
        m_text->clear();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        // 显式绘制半透明底板，避免透明顶层窗口仅显示文字。
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor(163, 171, 181));
        painter.setBrush(QColor(248, 250, 252, 245));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6);
    }

    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parentWidget()) {
            if (event->type() == QEvent::Hide || event->type() == QEvent::WindowDeactivate
                || event->type() == QEvent::Move || event->type() == QEvent::Resize) {
                dismiss();
            }
        } else if (m_table != nullptr && watched == m_table->viewport()) {
            if (event->type() == QEvent::MouseMove || event->type() == QEvent::Enter
                || event->type() == QEvent::ToolTip) {
                const QModelIndex index = m_table->indexAt(m_table->viewport()->mapFromGlobal(QCursor::pos()));
                if (index.isValid() && index.column() == 6 && m_table->item(index.row(), 0) != nullptr) {
                    beginPreview(m_table->viewport(), m_table->item(index.row(), 0)->data(Qt::UserRole).toString(), index);
                    if (event->type() == QEvent::ToolTip) {
                        return true;
                    }
                } else if (m_detailsMode) {
                    m_showTimer.stop();
                    m_hideTimer.start();
                }
            } else if (event->type() == QEvent::Leave && m_detailsMode) {
                m_showTimer.stop();
                m_hideTimer.start();
            } else if (event->type() == QEvent::Hide || event->type() == QEvent::Resize) {
                dismiss();
            }
        } else if (auto *bar = qobject_cast<QProgressBar *>(watched)) {
            if (event->type() == QEvent::Enter) {
                beginPreview(bar, bar->property("copyPreviewHostId").toString());
            } else if (event->type() == QEvent::Leave && m_anchor == bar) {
                m_showTimer.stop();
                m_hideTimer.start();
            }
        }
        return QFrame::eventFilter(watched, event);
    }

    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::Enter) {
            m_hideTimer.stop();
        } else if (event->type() == QEvent::Leave) {
            m_hideTimer.start();
        }
        return QFrame::event(event);
    }

private:
    void beginPreview(QWidget *anchor, const QString &hostId, const QModelIndex &detailIndex = QModelIndex())
    {
        m_hideTimer.stop();
        if (m_anchor != anchor || m_hostId != hostId || m_detailIndex != detailIndex) {
            m_showTimer.stop();
            hide();
            m_text->clear();
            m_anchor = anchor;
            m_hostId = hostId;
            m_detailIndex = detailIndex;
            m_detailsMode = detailIndex.isValid();
        }
        if (!isVisible() && !m_showTimer.isActive()) {
            m_showTimer.start();
        }
    }

    QRect anchorRect() const
    {
        if (m_anchor == nullptr) {
            return QRect();
        }
        return m_detailsMode
            ? (m_table != nullptr && m_detailIndex.isValid() ? m_table->visualRect(m_detailIndex) : QRect())
            : m_anchor->rect();
    }

    bool anchorContainsCursor() const
    {
        if (m_anchor == nullptr || !m_anchor->isVisible() || !m_anchor->isEnabled()) {
            return false;
        }
        const QPoint position = m_anchor->mapFromGlobal(QCursor::pos());
        if (!anchorRect().contains(position)) {
            return false;
        }
        return !m_detailsMode || (m_table != nullptr && m_table->indexAt(position) == m_detailIndex);
    }

    void updateContents()
    {
        if (m_anchor == nullptr) {
            return;
        }
        QString text;
        if (m_detailsMode) {
            m_title->setText("同步详情");
            const QTableWidgetItem *item = m_table != nullptr && m_detailIndex.isValid()
                ? m_table->item(m_detailIndex.row(), 6) : nullptr;
            text = m_details.value(m_hostId, item != nullptr ? item->toolTip() : QString());
        } else {
            const QStringList files = m_files.value(m_hostId);
            m_title->setText(m_titles.value(m_hostId, "本轮拷贝预览"));
            text = files.isEmpty()
                ? (m_titles.contains(m_hostId) ? QString("本轮未产生文件处理记录") : QString("暂无文件记录"))
                : files.join("\n\n");
        }
        if (m_text->toPlainText() == text) {
            return;
        }
        // 阅读旧记录时保持滚动位置，避免批量更新把视图跳回顶部。
        const int scroll = m_text->verticalScrollBar()->value();
        m_text->setPlainText(text);
        m_text->verticalScrollBar()->setValue(scroll);
    }

    QLabel *m_title = nullptr;
    QPlainTextEdit *m_text = nullptr;
    QPointer<QWidget> m_anchor;
    QPointer<QTableWidget> m_table;
    QPersistentModelIndex m_detailIndex;
    QString m_hostId;
    bool m_detailsMode = false;
    QHash<QString, QStringList> m_files;
    QHash<QString, QString> m_titles;
    QHash<QString, QString> m_details;
    QTimer m_showTimer;
    QTimer m_hideTimer;
};

namespace
{
QString normalizePathForUi(const QString &value)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(value.trimmed())));
}

QString defaultHostNameFromPath(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    if (!native.startsWith("\\\\")) {
        return QFileInfo(native).fileName();
    }

    const QStringList parts = native.mid(2).split('\\', QString::SkipEmptyParts);
    return parts.isEmpty() ? path : parts.first();
}

QString hostNameFromSharePath(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path.trimmed());
    if (native.startsWith("\\\\")) {
        const QStringList parts = native.mid(2).split('\\', QString::SkipEmptyParts);
        if (!parts.isEmpty()) {
            return parts.first();
        }
    }

    return defaultHostNameFromPath(path);
}

QString hostDisplayText(const HostEntry &host)
{
    if (!host.name.trimmed().isEmpty()) {
        return host.name.trimmed();
    }

    return defaultHostNameFromPath(host.sharePath);
}

QString sharePathTooltip(const QString &sharePath)
{
    return QString("Ctrl+点击打开共享目录；双击复制主机名\n%1").arg(sharePath);
}

static const int kMaxReleaseDirHistory = 20;
static const int kUpdateCheckIntervalMs = 2 * 60 * 60 * 1000;
static const char *kUpdateApiUrl = "https://api.github.com/repos/UUxxXxxUU/SyncTool/contents/update.json?ref=master";
static const char *kUpdateInfoUrl = "https://raw.githubusercontent.com/UUxxXxxUU/SyncTool/master/update.json";
static const char *kReleaseListUrl = "https://github.com/UUxxXxxUU/SyncTool/releases";
static const char *kReleaseTagUrl = "https://github.com/UUxxXxxUU/SyncTool/releases/tag/v%1";
static const char *kInstallerUrl = "https://github.com/UUxxXxxUU/SyncTool/releases/download/v%1/RemoteReleaseSync-%1-windows-x64-setup.exe";
static const int kInstallerStallTimeoutMs = 60000;
static const qint64 kMaxInstallerBytes = 512LL * 1024 * 1024;
static const int kInstallDialogResult = 2;
static const char *kUpdateButtonStyle = "QPushButton { background-color: %1; color: #17324d; border: 1px solid #72a9df; border-radius: 3px; padding: 4px 10px; }";
#ifdef Q_OS_WIN
static const char *kAutoStartRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const char *kAutoStartValueName = "RemoteReleaseSync";
#endif

QString startupCommand()
{
#ifdef Q_OS_WIN
    return QString("\"%1\" --startup").arg(QDir::toNativeSeparators(QCoreApplication::applicationFilePath()));
#else
    return QString();
#endif
}

QString updateDownloadDir()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath("RemoteReleaseSync-update");
}

bool isDirectoryWritable(const QString &dirPath)
{
    QFile probe(QDir(dirPath).filePath(QString(".write-test-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces))));
    if (!probe.open(QIODevice::WriteOnly)) {
        return false;
    }
    probe.close();
    probe.remove();
    return true;
}

QUrl optionalHttpsUrl(const QJsonObject &info, const QString &key, const QUrl &fallback)
{
    if (!info.value(key).isString()) {
        return fallback;
    }
    const QUrl url(info.value(key).toString().trimmed(), QUrl::StrictMode);
    return url.isValid() && url.scheme() == "https" && !url.host().isEmpty() ? url : fallback;
}

QNetworkRequest makeDownloadRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("User-Agent", "RemoteReleaseSync");
    return request;
}

QString describeDownloadError(QNetworkReply *reply)
{
    if (reply->property("timedOut").toBool()) {
        return QString("下载超时，请检查网络后重试。");
    }
    if (reply->property("tooLarge").toBool()) {
        return QString("安装包大小异常，已停止下载。");
    }
    if (reply->property("writeFailed").toBool()) {
        return QString("写入临时文件失败，请检查磁盘空间。");
    }
    const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (statusCode == 404) {
        return QString("安装包尚未发布或已被移除（HTTP 404），可前往发布页手动下载。");
    }
    if (statusCode > 0 && statusCode != 200) {
        return QString("下载服务器返回 HTTP %1。").arg(statusCode);
    }
    return QString("下载失败，请检查网络后重试。\n%1").arg(reply->errorString());
}

QString formatMegabytes(qint64 bytes)
{
    return QString::number(static_cast<double>(bytes) / (1024.0 * 1024.0), 'f', 1) + " MB";
}
}

MainWindow::MainWindow(QWidget *parent, const QString &configPath, bool manageSystemStartup)
    : QMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_configPath(configPath.isEmpty() ? ConfigManager::defaultConfigPath() : configPath)
    , m_manageSystemStartup(manageSystemStartup)
{
    setupUi();
    m_copyPreview = new RecentCopyPreview(this);
    m_copyPreview->watchDetails(m_hostTable);
    loadConfiguration();

    m_updateCheckTimer = new QTimer(this);
    m_updateCheckTimer->setTimerType(Qt::PreciseTimer);
    connect(m_updateCheckTimer, &QTimer::timeout, this, [this]() {
        startUpdateCheck(false);
    });
    m_updateCheckTimer->start(kUpdateCheckIntervalMs);
    if (m_config.checkUpdatesOnStartup) {
        QTimer::singleShot(3000, this, [this]() {
            startUpdateCheck(false);
        });
    }

    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollForChanges);
    updateTimerInterval();
    m_pollTimer->start();

    auto *lastSyncTimer = new QTimer(this);
    connect(lastSyncTimer, &QTimer::timeout, this, &MainWindow::refreshLastSyncTimes);
    lastSyncTimer->start(1000);

    statusBar()->showMessage(QString("配置文件：%1").arg(QDir::toNativeSeparators(m_configPath)), 5000);
    QTimer::singleShot(300, this, [this]() {
        if (!m_backgroundTasksPaused) {
            startOperations(m_autoSyncCheck->isChecked());
        }
    });

    // ===== 系统托盘 =====
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QApplication::windowIcon());
    m_trayIcon->setToolTip("远程 Release 同步工具");

    m_trayMenu = new QMenu(this);
    m_showAction = m_trayMenu->addAction("显示主界面");
    connect(m_showAction, &QAction::triggered, this, [this]() {
        showNormal();
        activateWindow();
        raise();
    });
    m_trayMenu->addSeparator();
    m_exitAction = m_trayMenu->addAction("退出");
    connect(m_exitAction, &QAction::triggered, this, &MainWindow::realExit);

    m_trayIcon->setContextMenu(m_trayMenu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this, &MainWindow::trayIconActivated);

    m_trayIcon->show();

    if (QCoreApplication::arguments().contains("--updated")) {
        m_trayIcon->showMessage("远程 Release 同步工具",
                                QString("已更新到 v%1。").arg(QCoreApplication::applicationVersion()),
                                QSystemTrayIcon::Information,
                                5000);
    }
    // 安装程序重启本程序后会自行退出，稍后再清理上次下载的安装包。
    QTimer::singleShot(60000, this, [this]() {
        if (m_installerReply == nullptr && m_readyInstallerPath.isEmpty()) {
            QDir(updateDownloadDir()).removeRecursively();
        }
    });
}

MainWindow::~MainWindow()
{
    m_isClosing = true;
    stopUpdateChecks();
    requestWorkersToStop();

    for (auto it = m_runningThreads.constBegin(); it != m_runningThreads.constEnd(); ++it) {
        QThread *thread = it.value();
        if (thread != nullptr && thread->isRunning()) {
            thread->quit();
            thread->wait();
        }
    }
    SyncWorker::releaseShareConnections();
    // QWidget 析构隐藏窗口时仍会发送焦点/选择信号；先断开，避免访问已析构的配置和 Ui 成员。
    for (QObject *child : findChildren<QObject *>()) {
        QObject::disconnect(child, nullptr, this, nullptr);
    }
}

bool MainWindow::event(QEvent *event)
{
    const bool handled = QMainWindow::event(event);
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide
        || event->type() == QEvent::WindowStateChange) {
        updateCheckButton();
    }
    return handled;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if ((QApplication::keyboardModifiers() & Qt::ControlModifier) != 0) {
        event->ignore();
        realExit();
        return;
    }

    // 点击关闭按钮默认为隐藏到托盘
    hide();
    m_trayIcon->showMessage("远程 Release 同步工具",
                             "程序已最小化到系统托盘，右键点击图标可退出。",
                             QSystemTrayIcon::Information,
                             3000);
    event->ignore();
}

void MainWindow::trayIconActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::DoubleClick) {
        showNormal();
        activateWindow();
        raise();
    }
}

void MainWindow::realExit()
{
    if (m_exitFinished) {
        return;
    }

    m_isClosing = true;
    stopUpdateChecks();
    setEnabled(false);

    // 停止定时器，防止关闭过程中触发新的检查
    if (m_pollTimer != nullptr) {
        m_pollTimer->stop();
    }

    m_pendingSyncAfterCurrentRun = false;
    m_pendingSyncStatusMessage.clear();

    if (m_exitAction != nullptr) {
        m_exitAction->setEnabled(false);
    }
    if (m_showAction != nullptr) {
        m_showAction->setEnabled(false);
    }

    // 取消所有进行中的工作线程，等线程自然结束后再退出
    if (hasRunningWorkers()) {
        statusBar()->showMessage("正在停止后台任务...");
        if (m_trayIcon != nullptr) {
            m_trayIcon->showMessage("远程 Release 同步工具",
                                    "正在停止后台任务，完成后会自动退出。",
                                    QSystemTrayIcon::Information,
                                    3000);
        }

        requestWorkersToStop();
        return;
    }

    finishExit();
}

void MainWindow::requestWorkerToStop(const QString &hostId)
{
    const auto cancelled = m_cancellationFlags.value(hostId);
    if (!cancelled) {
        return;
    }

    // 取消标记独立于工作对象存活，避免线程结束时跨线程访问已销毁的对象。
    cancelled->store(true);
    m_stoppingHostIds.insert(hostId);
    for (int row = 0; row < m_config.hosts.size(); ++row) {
        if (m_config.hosts.at(row).id == hostId) {
            const RowWidgets widgets = m_rowWidgets.value(hostId);
            const int progress = widgets.progressBar != nullptr ? widgets.progressBar->value() : 0;
            updateHostRow(row, SyncWorker::Stopping, 0, "正在停止", "正在等待后台任务停止...", QString(), progress);
            break;
        }
    }
}

void MainWindow::requestWorkersToStop()
{
    for (auto it = m_runningThreads.constBegin(); it != m_runningThreads.constEnd(); ++it) {
        requestWorkerToStop(it.key());
    }
}

void MainWindow::finishExit()
{
    if (m_exitFinished) {
        return;
    }

    m_exitFinished = true;

    // 保存配置
    saveConfiguration();

    // 隐藏托盘图标
    if (m_trayIcon != nullptr) {
        m_trayIcon->hide();
    }

    // 真正退出
    QApplication::quit();
}

void MainWindow::setupUi()
{
    m_ui->setupUi(this);
    setWindowTitle(QString("远程 Release 同步工具 v%1").arg(QCoreApplication::applicationVersion()));
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        resize(QSize(1280, 860).boundedTo(screen->availableGeometry().size() - QSize(40, 60)));
    }
    m_ui->settingsScroll->setMinimumHeight(qMin(m_ui->settingsContent->sizeHint().height(), qMax(200, height() - 230)));
    m_releaseDirEdit = m_ui->releaseDirEdit;
    m_typeList = m_ui->typeList;
    m_newTypeEdit = m_ui->newTypeEdit;
    m_autoSyncCheck = m_ui->autoSyncCheck;
    m_startWithSystemCheck = m_ui->startWithSystemCheck;
    m_intervalSpin = m_ui->intervalSpin;
    m_hostEnabledCheck = m_ui->hostEnabledCheck;
    m_hostNameEdit = m_ui->hostNameEdit;
    m_hostPathEdit = m_ui->hostPathEdit;
    m_hostUserEdit = m_ui->hostUserEdit;
    m_hostPasswordEdit = m_ui->hostPasswordEdit;
    m_hostTable = m_ui->hostTable;
    m_checkUpdatesOnStartupCheck = m_ui->checkUpdatesOnStartupCheck;
    m_silentUpdateCheck = m_ui->silentUpdateCheck;
    m_checkUpdatesButton = m_ui->checkUpdatesButton;
    m_releaseDirEdit->lineEdit()->setPlaceholderText("选择或输入本地 Release 目录路径...");
    m_ui->stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    for (int column : {0, 1, 3, 4, 5}) {
        m_hostTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_hostTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_hostTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_hostTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Fixed);
    m_hostTable->setColumnWidth(7, 160);
    m_ui->ruleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_ui->ruleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_ui->ruleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    statusBar()->addPermanentWidget(m_ui->statusControls);
    addAction(m_ui->usageTipsAction);
    connect(m_ui->usageTipsButton, &QPushButton::clicked, this, &MainWindow::showUsageTips);
    connect(m_ui->usageTipsAction, &QAction::triggered, this, &MainWindow::showUsageTips);
    connect(m_checkUpdatesButton, &QPushButton::clicked, this, &MainWindow::checkForUpdates);
    connect(m_ui->addRuleButton, &QPushButton::clicked, this, [this]() { editSyncRule(false); });
    connect(m_ui->editRuleButton, &QPushButton::clicked, this, [this]() { editSyncRule(true); });
    connect(m_ui->removeRuleButton, &QPushButton::clicked, this, &MainWindow::removeSyncRule);
    connect(m_ui->ruleTable, &QTableWidget::cellDoubleClicked, this, [this](int, int) { editSyncRule(true); });
    connect(m_ui->ruleTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::updateRulePreview);
    connect(m_ui->ruleTable, &QTableWidget::itemChanged, this, [this](QTableWidgetItem *item) {
        if (item->column() != 0 || item->row() >= m_config.syncRules.size()) return;
        m_config.syncRules[item->row()].enabled = item->checkState() == Qt::Checked;
        syncRulesChanged();
    });
    connect(m_releaseDirEdit, &QComboBox::editTextChanged, this, &MainWindow::updateRulePreview);
    connect(m_hostPathEdit, &QLineEdit::textChanged, this, &MainWindow::updateRulePreview);
    connect(m_checkUpdatesOnStartupCheck, &QCheckBox::toggled, this, [this](bool) {
        saveConfiguration();
    });
    connect(m_silentUpdateCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        saveConfiguration();
        if (enabled) {
            m_silentInstallFailedVersion.clear();
            if (!m_availableUpdateVersion.isEmpty() && !isReadyInstallerCurrent()) {
                startInstallerDownload(false);
            }
        } else if (m_installerReply != nullptr && !m_installerDownloadInteractive) {
            abortInstallerDownload();
        }
    });

    m_updatePulse = new QVariantAnimation(this);
    m_updatePulse->setDuration(2400);
    m_updatePulse->setStartValue(QColor("#e8f3ff"));
    m_updatePulse->setKeyValueAt(0.5, QColor("#96c9ff"));
    m_updatePulse->setEndValue(QColor("#e8f3ff"));
    m_updatePulse->setLoopCount(-1);
    connect(m_updatePulse, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_checkUpdatesButton->setStyleSheet(QString::fromLatin1(kUpdateButtonStyle).arg(value.value<QColor>().name()));
    });
    updateCheckButton();

    connect(m_ui->browseButton, &QPushButton::clicked, this, &MainWindow::browseReleaseDir);
    connect(m_ui->addTypeButton, &QPushButton::clicked, this, &MainWindow::addFileType);
    connect(m_ui->removeTypeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedFileType);
    connect(m_typeList, &QListWidget::itemChanged, this, &MainWindow::handleTypeItemChanged);
    connect(m_ui->saveConfigButton, &QPushButton::clicked, this, &MainWindow::saveConfiguration);
    connect(m_ui->refreshButton, &QPushButton::clicked, this, [this]() {
        m_backgroundTasksPaused = false;
        startOperations(false);
    });
    connect(m_ui->syncButton, &QPushButton::clicked, this, &MainWindow::syncNow);
    connect(m_ui->stopButton, &QPushButton::clicked, this, &MainWindow::stopAllOperations);
    connect(m_ui->addHostButton, &QPushButton::clicked, this, [this]() { addOrUpdateHost(false); });
    connect(m_ui->updateHostButton, &QPushButton::clicked, this, [this]() { addOrUpdateHost(true); });
    connect(m_ui->removeHostButton, &QPushButton::clicked, this, &MainWindow::removeSelectedHost);
    connect(m_ui->clearEditorButton, &QPushButton::clicked, this, &MainWindow::clearHostEditor);
    connect(m_hostTable, &QTableWidget::itemChanged, this, &MainWindow::handleHostTableItemChanged);
    connect(m_hostTable, &QTableWidget::cellClicked, this, &MainWindow::handleHostTableCellClicked);
    connect(m_hostTable, &QTableWidget::cellDoubleClicked, this, &MainWindow::handleHostTableCellDoubleClicked);
    connect(m_hostTable, &QTableWidget::customContextMenuRequested, this, &MainWindow::showHostTableContextMenu);
    connect(m_hostTable, &QTableWidget::itemSelectionChanged, this, &MainWindow::loadSelectedHostToEditor);
    connect(m_intervalSpin,
            static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
            this,
            [this](int) {
        updateTimerInterval();
        saveConfiguration();
    });
    connect(m_autoSyncCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        if (enabled) {
            m_backgroundTasksPaused = false;
        }
        saveConfiguration();
        if (enabled) {
            requestAutoSync("已开启自动同步");
        }
    });
    connect(m_startWithSystemCheck, &QCheckBox::toggled, this, [this](bool) {
        saveConfiguration();
    });
    connect(m_releaseDirEdit->lineEdit(), &QLineEdit::editingFinished, this, &MainWindow::onReleaseDirEditFinished);
    connect(m_releaseDirEdit, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::onReleaseDirEditFinished);
}

void MainWindow::refreshRuleTable()
{
    QSignalBlocker blocker(m_ui->ruleTable);
    const int selectedRow = m_ui->ruleTable->currentRow();
    m_ui->ruleTable->setRowCount(m_config.syncRules.size());
    for (int row = 0; row < m_config.syncRules.size(); ++row) {
        const SyncRule &rule = m_config.syncRules.at(row);
        auto *enabled = new QTableWidgetItem();
        enabled->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabled->setCheckState(rule.enabled ? Qt::Checked : Qt::Unchecked);
        const QString displayPath = rule.relativePath.startsWith('.') ? rule.relativePath : "./" + rule.relativePath;
        auto *path = new QTableWidgetItem(displayPath);
        path->setToolTip(rule.relativePath);
        auto *types = new QTableWidgetItem(rule.fileTypes.isEmpty() ? QString("全部文件") : rule.fileTypes.join(';'));
        types->setToolTip(types->text());
        m_ui->ruleTable->setItem(row, 0, enabled);
        m_ui->ruleTable->setItem(row, 1, path);
        m_ui->ruleTable->setItem(row, 2, types);
    }
    if (!m_config.syncRules.isEmpty()) m_ui->ruleTable->selectRow(qBound(0, selectedRow, m_config.syncRules.size() - 1));
    m_ui->syncTabs->setTabText(1, QString("附加同步规则（%1）").arg(m_config.syncRules.size()));
    updateRulePreview();
}

void MainWindow::updateRulePreview()
{
    if (m_isClosing) return;
    const int row = m_ui->ruleTable->currentRow();
    const bool selected = row >= 0 && row < m_config.syncRules.size();
    m_ui->editRuleButton->setEnabled(selected);
    m_ui->removeRuleButton->setEnabled(selected);
    if (!selected) {
        m_ui->rulePreview->setText("路径相对于两端的 Release 目录：./image 在其内部，../Lang 在其同级。双击规则可编辑。");
        return;
    }
    const QString relative = m_config.syncRules.at(row).relativePath;
    const QString local = SyncPlan::resolvePath(m_releaseDirEdit->currentText(), relative);
    const QString remote = SyncPlan::resolvePath(m_hostPathEdit->text(), relative);
    QString error;
    const bool valid = SyncPlan::validateRemotePath(m_hostPathEdit->text(), relative, &error);
    m_ui->rulePreview->setText(QString("本地：%1\n远端：%2")
        .arg(local.isEmpty() ? QString("请先选择 Release 目录") : QDir::toNativeSeparators(local),
             !valid ? error : remote.isEmpty() ? QString("请选择主机，或在主机编辑区填写共享目录") : QDir::toNativeSeparators(remote)));
}

void MainWindow::syncRulesChanged()
{
    // 当前任务保留原规则快照，旧成功回调不能恢复已失效的同步记录。
    for (HostEntry &host : m_config.hosts) {
        host.lastSyncUtc = QDateTime();
        ++m_syncContextRevisions[host.id];
    }
    refreshLastSyncTimes();
    refreshRuleTable();
    saveConfiguration();
    m_lastFingerprint.clear();
    requestAutoSync("附加同步规则已更新，开始自动同步");
}

void MainWindow::editSyncRule(bool editExisting)
{
    const int row = m_ui->ruleTable->currentRow();
    if (editExisting && (row < 0 || row >= m_config.syncRules.size())) return;
    QDialog dialog(this);
    Ui::SyncRuleDialog ui;
    ui.setupUi(&dialog);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    dialog.setWindowTitle(editExisting ? "编辑附加同步规则" : "添加附加同步规则");
    ui.buttonBox->button(QDialogButtonBox::Save)->setText("保存");
    ui.buttonBox->button(QDialogButtonBox::Cancel)->setText("取消");
    const SyncRule initial = editExisting ? m_config.syncRules.at(row) : SyncRule();
    ui.pathEdit->setText(initial.relativePath);
    ui.typesEdit->setText(initial.fileTypes.join(';'));
    ui.enabledCheck->setChecked(initial.enabled);
    auto preview = [&]() {
        QString path, error;
        if (!SyncPlan::normalizeRelativePath(ui.pathEdit->text(), &path, &error)) {
            ui.previewLabel->setText(error);
            return;
        }
        const QString local = SyncPlan::resolvePath(m_releaseDirEdit->currentText(), path);
        const QString remote = SyncPlan::resolvePath(m_hostPathEdit->text(), path);
        const bool valid = SyncPlan::validateRemotePath(m_hostPathEdit->text(), path, &error);
        ui.previewLabel->setText(QString("本地：%1\n远端：%2")
            .arg(local.isEmpty() ? QString("请先配置本地 Release 目录") : QDir::toNativeSeparators(local),
                 !valid ? error : remote.isEmpty() ? QString("请选择主机以预览远端路径") : QDir::toNativeSeparators(remote)));
    };
    connect(ui.pathEdit, &QLineEdit::textChanged, &dialog, preview);
    auto browse = [&](bool file) {
        const QString base = m_releaseDirEdit->currentText().trimmed();
        if (base.isEmpty()) {
            QMessageBox::information(&dialog, "选择 Release 目录", "请先在主窗口设置本地 Release 目录。");
            return;
        }
        const QString selected = file ? QFileDialog::getOpenFileName(&dialog, "选择规则文件", base)
                                      : QFileDialog::getExistingDirectory(&dialog, "选择规则目录", base);
        if (!selected.isEmpty()) ui.pathEdit->setText(QDir(base).relativeFilePath(selected));
    };
    connect(ui.browseDirButton, &QPushButton::clicked, &dialog, [&]() { browse(false); });
    connect(ui.browseFileButton, &QPushButton::clicked, &dialog, [&]() { browse(true); });
    connect(ui.buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    SyncRule result;
    connect(ui.buttonBox, &QDialogButtonBox::accepted, &dialog, [&]() {
        QString error;
        const QString text = ui.typesEdit->text().trimmed();
        const QStringList types = text.isEmpty() ? QStringList() : text.split(QRegularExpression("[;,，；\\s]+"), QString::SkipEmptyParts);
        if (!SyncPlan::normalizeRelativePath(ui.pathEdit->text(), &result.relativePath, &error)
            || !SyncPlan::normalizeTypes(types, &result.fileTypes, &error)) {
            QMessageBox::warning(&dialog, "规则无效", error);
            return;
        }
        result.enabled = ui.enabledCheck->isChecked();
        if (result.enabled) {
            for (const HostEntry &host : m_config.hosts) {
                if (!SyncPlan::validateRemotePath(host.sharePath, result.relativePath, &error)) {
                    QMessageBox::warning(&dialog, "规则无效", QString("主机 %1：%2").arg(host.name, error));
                    return;
                }
            }
            QVector<SyncPlan::Scope> scopes;
            if (!SyncPlan::build(m_releaseDirEdit->currentText(), QString(), {}, {result}, &scopes, &error)) {
                QMessageBox::warning(&dialog, "规则无效", error);
                return;
            }
        }
        dialog.accept();
    });
    preview();
    if (dialog.exec() != QDialog::Accepted) return;
    if (editExisting) m_config.syncRules[row] = result;
    else m_config.syncRules.append(result);
    syncRulesChanged();
    m_ui->ruleTable->selectRow(editExisting ? row : m_config.syncRules.size() - 1);
}

void MainWindow::removeSyncRule()
{
    const int row = m_ui->ruleTable->currentRow();
    if (row < 0 || row >= m_config.syncRules.size()) return;
    m_config.syncRules.removeAt(row);
    syncRulesChanged();
}

void MainWindow::showUsageTips()
{
    if (m_isClosing) {
        return;
    }
    if (m_usageTipsDialog != nullptr) {
        m_usageTipsDialog->showNormal();
        m_usageTipsDialog->raise();
        m_usageTipsDialog->activateWindow();
        return;
    }

    auto *dialog = new QDialog(this);
    m_usageTipsDialog = dialog;
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("使用技巧 · 快捷操作与隐藏功能");
    dialog->setWindowFlags(dialog->windowFlags() & ~Qt::WindowContextHelpButtonHint);
    QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    const QSize available = screen != nullptr
        ? screen->availableGeometry().size() - QSize(64, 80) : QSize(1080, 760);
    dialog->resize(QSize(1080, 760).boundedTo(available));
    Ui::UsageTipsDialog ui;
    ui.setupUi(dialog);
    ui.buttonBox->button(QDialogButtonBox::Ok)->setText("知道了");
    connect(ui.buttonBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    dialog->show();
}

void MainWindow::checkForUpdates()
{
    if (m_isClosing) {
        return;
    }
    if (!m_availableUpdateVersion.isEmpty()) {
        showAvailableUpdate();
        return;
    }
    startUpdateCheck(true);
}

void MainWindow::startUpdateCheck(bool interactive)
{
    if (m_isClosing) {
        return;
    }
    if (m_updateCheckInProgress) {
        m_updateCheckInteractive = m_updateCheckInteractive || interactive;
        return;
    }
    m_updateCheckInProgress = true;
    m_updateCheckInteractive = interactive;
    updateCheckButton();
    if (!QSslSocket::supportsSsl()) {
        finishUpdateCheck("当前发布包缺少 HTTPS 支持，请补齐与 Qt 配套的 TLS 运行库。");
        return;
    }

    if (m_updateNetworkManager == nullptr) {
        m_updateNetworkManager = new QNetworkAccessManager(this);
    }
    requestUpdateInfo(false);
}

void MainWindow::requestUpdateInfo(bool useRawFallback)
{
    if (m_isClosing || !m_updateCheckInProgress || m_updateNetworkManager == nullptr) {
        return;
    }
    QNetworkRequest request(QUrl(QString::fromLatin1(useRawFallback ? kUpdateInfoUrl : kUpdateApiUrl)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Accept", useRawFallback ? "application/json" : "application/vnd.github+json");
    request.setRawHeader("User-Agent", "RemoteReleaseSync");
    QNetworkReply *reply = m_updateNetworkManager->get(request);
    m_updateReply = reply;

    // Qt 5.12 没有请求超时接口，用单次定时器中止迟迟未完成的请求。
    QTimer *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    timeout->setTimerType(Qt::PreciseTimer);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeout, useRawFallback]() {
        timeout->stop();
        m_updateReply.clear();
        reply->deleteLater();
        if (m_isClosing) {
            return;
        }
        const auto failCheck = [this, useRawFallback](const QString &reason) {
            if (useRawFallback) {
                finishUpdateCheck(reason);
            } else {
                requestUpdateInfo(true);
            }
        };

        if (reply->error() != QNetworkReply::NoError) {
            const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString reason = reply->error() == QNetworkReply::OperationCanceledError
                ? QString("检查超时，请稍后重试。")
                : (statusCode > 0 ? QString("更新服务器返回 HTTP %1。").arg(statusCode)
                                  : QString("无法获取更新信息，请检查网络后重试。\n%1").arg(reply->errorString()));
            failCheck(reason);
            return;
        }
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            failCheck(QString("更新服务器返回 HTTP %1。").arg(statusCode));
            return;
        }

        QByteArray payload = reply->readAll();
        if (!useRawFallback) {
            QJsonParseError apiParseError;
            const QJsonDocument apiDocument = QJsonDocument::fromJson(payload, &apiParseError);
            const QJsonObject apiInfo = apiDocument.object();
            if (apiParseError.error != QJsonParseError::NoError || !apiDocument.isObject()
                || apiInfo.value("encoding").toString() != "base64" || !apiInfo.value("content").isString()) {
                failCheck("更新接口未返回有效的文件内容。");
                return;
            }
            // Contents API 返回 Base64 文件内容，解码后与 raw 共用同一套清单校验。
            payload = QByteArray::fromBase64(apiInfo.value("content").toString().toLatin1());
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            failCheck("更新信息不是有效的 JSON 对象。");
            return;
        }
        const QJsonObject info = document.object();
        if (!info.value("version").isString() || !info.value("notes").isString()
            || !info.value("downloadUrl").isString()) {
            failCheck("更新信息缺少有效的 version、notes 或 downloadUrl 字段。");
            return;
        }

        const QString latestVersionText = info.value("version").toString().trimmed();
        const QString currentVersionText = QCoreApplication::applicationVersion();
        const QRegularExpression versionPattern("\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z");
        const QVersionNumber latestVersion = QVersionNumber::fromString(latestVersionText);
        const QVersionNumber currentVersion = QVersionNumber::fromString(currentVersionText);
        if (!versionPattern.match(latestVersionText).hasMatch() || latestVersion.segmentCount() != 3
            || !versionPattern.match(currentVersionText).hasMatch() || currentVersion.segmentCount() != 3) {
            failCheck("版本号必须为三段数字，例如 1.0.0。");
            return;
        }
        const QUrl downloadUrl(info.value("downloadUrl").toString().trimmed(), QUrl::StrictMode);
        if (!downloadUrl.isValid() || downloadUrl.scheme() != "https" || downloadUrl.host().isEmpty()) {
            failCheck("更新信息中的下载地址必须为有效的 HTTPS 地址。");
            return;
        }
        // sections、releaseUrl、installerUrl 为可选字段，缺失或无效时使用默认值，兼容旧格式清单。
        QVector<UpdateNoteSection> sections;
        const QJsonArray sectionArray = info.value("sections").toArray();
        for (const QJsonValue &sectionValue : sectionArray) {
            const QJsonObject sectionObject = sectionValue.toObject();
            UpdateNoteSection section;
            section.title = sectionObject.value("title").toString().trimmed();
            const QJsonArray itemArray = sectionObject.value("items").toArray();
            for (const QJsonValue &itemValue : itemArray) {
                const QString item = itemValue.toString().trimmed();
                if (!item.isEmpty()) {
                    section.items.append(item);
                }
            }
            if (!section.title.isEmpty() && !section.items.isEmpty()) {
                sections.append(section);
            }
        }
        // 仅完整校验成功的结果可以替换缓存，断网或清单异常时保留已发现的新版本。
        if (QVersionNumber::compare(latestVersion, currentVersion) > 0) {
            m_availableUpdateVersion = latestVersionText;
            m_availableUpdateNotes = info.value("notes").toString().trimmed();
            m_availableUpdateUrl = downloadUrl;
            m_availableReleaseUrl = optionalHttpsUrl(info, "releaseUrl",
                                                     QUrl(QString::fromLatin1(kReleaseTagUrl).arg(latestVersionText)));
            m_availableInstallerUrl = optionalHttpsUrl(info, "installerUrl",
                                                       QUrl(QString::fromLatin1(kInstallerUrl).arg(latestVersionText)));
            m_availableUpdateSections = sections;
        } else {
            m_availableUpdateVersion.clear();
            m_availableUpdateNotes.clear();
            m_availableUpdateUrl = QUrl();
            m_availableReleaseUrl = QUrl();
            m_availableInstallerUrl = QUrl();
            m_availableUpdateSections.clear();
        }
        finishUpdateCheck();
    });
    timeout->start(10000);
}

void MainWindow::finishUpdateCheck(const QString &errorMessage)
{
    const bool interactive = m_updateCheckInteractive;
    m_updateCheckInProgress = false;
    m_updateCheckInteractive = false;
    updateCheckButton();
    if (m_isClosing) {
        return;
    }
    if (errorMessage.isEmpty() && !m_availableUpdateVersion.isEmpty() && m_silentUpdateCheck->isChecked()
        && !isReadyInstallerCurrent() && m_installerDownloadVersion != m_availableUpdateVersion) {
        startInstallerDownload(false);
    }
    if (!interactive) {
        return;
    }
    if (!errorMessage.isEmpty()) {
        QMessageBox::warning(this, "检查更新失败", errorMessage);
    } else if (m_availableUpdateVersion.isEmpty()) {
        showUpdateHistoryLink(QString("当前已是最新版本（%1）。").arg(QCoreApplication::applicationVersion()));
    } else {
        showAvailableUpdate();
    }
}

void MainWindow::showAvailableUpdate()
{
    if (m_isClosing || m_updateDialogOpen || m_availableUpdateVersion.isEmpty()) {
        return;
    }
    // 对话框会运行嵌套事件循环，保留本次展示的内容，避免后台结果替换显示信息。
    const QString version = m_availableUpdateVersion;
    const QString notes = m_availableUpdateNotes;
    const QVector<UpdateNoteSection> sections = m_availableUpdateSections;
    const QUrl historyUrl = m_availableUpdateUrl;
    const QUrl releaseUrl = m_availableReleaseUrl.isValid() ? m_availableReleaseUrl : m_availableUpdateUrl;
    m_updateDialogOpen = true;
    updateCheckButton();

    QDialog dialog(this);
    Ui::UpdateDialog ui;
    ui.setupUi(&dialog);
    dialog.setWindowFlags(dialog.windowFlags() & ~Qt::WindowContextHelpButtonHint);
    ui.titleLabel->setText(QString("新版本 v%1").arg(version));
    ui.currentVersionLabel->setText(QString("当前版本 v%1").arg(QCoreApplication::applicationVersion()));
    auto *notesWidget = ui.notesWidget;
    auto *notesLayout = ui.notesLayout;
    auto *releasePageButton = ui.releasePageButton;
    if (!notes.isEmpty() || sections.isEmpty()) {
        auto *summaryLabel = new QLabel(notes.isEmpty() ? QString("暂无更新说明。") : notes, notesWidget);
        summaryLabel->setTextFormat(Qt::PlainText);
        summaryLabel->setWordWrap(true);
        summaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        if (!sections.isEmpty()) {
            summaryLabel->setStyleSheet("color: #4a5561;");
        }
        notesLayout->addWidget(summaryLabel);
    }
    for (const UpdateNoteSection &section : sections) {
        auto *sectionButton = new QToolButton(notesWidget);
        sectionButton->setObjectName("sectionButton");
        sectionButton->setText(QString("%1（%2）").arg(section.title).arg(section.items.size()));
        sectionButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        sectionButton->setArrowType(Qt::RightArrow);
        sectionButton->setCheckable(true);
        sectionButton->setAutoRaise(true);
        sectionButton->setCursor(Qt::PointingHandCursor);
        QFont sectionFont = sectionButton->font();
        sectionFont.setPointSize(sectionFont.pointSize() + 1);
        sectionFont.setBold(true);
        sectionButton->setFont(sectionFont);

        QStringList itemsHtml;
        for (const QString &item : section.items) {
            itemsHtml.append(QString("<li>%1</li>").arg(item.toHtmlEscaped()));
        }
        auto *itemsLabel = new QLabel(QString("<ul style=\"margin-top:0px; margin-bottom:0px; -qt-list-indent:1;\">%1</ul>")
                                          .arg(itemsHtml.join(QString())),
                                      notesWidget);
        itemsLabel->setTextFormat(Qt::RichText);
        itemsLabel->setWordWrap(true);
        itemsLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        itemsLabel->setContentsMargins(18, 0, 0, 4);
        itemsLabel->setVisible(false);
        connect(sectionButton, &QToolButton::toggled, itemsLabel, [sectionButton, itemsLabel](bool expanded) {
            sectionButton->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
            itemsLabel->setVisible(expanded);
        });
        notesLayout->addWidget(sectionButton, 0, Qt::AlignLeft);
        notesLayout->addWidget(itemsLabel);
    }
    notesLayout->addStretch(1);
    connect(ui.linksLabel, &QLabel::linkActivated, this, [this, releaseUrl, historyUrl](const QString &link) {
        openUpdateUrl(link == "history" ? historyUrl : releaseUrl);
    });
    auto *progressBar = ui.progressBar;
    auto *statusLabel = ui.statusLabel;
    auto *cancelButton = ui.cancelButton;
    auto *installButton = ui.installButton;
    m_updateDialog = &dialog;
    m_updateDialogProgress = progressBar;
    m_updateDialogStatus = statusLabel;
    m_updateDialogInstallButton = installButton;
    if (isReadyInstallerCurrent()) {
        setUpdateDialogStatus("安装包已下载并通过校验，点击“更新”立即安装，程序会自动重启。", 100);
    } else if (m_installerReply != nullptr) {
        setUpdateDialogStatus("正在后台下载安装包...");
    } else {
        setUpdateDialogStatus("点击“更新”下载安装包，安装完成后程序会自动重启。", 0);
    }

    connect(releasePageButton, &QPushButton::clicked, this, [this, releaseUrl]() {
        openUpdateUrl(releaseUrl);
    });
    connect(cancelButton, &QPushButton::clicked, &dialog, &QDialog::reject);
    connect(installButton, &QPushButton::clicked, &dialog, [this, &dialog, installButton]() {
        if (isReadyInstallerCurrent()) {
            dialog.done(kInstallDialogResult);
            return;
        }
        m_installAfterDownload = true;
        installButton->setEnabled(false);
        installButton->setText("下载中...");
        startInstallerDownload(true);
    });

    const int result = dialog.exec();
    m_updateDialog.clear();
    m_updateDialogProgress.clear();
    m_updateDialogStatus.clear();
    m_updateDialogInstallButton.clear();
    m_updateDialogOpen = false;
    m_dismissedUpdateVersion = version;
    if (result != kInstallDialogResult) {
        m_installAfterDownload = false;
        // 用户取消时停止由对话框发起的下载；开启无感更新时保留后台下载。
        if (m_installerReply != nullptr && m_installerDownloadInteractive && !m_silentUpdateCheck->isChecked()) {
            abortInstallerDownload();
        }
        m_installerDownloadInteractive = false;
    }
    updateCheckButton();
    if (!m_isClosing && result == kInstallDialogResult) {
        installUpdateNow();
    }
}

void MainWindow::openUpdateUrl(const QUrl &url)
{
    const QUrl target = url.isValid() ? url : QUrl(QString::fromLatin1(kReleaseListUrl));
    if (!QDesktopServices::openUrl(target)) {
        QMessageBox::warning(this, "打开页面失败", QString("请在浏览器中访问：\n%1").arg(target.toString()));
    }
}

void MainWindow::showUpdateHistoryLink(const QString &message)
{
    QMessageBox box(this);
    box.setWindowTitle("检查更新");
    box.setIcon(QMessageBox::Information);
    box.setText(message);
    box.addButton("确定", QMessageBox::AcceptRole);
    QPushButton *historyButton = box.addButton("历史版本下载", QMessageBox::ActionRole);
    box.exec();
    if (!m_isClosing && box.clickedButton() == historyButton) {
        openUpdateUrl(QUrl(QString::fromLatin1(kReleaseListUrl)));
    }
}

bool MainWindow::isReadyInstallerCurrent() const
{
    return !m_readyInstallerPath.isEmpty() && !m_availableUpdateVersion.isEmpty()
        && m_readyInstallerVersion == m_availableUpdateVersion && QFileInfo::exists(m_readyInstallerPath);
}

void MainWindow::setUpdateDialogStatus(const QString &text, int percent)
{
    if (m_updateDialogStatus != nullptr) {
        m_updateDialogStatus->setText(text);
    }
    if (percent >= 0 && m_updateDialogProgress != nullptr) {
        m_updateDialogProgress->setValue(qBound(0, percent, 100));
    }
}

void MainWindow::startInstallerDownload(bool interactive)
{
    if (m_isClosing || m_availableUpdateVersion.isEmpty() || !m_availableInstallerUrl.isValid()) {
        return;
    }
    if (isReadyInstallerCurrent()) {
        m_installerDownloadInteractive = m_installerDownloadInteractive || interactive;
        finishInstallerDownload();
        return;
    }
    if (m_installerReply != nullptr && m_installerDownloadVersion == m_availableUpdateVersion) {
        m_installerDownloadInteractive = m_installerDownloadInteractive || interactive;
        return;
    }

    const bool installAfterDownload = m_installAfterDownload;
    abortInstallerDownload();
    m_installAfterDownload = installAfterDownload;
    m_installerDownloadVersion = m_availableUpdateVersion;
    m_installerDownloadUrl = m_availableInstallerUrl;
    m_installerDownloadInteractive = interactive;
    if (!QSslSocket::supportsSsl()) {
        failInstallerDownload("当前发布包缺少 HTTPS 支持，请补齐与 Qt 配套的 TLS 运行库。");
        return;
    }
    if (m_updateNetworkManager == nullptr) {
        m_updateNetworkManager = new QNetworkAccessManager(this);
    }
    setUpdateDialogStatus("正在获取安装包校验信息...", 0);
    requestInstallerChecksum();
}

void MainWindow::requestInstallerChecksum()
{
    const QUrl checksumUrl(m_installerDownloadUrl.toString() + ".sha256");
    QNetworkReply *reply = m_updateNetworkManager->get(makeDownloadRequest(checksumUrl));
    m_installerReply = reply;

    QTimer *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("timedOut", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeout]() {
        timeout->stop();
        reply->deleteLater();
        if (m_installerReply != reply) {
            return;
        }
        m_installerReply.clear();
        if (m_isClosing) {
            return;
        }
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || statusCode != 200) {
            failInstallerDownload(describeDownloadError(reply));
            return;
        }
        // 校验文件格式为“摘要 *文件名”，只取开头的 64 位十六进制 SHA256。
        const QString content = QString::fromLatin1(reply->read(4096));
        const QRegularExpressionMatch match = QRegularExpression("\\A\\s*([0-9A-Fa-f]{64})(\\s|\\z)").match(content);
        if (!match.hasMatch()) {
            failInstallerDownload("安装包校验文件格式无效。");
            return;
        }
        requestInstallerFile(match.captured(1).toLower());
    });
    timeout->start(15000);
}

void MainWindow::requestInstallerFile(const QString &expectedSha256)
{
    const QString dirPath = updateDownloadDir();
    if (!QDir().mkpath(dirPath)) {
        failInstallerDownload(QString("无法创建临时目录：%1").arg(QDir::toNativeSeparators(dirPath)));
        return;
    }
    const QString partPath = QDir(dirPath).filePath(
        QString("RemoteReleaseSync-%1-windows-x64-setup.exe.part").arg(m_installerDownloadVersion));
    QFile::remove(partPath);
    m_installerFile = new QFile(partPath, this);
    if (!m_installerFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        failInstallerDownload(QString("无法写入临时文件：%1").arg(m_installerFile->errorString()));
        return;
    }
    m_installerHash.reset(new QCryptographicHash(QCryptographicHash::Sha256));

    QNetworkReply *reply = m_updateNetworkManager->get(makeDownloadRequest(m_installerDownloadUrl));
    m_installerReply = reply;
    setUpdateDialogStatus("正在下载安装包...", 0);

    // 只在长时间没有收到数据时中止，避免大文件在慢速网络下被误判超时。
    QTimer *stallTimer = new QTimer(reply);
    stallTimer->setSingleShot(true);
    connect(stallTimer, &QTimer::timeout, reply, [reply]() {
        reply->setProperty("timedOut", true);
        reply->abort();
    });
    connect(reply, &QNetworkReply::readyRead, this, [this, reply]() {
        appendInstallerChunk(reply);
    });
    connect(reply, &QNetworkReply::downloadProgress, this, [this, stallTimer](qint64 received, qint64 total) {
        stallTimer->start(kInstallerStallTimeoutMs);
        if (total > 0) {
            setUpdateDialogStatus(QString("正在下载安装包 %1 / %2").arg(formatMegabytes(received), formatMegabytes(total)),
                                  static_cast<int>(received * 100 / total));
        } else {
            setUpdateDialogStatus(QString("正在下载安装包 %1").arg(formatMegabytes(received)));
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, stallTimer, expectedSha256]() {
        stallTimer->stop();
        reply->deleteLater();
        if (m_installerReply != reply) {
            return;
        }
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const bool succeeded = reply->error() == QNetworkReply::NoError && statusCode == 200 && appendInstallerChunk(reply);
        m_installerReply.clear();
        if (m_isClosing) {
            abortInstallerDownload();
            return;
        }
        if (!succeeded) {
            failInstallerDownload(describeDownloadError(reply));
            return;
        }

        const QString partPath = m_installerFile->fileName();
        const bool flushed = m_installerFile->flush();
        m_installerFile->close();
        delete m_installerFile;
        m_installerFile = nullptr;
        const QString actualSha256 = QString::fromLatin1(m_installerHash->result().toHex());
        m_installerHash.reset();
        if (!flushed) {
            QFile::remove(partPath);
            failInstallerDownload("写入临时文件失败，请检查磁盘空间。");
            return;
        }
        if (actualSha256 != expectedSha256) {
            QFile::remove(partPath);
            failInstallerDownload("安装包 SHA256 校验失败，已删除下载的文件，请稍后重试。");
            return;
        }
        const QString finalPath = partPath.left(partPath.size() - QString(".part").size());
        QFile::remove(finalPath);
        if (!QFile::rename(partPath, finalPath)) {
            QFile::remove(partPath);
            failInstallerDownload(QString("无法保存安装包：%1").arg(QDir::toNativeSeparators(finalPath)));
            return;
        }
        m_readyInstallerPath = finalPath;
        m_readyInstallerVersion = m_installerDownloadVersion;
        finishInstallerDownload();
    });
    stallTimer->start(kInstallerStallTimeoutMs);
}

bool MainWindow::appendInstallerChunk(QNetworkReply *reply)
{
    if (m_installerReply != reply || m_installerFile == nullptr || !m_installerHash) {
        return false;
    }
    // 自动跟随重定向时，跳转响应的内容不属于安装包。
    if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
        reply->readAll();
        return true;
    }
    const QByteArray chunk = reply->readAll();
    if (chunk.isEmpty()) {
        return true;
    }
    if (m_installerFile->size() + chunk.size() > kMaxInstallerBytes) {
        reply->setProperty("tooLarge", true);
        reply->abort();
        return false;
    }
    if (m_installerFile->write(chunk) != chunk.size()) {
        reply->setProperty("writeFailed", true);
        reply->abort();
        return false;
    }
    m_installerHash->addData(chunk);
    return true;
}

void MainWindow::abortInstallerDownload()
{
    if (m_installerReply != nullptr) {
        QNetworkReply *reply = m_installerReply;
        m_installerReply.clear();
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
    }
    if (m_installerFile != nullptr) {
        const QString partPath = m_installerFile->fileName();
        m_installerFile->close();
        delete m_installerFile;
        m_installerFile = nullptr;
        QFile::remove(partPath);
    }
    m_installerHash.reset();
    m_installerDownloadVersion.clear();
    m_installerDownloadUrl = QUrl();
    m_installerDownloadInteractive = false;
    m_installAfterDownload = false;
}

void MainWindow::failInstallerDownload(const QString &errorMessage)
{
    const bool interactive = m_installerDownloadInteractive;
    abortInstallerDownload();
    if (m_isClosing) {
        return;
    }
    if (m_updateDialog != nullptr) {
        setUpdateDialogStatus(QString("下载失败：%1").arg(errorMessage), 0);
        if (m_updateDialogInstallButton != nullptr) {
            m_updateDialogInstallButton->setEnabled(true);
            m_updateDialogInstallButton->setText("重试");
        }
    } else if (interactive) {
        QMessageBox::warning(this, "下载更新失败", errorMessage);
    } else {
        statusBar()->showMessage(QString("后台下载更新失败，将在下次检查时重试：%1").arg(errorMessage.section('\n', 0, 0)), 8000);
    }
}

void MainWindow::finishInstallerDownload()
{
    const bool installNow = m_installAfterDownload;
    m_installerDownloadVersion.clear();
    m_installerDownloadUrl = QUrl();
    m_installerDownloadInteractive = false;
    m_installAfterDownload = false;
    if (m_isClosing) {
        return;
    }
    if (installNow && m_updateDialog != nullptr) {
        setUpdateDialogStatus("安装包已下载并通过校验，正在准备安装...", 100);
        m_updateDialog->done(kInstallDialogResult);
        return;
    }
    if (m_updateDialog != nullptr) {
        setUpdateDialogStatus("安装包已下载并通过校验，点击“更新”立即安装，程序会自动重启。", 100);
        if (m_updateDialogInstallButton != nullptr) {
            m_updateDialogInstallButton->setEnabled(true);
            m_updateDialogInstallButton->setText("更新");
        }
    }
    if (m_silentUpdateCheck->isChecked()) {
        statusBar()->showMessage(QString("新版本 %1 已下载，将在没有同步任务时自动安装。").arg(m_readyInstallerVersion), 8000);
        QTimer::singleShot(0, this, [this]() {
            installSilentUpdateIfIdle();
        });
    }
}

bool MainWindow::launchInstaller(bool silent, QString *errorMessage)
{
#ifdef Q_OS_WIN
    const QString appDir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const bool writable = isDirectoryWritable(appDir);
    if (silent && !writable) {
        *errorMessage = QString("程序目录没有写入权限：%1").arg(appDir);
        return false;
    }
    // 安装到当前程序所在目录；安装程序等待本程序退出后替换文件，并按当前窗口状态重启程序。
    QStringList arguments;
    arguments << (silent ? "/VERYSILENT" : "/SILENT") << "/SUPPRESSMSGBOXES" << "/NORESTART" << "/SP-"
              << QString("/DIR=\"%1\"").arg(appDir)
              << QString("/RELAUNCH=%1").arg(isVisible() ? "show" : "hidden")
              << QString("/LOG=\"%1\"").arg(QDir::toNativeSeparators(QDir(updateDownloadDir()).filePath("install.log")));
    if (!writable) {
        // 目录需要管理员权限时让安装程序请求提升权限。
        arguments << "/ALLUSERS";
    }
    const QString installerPath = QDir::toNativeSeparators(m_readyInstallerPath);
    const QString parameters = arguments.join(' ');
    const HINSTANCE result = ShellExecuteW(nullptr,
                                           L"open",
                                           reinterpret_cast<LPCWSTR>(installerPath.utf16()),
                                           reinterpret_cast<LPCWSTR>(parameters.utf16()),
                                           nullptr,
                                           SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        *errorMessage = QString("无法启动安装程序（错误码 %1）。").arg(static_cast<qlonglong>(reinterpret_cast<INT_PTR>(result)));
        return false;
    }
    return true;
#else
    Q_UNUSED(silent);
    *errorMessage = QString("当前系统不支持自动安装更新。");
    return false;
#endif
}

void MainWindow::installUpdateNow()
{
    if (m_isClosing || m_isInstallingUpdate || !isReadyInstallerCurrent()) {
        return;
    }
    if (hasRunningWorkers()
        && QMessageBox::question(this, "安装更新", "当前有同步任务正在运行，安装更新会停止这些任务。是否继续？")
               != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!launchInstaller(false, &error)) {
        QMessageBox::warning(this, "安装更新失败", QString("%1\n可前往发布页手动下载安装包。").arg(error));
        return;
    }
    m_isInstallingUpdate = true;
    realExit();
}

bool MainWindow::installSilentUpdateIfIdle()
{
    if (m_isClosing || m_isInstallingUpdate || !m_silentUpdateCheck->isChecked() || !isReadyInstallerCurrent()
        || m_silentInstallFailedVersion == m_readyInstallerVersion || m_updateDialogOpen
        || hasRunningWorkers() || m_pendingSyncAfterCurrentRun || (isVisible() && isActiveWindow())
        || QApplication::activeModalWidget() != nullptr || QApplication::activePopupWidget() != nullptr) {
        return false;
    }
    QString error;
    if (!launchInstaller(true, &error)) {
        // 同一版本不再反复尝试，用户仍可通过“检查更新”手动安装。
        m_silentInstallFailedVersion = m_readyInstallerVersion;
        statusBar()->showMessage(QString("无感更新未执行：%1").arg(error), 10000);
        return false;
    }
    m_isInstallingUpdate = true;
    realExit();
    return true;
}

void MainWindow::updateCheckButton()
{
    if (m_checkUpdatesButton == nullptr || m_updatePulse == nullptr) {
        return;
    }
    const bool hasUpdate = !m_availableUpdateVersion.isEmpty();
    m_checkUpdatesButton->setEnabled(!m_isClosing && (!m_updateCheckInProgress || hasUpdate));
    m_checkUpdatesButton->setText(hasUpdate ? "发现新版本" : (m_updateCheckInProgress ? "正在检查..." : "检查更新"));
    m_checkUpdatesButton->setToolTip(hasUpdate
        ? QString("发现版本 %1，点击查看更新说明并选择是否更新。").arg(m_availableUpdateVersion)
        : QString("手动检查软件更新；程序每 2 小时自动检查一次。"));

    const bool dismissed = !m_dismissedUpdateVersion.isEmpty()
        && QVersionNumber::compare(QVersionNumber::fromString(m_availableUpdateVersion),
                                   QVersionNumber::fromString(m_dismissedUpdateVersion)) <= 0;
    if (!hasUpdate || dismissed || m_isClosing || m_updateDialogOpen) {
        m_updatePulse->stop();
        m_checkUpdatesButton->setStyleSheet(hasUpdate
            ? QString::fromLatin1(kUpdateButtonStyle).arg("#e8f3ff") : QString());
        return;
    }
    if (isVisible() && !isMinimized()) {
        if (m_updatePulse->state() == QAbstractAnimation::Paused) {
            m_updatePulse->resume();
        } else if (m_updatePulse->state() == QAbstractAnimation::Stopped) {
            m_updatePulse->start();
        }
    } else if (m_updatePulse->state() == QAbstractAnimation::Running) {
        m_updatePulse->pause();
    }
}

void MainWindow::stopUpdateChecks()
{
    if (m_updateCheckTimer != nullptr) {
        m_updateCheckTimer->stop();
    }
    if (m_updatePulse != nullptr) {
        m_updatePulse->stop();
    }
    m_updateCheckInProgress = false;
    m_updateCheckInteractive = false;
    if (m_updateReply != nullptr) {
        m_updateReply->abort();
        m_updateReply.clear();
    }
    abortInstallerDownload();
}

void MainWindow::onReleaseDirEditFinished()
{
    if (m_isClosing) return;
    const QString path = m_releaseDirEdit->currentText().trimmed();
    if (!path.isEmpty()) {
        addToReleaseDirHistory(path);
    }
    m_lastFingerprint.clear();
    saveConfiguration();
}

void MainWindow::addToReleaseDirHistory(const QString &path)
{
    const QString normalized = normalizePathForUi(path);
    if (normalized.isEmpty()) {
        return;
    }

    // 移除已存在的相同路径（稍后会添加到最前面）
    m_config.releaseDirHistory.removeAll(normalized);

    // 插入到最前面
    m_config.releaseDirHistory.prepend(normalized);

    // 限制历史记录数量
    while (m_config.releaseDirHistory.size() > kMaxReleaseDirHistory) {
        m_config.releaseDirHistory.removeLast();
    }

    // 更新下拉框
    QSignalBlocker blocker(m_releaseDirEdit);
    m_releaseDirEdit->clear();
    for (const QString &item : m_config.releaseDirHistory) {
        m_releaseDirEdit->addItem(item);
    }
    m_releaseDirEdit->setCurrentText(normalized);
}

void MainWindow::loadConfiguration()
{
    m_config = ConfigManager::load(m_configPath);
    const bool systemAutoStartEnabled = m_manageSystemStartup && isSystemAutoStartEnabled();
    if (m_manageSystemStartup && m_config.startWithSystem && !systemAutoStartEnabled) {
        QString error;
        if (!setSystemAutoStartEnabled(true, &error)) {
            m_config.startWithSystem = false;
        }
    } else if (m_manageSystemStartup && !m_config.startWithSystem && systemAutoStartEnabled) {
        m_config.startWithSystem = true;
    }
    applyConfigToUi();
}

void MainWindow::applyConfigToUi()
{
    // 先填充历史记录到下拉框
    {
        QSignalBlocker blocker(m_releaseDirEdit);
        m_releaseDirEdit->clear();
        for (const QString &item : m_config.releaseDirHistory) {
            if (!item.trimmed().isEmpty()) {
                m_releaseDirEdit->addItem(item);
            }
        }
        m_releaseDirEdit->setCurrentText(normalizePathForUi(m_config.releaseDir));
    }

    {
        QSignalBlocker blocker(m_autoSyncCheck);
        m_autoSyncCheck->setChecked(m_config.autoSync);
    }
    {
        QSignalBlocker blocker(m_startWithSystemCheck);
        m_startWithSystemCheck->setChecked(m_config.startWithSystem);
    }
    {
        QSignalBlocker blocker(m_checkUpdatesOnStartupCheck);
        m_checkUpdatesOnStartupCheck->setChecked(m_config.checkUpdatesOnStartup);
    }
    {
        QSignalBlocker blocker(m_silentUpdateCheck);
        m_silentUpdateCheck->setChecked(m_config.silentUpdate);
    }
    {
        QSignalBlocker blocker(m_intervalSpin);
        m_intervalSpin->setValue(qMax(1, m_config.scanIntervalSeconds));
    }

    if (m_config.fileTypes.isEmpty()) {
        m_config.fileTypes = QStringList() << "dll" << "exe" << "pdb";
    }

    refreshTypeList();
    refreshRuleTable();
    refreshHostTable();
    clearHostEditor();
    m_lastFingerprint = currentLocalFingerprint();
}

void MainWindow::collectUiToConfig()
{
    const QString previousReleaseDir = normalizePathForUi(m_config.releaseDir);
    QStringList previousTypes = m_config.selectedFileTypes;
    previousTypes.removeDuplicates();
    previousTypes.sort();

    m_config.releaseDir = normalizePathForUi(m_releaseDirEdit->currentText());
    m_config.autoSync = m_autoSyncCheck->isChecked();
    m_config.startWithSystem = m_startWithSystemCheck->isChecked();
    m_config.checkUpdatesOnStartup = m_checkUpdatesOnStartupCheck->isChecked();
    m_config.silentUpdate = m_silentUpdateCheck->isChecked();
    m_config.scanIntervalSeconds = m_intervalSpin->value();

    QStringList types;
    QStringList allTypes;
    for (int index = 0; index < m_typeList->count(); ++index) {
        QListWidgetItem *item = m_typeList->item(index);
        allTypes.append(normalizedExtension(item->text()));
        if (item->checkState() == Qt::Checked) {
            types.append(normalizedExtension(item->text()));
        }
    }

    allTypes.removeDuplicates();
    types.removeDuplicates();
    m_config.fileTypes = allTypes;
    m_config.selectedFileTypes = types;

    types.sort();
    if (previousReleaseDir.compare(m_config.releaseDir, Qt::CaseInsensitive) != 0 || previousTypes != types) {
        // 配置变化后，旧任务的成功回调不能恢复已清除的同步记录。
        for (HostEntry &host : m_config.hosts) {
            host.lastSyncUtc = QDateTime();
            ++m_syncContextRevisions[host.id];
        }
        refreshLastSyncTimes();
    }
}

void MainWindow::refreshTypeList()
{
    QSignalBlocker blocker(m_typeList);
    m_typeList->clear();

    QStringList allTypes = m_config.fileTypes;
    QStringList selectedTypes = m_config.selectedFileTypes;
    allTypes.removeDuplicates();
    selectedTypes.removeDuplicates();
    std::sort(allTypes.begin(), allTypes.end());

    for (const QString &type : allTypes) {
        auto *item = new QListWidgetItem(type, m_typeList);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        item->setCheckState(selectedTypes.contains(type) ? Qt::Checked : Qt::Unchecked);
    }
}

void MainWindow::refreshHostTable()
{
    m_copyPreview->dismiss();
    m_isRefreshingTable = true;
    m_rowWidgets.clear();
    m_hostTable->setRowCount(m_config.hosts.size());

    for (int row = 0; row < m_config.hosts.size(); ++row) {
        const HostEntry &host = m_config.hosts.at(row);

        auto *enabledItem = new QTableWidgetItem();
        enabledItem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable);
        enabledItem->setCheckState(host.enabled ? Qt::Checked : Qt::Unchecked);
        enabledItem->setData(Qt::UserRole, host.id);
        m_hostTable->setItem(row, 0, enabledItem);

        auto *nameItem = new QTableWidgetItem(host.name.trimmed().isEmpty() ? defaultHostNameFromPath(host.sharePath) : host.name.trimmed());
        auto *pathItem = new QTableWidgetItem(normalizePathForUi(host.sharePath));
        auto *userItem = new QTableWidgetItem(host.username);
        auto *statusItem = new QTableWidgetItem("未检测");
        auto *detailItem = new QTableWidgetItem("等待状态刷新");
        pathItem->setToolTip(sharePathTooltip(pathItem->text()));
        m_hostTable->setItem(row, 1, nameItem);
        m_hostTable->setItem(row, 2, pathItem);
        m_hostTable->setItem(row, 3, userItem);
        m_hostTable->setItem(row, 5, statusItem);
        m_hostTable->setItem(row, 6, detailItem);
        m_hostTable->setItem(row, 7, new QTableWidgetItem());

        auto *progressBar = new QProgressBar(m_hostTable);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setFormat("%p%");
        m_copyPreview->watch(progressBar, host.id);
        m_hostTable->setCellWidget(row, 4, progressBar);

        RowWidgets widgets;
        widgets.progressBar = progressBar;
        m_rowWidgets.insert(host.id, widgets);

        if (m_stoppingHostIds.contains(host.id)) {
            updateHostRow(row, SyncWorker::Stopping, 0, "正在停止", "正在等待后台任务停止...", QString(), 0);
        } else {
            updateHostRow(row, SyncWorker::Checking, 0, "未检测", "等待状态刷新", "等待状态刷新", 0);
        }
    }

    m_isRefreshingTable = false;
    refreshLastSyncTimes();
}

void MainWindow::refreshLastSyncTimes()
{
    if (m_isClosing) {
        return;
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (int row = 0; row < m_config.hosts.size(); ++row) {
        QTableWidgetItem *item = m_hostTable->item(row, 7);
        if (item == nullptr) {
            continue;
        }

        const HostEntry &host = m_config.hosts.at(row);
        QString text = "暂无同步记录";
        QString tooltip = "暂无实际复制成功的记录";
        if (host.lastSyncUtc.isValid()) {
            tooltip = QString("上次实际复制成功：%1").arg(host.lastSyncUtc.toLocalTime().toString("yyyy-MM-dd HH:mm:ss t"));
            const qint64 seconds = host.lastSyncUtc.secsTo(now);
            if (host.lastSyncUtc > now) {
                text = "时间异常";
                tooltip += "\n记录时间晚于当前时间，请检查系统时间。";
            } else if (seconds < 5) {
                text = "刚刚";
            } else if (seconds < 60) {
                text = QString("%1 秒前").arg(seconds);
            } else if (seconds < 3600) {
                text = QString("%1 分钟前").arg(seconds / 60);
            } else if (seconds < 86400) {
                text = QString("%1 小时前").arg(seconds / 3600);
            } else {
                text = QString("%1 天前").arg(seconds / 86400);
            }
        }

        if (item->text() != text) {
            item->setText(text);
        }
        if (item->toolTip() != tooltip) {
            item->setToolTip(tooltip);
        }
    }
}

void MainWindow::browseReleaseDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, "选择本地 Release 目录", m_releaseDirEdit->currentText());
    if (dir.isEmpty()) {
        return;
    }

    addToReleaseDirHistory(dir);
    m_lastFingerprint.clear();
    saveConfiguration();
    requestAutoSync("本地目录已更新，开始自动同步");
}

void MainWindow::addOrUpdateHost(bool updateExisting)
{
    const QString sharePath = normalizePathForUi(m_hostPathEdit->text());
    if (sharePath.isEmpty()) {
        QMessageBox::warning(this, "提示", "请填写共享目录路径。");
        return;
    }

    HostEntry host;
    host.name = m_hostNameEdit->text().trimmed();
    host.sharePath = sharePath;
    host.username = m_hostUserEdit->text().trimmed();
    host.password = m_hostPasswordEdit->text();
    host.enabled = m_hostEnabledCheck->isChecked();

    const int row = m_hostTable->currentRow();
    if (updateExisting) {
        if (row < 0 || row >= m_config.hosts.size()) {
            QMessageBox::warning(this, "提示", "请在表格中选择一台主机后再更新。");
            return;
        }
        const HostEntry &previousHost = m_config.hosts.at(row);
        host.id = previousHost.id;
        if (normalizePathForUi(previousHost.sharePath).compare(host.sharePath, Qt::CaseInsensitive) == 0) {
            host.lastSyncUtc = previousHost.lastSyncUtc;
        } else {
            ++m_syncContextRevisions[host.id];
        }
        m_config.hosts[row] = host;
    } else {
        host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (host.name.isEmpty()) {
            host.name = defaultHostNameFromPath(host.sharePath);
        }
        m_config.hosts.push_back(host);
    }

    refreshHostTable();
    if (!host.enabled) {
        requestWorkerToStop(host.id);
    }
    saveConfiguration();
    if (host.enabled) {
        requestAutoSync(QString("主机 [%1] 配置已更新，开始自动同步").arg(hostDisplayText(host)));
    }
}

void MainWindow::removeSelectedHost()
{
    const int row = m_hostTable->currentRow();
    if (row < 0 || row >= m_config.hosts.size()) {
        QMessageBox::warning(this, "提示", "请先选择需要删除的主机。");
        return;
    }

    if (m_runningThreads.contains(m_config.hosts.at(row).id)) {
        QMessageBox::warning(this, "提示", "该主机正在执行任务，请等待完成后再删除。");
        return;
    }

    m_syncContextRevisions.remove(m_config.hosts.at(row).id);
    m_copyPreview->removeHost(m_config.hosts.at(row).id);
    m_config.hosts.removeAt(row);
    refreshHostTable();
    clearHostEditor();
    saveConfiguration();
}

void MainWindow::clearHostEditor()
{
    m_hostEnabledCheck->setChecked(true);
    m_hostNameEdit->clear();
    m_hostPathEdit->clear();
    m_hostUserEdit->clear();
    m_hostPasswordEdit->clear();
    m_hostTable->clearSelection();
}

void MainWindow::loadSelectedHostToEditor()
{
    if (m_isClosing) return;
    const int row = m_hostTable->currentRow();
    if (row < 0 || row >= m_config.hosts.size()) {
        return;
    }

    const HostEntry &host = m_config.hosts.at(row);
    m_hostEnabledCheck->setChecked(host.enabled);
    m_hostNameEdit->setText(host.name);
    m_hostPathEdit->setText(normalizePathForUi(host.sharePath));
    m_hostUserEdit->setText(host.username);
    m_hostPasswordEdit->setText(host.password);
}

void MainWindow::saveConfiguration()
{
    collectUiToConfig();

    QString error;
    if (!ConfigManager::save(m_configPath, m_config, &error)) {
        QMessageBox::warning(this, "保存失败", QString("配置文件写入失败：%1").arg(error));
        return;
    }
    if (m_manageSystemStartup && !setSystemAutoStartEnabled(m_config.startWithSystem, &error)) {
        QMessageBox::warning(this, "自启设置失败", QString("开机自启设置写入失败：%1").arg(error));
        return;
    }

    statusBar()->showMessage("配置已保存", 3000);
}

void MainWindow::syncNow()
{
    if (m_isClosing) {
        return;
    }

    m_backgroundTasksPaused = false;
    m_lastFingerprint = currentLocalFingerprint();
    if (hasRunningWorkers()) {
        m_pendingSyncAfterCurrentRun = true;
        statusBar()->showMessage("当前正在检查/同步，完成后会自动执行一次同步", 3000);
        return;
    }

    startOperations(true);
}

void MainWindow::stopAllOperations()
{
    if (m_isClosing) {
        return;
    }

    m_backgroundTasksPaused = true;
    m_pendingSyncAfterCurrentRun = false;
    m_pendingSyncStatusMessage.clear();
    {
        QSignalBlocker blocker(m_autoSyncCheck);
        m_autoSyncCheck->setChecked(false);
    }
    requestWorkersToStop();
    saveConfiguration();
    statusBar()->showMessage(hasRunningWorkers() ? "正在停止所有后台任务..." : "已停止，自动同步已关闭");
}

void MainWindow::addFileType()
{
    const QString type = normalizedExtension(m_newTypeEdit->text());
    if (type.isEmpty()) {
        QMessageBox::warning(this, "提示", "请输入有效的扩展名，如 dll。");
        return;
    }

    for (int index = 0; index < m_typeList->count(); ++index) {
        if (normalizedExtension(m_typeList->item(index)->text()) == type) {
            m_typeList->item(index)->setCheckState(Qt::Checked);
            m_newTypeEdit->clear();
            saveConfiguration();
            return;
        }
    }

    auto *item = new QListWidgetItem(type, m_typeList);
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable | Qt::ItemIsEnabled);
    item->setCheckState(Qt::Checked);
    m_newTypeEdit->clear();
    saveConfiguration();
}

void MainWindow::removeSelectedFileType()
{
    QListWidgetItem *item = m_typeList->currentItem();
    if (item == nullptr) {
        QMessageBox::warning(this, "提示", "请先选择一个扩展名。");
        return;
    }

    delete m_typeList->takeItem(m_typeList->row(item));
    saveConfiguration();
    m_lastFingerprint.clear();
}

void MainWindow::handleTypeItemChanged(QListWidgetItem *item)
{
    Q_UNUSED(item);
    m_lastFingerprint.clear();
    saveConfiguration();
    requestAutoSync("同步类型已更新，开始自动同步");
}

void MainWindow::handleHostTableItemChanged(QTableWidgetItem *item)
{
    if (m_isClosing || m_isRefreshingTable || item == nullptr || item->column() != 0) {
        return;
    }

    const int row = item->row();
    if (row < 0 || row >= m_config.hosts.size()) {
        return;
    }

    m_config.hosts[row].enabled = item->checkState() == Qt::Checked;
    if (!m_config.hosts[row].enabled) {
        requestWorkerToStop(m_config.hosts.at(row).id);
    }
    saveConfiguration();
    if (m_config.hosts[row].enabled) {
        requestAutoSync(QString("已勾选主机 [%1]，开始自动同步").arg(hostDisplayText(m_config.hosts[row])));
    }
}

void MainWindow::handleHostTableCellClicked(int row, int column)
{
    if (m_isClosing || column != 2 || row < 0 || row >= m_config.hosts.size()) {
        return;
    }
    if ((QApplication::keyboardModifiers() & Qt::ControlModifier) == 0) {
        return;
    }

    const QString sharePath = normalizePathForUi(m_config.hosts.at(row).sharePath);
    if (sharePath.isEmpty()) {
        return;
    }

    if (m_isOpeningShare
        || (m_lastShareOpenPath.compare(sharePath, Qt::CaseInsensitive) == 0
            && m_shareOpenClickTimer.isValid()
            && m_shareOpenClickTimer.elapsed() < QApplication::doubleClickInterval())) {
        return;
    }

    m_isOpeningShare = true;
    m_lastShareOpenPath = sharePath;
    m_shareOpenClickTimer.start();

    const auto started = std::make_shared<bool>(false);
    // 启动调用可能阻塞；线程不以窗口为父对象，避免退出时销毁仍在运行的线程。
    auto *thread = QThread::create([sharePath, started]() {
        *started = QProcess::startDetached("explorer.exe", QStringList() << sharePath);
    });
    connect(thread, &QThread::finished, this, [this, sharePath, started]() {
        m_isOpeningShare = false;
        if (m_isClosing) {
            return;
        }

        statusBar()->showMessage(QString(*started ? "已请求打开共享目录：%1" : "启动资源管理器失败：%1")
                                     .arg(sharePath), 3000);
    }, Qt::QueuedConnection);
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);

    statusBar()->showMessage(QString("正在请求打开共享目录：%1").arg(sharePath));
    thread->start();
}

void MainWindow::handleHostTableCellDoubleClicked(int row, int column)
{
    if (m_isClosing || (column != 1 && column != 2)
        || (QApplication::keyboardModifiers() & Qt::ControlModifier) != 0) {
        return;
    }

    copyHostNameFromRow(row);
}

void MainWindow::showHostTableContextMenu(const QPoint &position)
{
    const QModelIndex index = m_hostTable->indexAt(position);
    if (!index.isValid()) {
        return;
    }

    const int row = index.row();
    if (row < 0 || row >= m_config.hosts.size()) {
        return;
    }

    QMenu menu(this);
    QAction *copyHostNameAction = menu.addAction("复制主机名");
    QAction *selectedAction = menu.exec(m_hostTable->viewport()->mapToGlobal(position));
    if (selectedAction == copyHostNameAction) {
        copyHostNameFromRow(row);
    }
}

void MainWindow::copyHostNameFromRow(int row)
{
    if (row < 0 || row >= m_config.hosts.size()) {
        return;
    }

    const QString hostName = hostNameFromSharePath(m_config.hosts.at(row).sharePath);
    if (hostName.isEmpty()) {
        statusBar()->showMessage("复制主机名失败：共享目录为空", 3000);
        return;
    }

    QApplication::clipboard()->setText(hostName);
    statusBar()->showMessage(QString("已复制主机名：%1").arg(hostName), 3000);
}

void MainWindow::handleWorkerProgress(const QString &hostId, int percent, const QString &detail)
{
    if (m_stoppingHostIds.contains(hostId)) {
        return;
    }

    RowWidgets widgets = m_rowWidgets.value(hostId);
    if (widgets.progressBar != nullptr) {
        widgets.progressBar->setValue(percent);
    }

    for (int row = 0; row < m_config.hosts.size(); ++row) {
        if (m_config.hosts.at(row).id == hostId) {
            if (m_hostTable->item(row, 6) != nullptr) {
                m_hostTable->item(row, 6)->setText(detail);
                m_hostTable->item(row, 6)->setToolTip(detail);
                m_copyPreview->setDetails(hostId, detail, false);
            }
            break;
        }
    }
}

void MainWindow::handleWorkerState(const QString &hostId,
                                   int state,
                                   int mismatchPercent,
                                   const QString &statusText,
                                   const QString &detail,
                                   const QString &detailTooltip)
{
    // 停止请求之后到达的旧信号不得覆盖停止状态；等线程退出后再显示已停止。
    if (m_stoppingHostIds.contains(hostId)) {
        return;
    }

    for (int row = 0; row < m_config.hosts.size(); ++row) {
        if (m_config.hosts.at(row).id == hostId) {
            RowWidgets widgets = m_rowWidgets.value(hostId);
            const int currentProgress = widgets.progressBar != nullptr ? widgets.progressBar->value() : 0;
            updateHostRow(row, state, mismatchPercent, statusText, detail, detailTooltip, currentProgress);
            break;
        }
    }
}

void MainWindow::handleWorkerCopySucceeded(const QString &hostId, const QDateTime &completedUtc, quint64 revision)
{
    if (m_isClosing || !completedUtc.isValid() || m_stoppingHostIds.contains(hostId)) {
        return;
    }

    collectUiToConfig();
    if (m_syncContextRevisions.value(hostId) != revision) {
        return;
    }

    for (HostEntry &host : m_config.hosts) {
        if (host.id == hostId) {
            host.lastSyncUtc = completedUtc.toUTC();
            refreshLastSyncTimes();
            QString error;
            if (!ConfigManager::save(m_configPath, m_config, &error)) {
                QMessageBox::warning(this, "同步时间保存失败", QString("文件同步已完成，但同步时间无法写入配置：%1").arg(error));
            }
            return;
        }
    }
}

void MainWindow::handleWorkerFinished(const QString &hostId)
{
    QThread *thread = m_runningThreads.value(hostId, nullptr);
    if (thread != nullptr) {
        thread->quit();
    }
}

void MainWindow::pollForChanges()
{
    if (m_isClosing || hasRunningWorkers()) {
        return;
    }
    if (m_backgroundTasksPaused) {
        installSilentUpdateIfIdle();
        return;
    }

    const QString fingerprint = currentLocalFingerprint();
    if (m_lastFingerprint.isEmpty()) {
        m_lastFingerprint = fingerprint;
        startOperations(m_autoSyncCheck->isChecked());
        return;
    }

    const bool changed = fingerprint != m_lastFingerprint;
    // 本地无新变化且没有任务运行时视为空闲，此时执行已下载好的无感更新。
    if (!changed && installSilentUpdateIfIdle()) {
        return;
    }
    if (changed) {
        m_lastFingerprint = fingerprint;
        if (m_autoSyncCheck->isChecked()) {
            statusBar()->showMessage("检测到本地文件变化，开始自动同步", 3000);
            startOperations(true);
            return;
        }
    }

    startOperations(m_autoSyncCheck->isChecked());
}

void MainWindow::updateHostRow(int row,
                               int state,
                               int mismatchPercent,
                               const QString &statusText,
                               const QString &detail,
                               const QString &detailTooltip,
                               int progressPercent)
{
    if (row < 0 || row >= m_hostTable->rowCount()) {
        return;
    }

    const QColor color = colorForState(state, mismatchPercent);
    for (int column = 1; column < m_hostTable->columnCount(); ++column) {
        QTableWidgetItem *tableItem = m_hostTable->item(row, column);
        if (tableItem != nullptr) {
            tableItem->setBackground(color);
        }
    }

    if (m_hostTable->item(row, 5) != nullptr) {
        m_hostTable->item(row, 5)->setText(statusText);
        m_hostTable->item(row, 5)->setToolTip(statusText);
    }
    const QString hostId = m_config.hosts.value(row).id;
    if (m_hostTable->item(row, 6) != nullptr) {
        m_hostTable->item(row, 6)->setText(detail);
        const QString tooltip = detailTooltip.isEmpty() ? detail : detailTooltip;
        m_hostTable->item(row, 6)->setToolTip(tooltip);
        // 扫描和复制中的短提示不覆盖最近一次完整检查结果。
        const bool remember = state != SyncWorker::Checking && state != SyncWorker::Syncing
            && state != SyncWorker::Stopping;
        m_copyPreview->setDetails(hostId, tooltip, remember);
    }

    RowWidgets widgets = m_rowWidgets.value(hostId);
    // 检查阶段保留上次同步进度，避免周期检查造成进度条闪动。
    if (widgets.progressBar != nullptr && state != SyncWorker::Checking) {
        if (widgets.progressBar->value() != progressPercent) {
            widgets.progressBar->setValue(progressPercent);
        }
        const QString progressStyle = QString("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: %1; }")
                                          .arg(color.darker(110).name());
        if (widgets.progressBar->styleSheet() != progressStyle) {
            widgets.progressBar->setStyleSheet(progressStyle);
        }
    }
}

QColor MainWindow::colorForState(int state, int mismatchPercent) const
{
    switch (state) {
    case SyncWorker::InSync:
        return QColor(198, 239, 206);
    case SyncWorker::Mismatch: {
        const int intensity = qBound(0, mismatchPercent, 100);
        const int green = qMax(40, 220 - intensity);
        const int blue = qMax(40, 220 - intensity * 2 / 3);
        return QColor(255, green, blue);
    }
    case SyncWorker::Disconnected:
    case SyncWorker::Stopped:
        return QColor(180, 180, 180);
    case SyncWorker::Error:
        return QColor(255, 199, 140);
    case SyncWorker::Syncing:
        return QColor(189, 215, 238);
    case SyncWorker::Checking:
    case SyncWorker::Stopping:
    default:
        return QColor(221, 235, 247);
    }
}

QString MainWindow::currentLocalFingerprint() const
{
    QStringList selectedTypes;
    for (int index = 0; index < m_typeList->count(); ++index) {
        const QListWidgetItem *item = m_typeList->item(index);
        if (item->checkState() == Qt::Checked) selectedTypes.append(normalizedExtension(item->text()));
    }
    return SyncPlan::fingerprint(m_releaseDirEdit->currentText(), selectedTypes, m_config.syncRules);
}
QString MainWindow::normalizedExtension(const QString &value) const
{
    QString type = value.trimmed().toLower();
    while (type.startsWith('.')) {
        type.remove(0, 1);
    }
    return type;
}

void MainWindow::requestAutoSync(const QString &statusMessage)
{
    if (m_isClosing || !m_autoSyncCheck->isChecked()) {
        return;
    }

    m_lastFingerprint = currentLocalFingerprint();
    if (hasRunningWorkers()) {
        m_pendingSyncAfterCurrentRun = true;
        m_pendingSyncStatusMessage = statusMessage;
        statusBar()->showMessage(QString("%1，当前任务完成后立即执行").arg(statusMessage), 3000);
        return;
    }

    m_pendingSyncStatusMessage.clear();
    statusBar()->showMessage(statusMessage, 3000);
    startOperations(true);
}

QVector<int> MainWindow::targetRows() const
{
    QVector<int> rows;
    for (int row = 0; row < m_config.hosts.size(); ++row) {
        if (m_config.hosts.at(row).enabled) {
            rows.push_back(row);
        }
    }

    return rows;
}

void MainWindow::startOperations(bool performCopy)
{
    if (m_isClosing || m_backgroundTasksPaused) {
        return;
    }

    collectUiToConfig();
    const QVector<int> rows = targetRows();
    if (rows.isEmpty()) {
        if (performCopy) {
            statusBar()->showMessage("没有可同步的主机被勾选。", 3000);
        }
        return;
    }

    QStringList selectedTypes = m_config.selectedFileTypes;
    selectedTypes.removeDuplicates();
    if (selectedTypes.isEmpty() && std::none_of(m_config.syncRules.cbegin(), m_config.syncRules.cend(), [](const SyncRule &rule) { return rule.enabled; })) {
        if (performCopy) {
            statusBar()->showMessage("请勾选基本同步类型，或启用至少一条附加同步规则。", 3000);
        }
        return;
    }

    for (int row : rows) {
        const HostEntry host = m_config.hosts.at(row);
        if (m_runningThreads.contains(host.id)) {
            continue;
        }

        auto *thread = new QThread(this);
        auto *worker = new SyncWorker(host, m_config.releaseDir, selectedTypes, performCopy, m_config.syncRules);
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &SyncWorker::process);
        connect(worker, &SyncWorker::progressChanged, this, &MainWindow::handleWorkerProgress);
        connect(worker, &SyncWorker::recentFilesChanged, this, [this](const QString &hostId, const QStringList &files, const QString &title) {
            if (!m_isClosing) {
                m_copyPreview->setRecentFiles(hostId, files, title);
            }
        });
        connect(worker, &SyncWorker::stateChanged, this, &MainWindow::handleWorkerState);
        const quint64 revision = m_syncContextRevisions.value(host.id);
        connect(worker, &SyncWorker::copySucceeded, this, [this, revision](const QString &hostId, const QDateTime &completedUtc) {
            handleWorkerCopySucceeded(hostId, completedUtc, revision);
        });
        connect(worker, &SyncWorker::finished, this, &MainWindow::handleWorkerFinished);
        connect(worker, &SyncWorker::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        connect(thread, &QThread::finished, this, [this, host]() {
            m_runningThreads.remove(host.id);
            m_cancellationFlags.remove(host.id);
            if (m_stoppingHostIds.remove(host.id)) {
                for (int row = 0; row < m_config.hosts.size(); ++row) {
                    if (m_config.hosts.at(row).id == host.id) {
                        const RowWidgets widgets = m_rowWidgets.value(host.id);
                        const int progress = widgets.progressBar != nullptr ? widgets.progressBar->value() : 0;
                        updateHostRow(row, SyncWorker::Stopped, 0, "已停止", "操作已被用户停止", QString(), progress);
                        break;
                    }
                }
            }
            if (!hasRunningWorkers()) {
                if (m_isClosing) {
                    finishExit();
                    return;
                }

                if (m_pendingSyncAfterCurrentRun) {
                    m_pendingSyncAfterCurrentRun = false;
                    const QString pendingMessage = m_pendingSyncStatusMessage.isEmpty()
                        ? QString("开始执行队列中的同步任务")
                        : m_pendingSyncStatusMessage;
                    m_pendingSyncStatusMessage.clear();
                    statusBar()->showMessage(pendingMessage, 3000);
                    startOperations(true);
                    return;
                }

                statusBar()->showMessage(m_backgroundTasksPaused ? "已停止，自动同步已关闭" : "所有后台任务已结束", 3000);
            }
        });

        m_runningThreads.insert(host.id, thread);
        m_cancellationFlags.insert(host.id, worker->cancellationFlag());
        updateHostRow(row, SyncWorker::Checking, 0,
                      "检测中",
                      "准备检测...",
                      "准备检测...",
                      0);
        thread->start();
    }
}

bool MainWindow::hasRunningWorkers() const
{
    return !m_runningThreads.isEmpty();
}

void MainWindow::updateTimerInterval()
{
    if (m_pollTimer != nullptr) {
        m_pollTimer->setInterval(qMax(1, m_intervalSpin->value()) * 1000);
    }
}

bool MainWindow::isSystemAutoStartEnabled() const
{
#ifdef Q_OS_WIN
    QSettings settings(kAutoStartRunKey, QSettings::NativeFormat);
    const QString value = settings.value(kAutoStartValueName).toString().trimmed();
    return value.compare(startupCommand(), Qt::CaseInsensitive) == 0;
#else
    return false;
#endif
}

bool MainWindow::setSystemAutoStartEnabled(bool enabled, QString *errorMessage) const
{
#ifndef Q_OS_WIN
    if (enabled && errorMessage != nullptr) {
        *errorMessage = "当前平台不支持开机自启。";
    }
    return !enabled;
#else
    QSettings settings(kAutoStartRunKey, QSettings::NativeFormat);
    if (enabled) {
        settings.setValue(kAutoStartValueName, startupCommand());
    } else {
        settings.remove(kAutoStartValueName);
    }
    settings.sync();

    if (settings.status() != QSettings::NoError) {
        if (errorMessage != nullptr) {
            *errorMessage = "无法写入当前用户的 Run 注册表项。";
        }
        return false;
    }

    return true;
#endif
}
