#pragma execution_character_set("utf-8")
#include "mainwindow.h"

#include "configmanager.h"
#include "diagnostics.h"
#include "downloadprogress.h"
#include "remotedebugger.h"
#include "shortcutmanager.h"
#include "networkhostdialog.h"
#include "syncworker.h"
#include "synccompletiontoast.h"
#include "syncplan.h"
#include "ui_mainwindow.h"
#include "ui_preferencesdialog.h"
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
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPainterPath>
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
#include <QSplitter>
#include <QSslSocket>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabBar>
#include <QTableWidgetItem>
#include <QThread>
#include <QTextBrowser>
#include <QTextOption>
#include <QTimer>
#include <QToolButton>
#include <QToolButton>
#include <QToolTip>
#include <QUrl>
#include <QVBoxLayout>
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
class SyncTabCountBadge final : public QLabel
{
public:
    SyncTabCountBadge(const QString &text, QTabBar *bar, int tab)
        : QLabel(text, bar), m_tab(tab)
    {
    }

protected:
    bool event(QEvent *event) override
    {
        const bool handled = QLabel::event(event);
        const auto type = event->type();
        if (!m_repositioning && (type == QEvent::Move || type == QEvent::Resize || type == QEvent::Show)) {
            m_repositioning = true;
            // 内置页签按钮占位正常参与布局，等 Qt 布局完成后再将泡泡对齐到角落。
            QTimer::singleShot(0, this, [this]() {
                auto *bar = static_cast<QTabBar *>(parentWidget());
                if (bar->tabButton(m_tab, QTabBar::RightSide) == this) {
                    const QRect tab = bar->tabRect(m_tab);
                    constexpr int inset = 3;
                    constexpr int tabGap = 3;
                    move(tab.right() + 1 - width() - inset - tabGap, tab.top() + inset);
                }
                m_repositioning = false;
            });
        }
        return handled;
    }

private:
    const int m_tab;
    bool m_repositioning = false;
};

struct SyncRoundFeedback
{
    int running = 0;
    QStringList copied;
    QStringList failures;
    int upToDate = 0;

    void showResult(SyncCompletionToast &toast, bool feedback) const
    {
        if (copied.isEmpty() && !feedback) return;
        if (!failures.isEmpty()) {
            const QString title = copied.isEmpty() ? QStringLiteral("同步未全部完成")
                : QStringLiteral("%1 台同步完成，%2 台未完成").arg(copied.size()).arg(failures.size());
            toast.present(title, failures.join(QStringLiteral("、")), SyncCompletionToast::Warning);
        } else if (!copied.isEmpty()) {
            const QString title = copied.size() == 1 ? QStringLiteral("同步完成")
                : QStringLiteral("%1 台主机同步完成").arg(copied.size());
            toast.present(title, copied.join(QStringLiteral("、")), SyncCompletionToast::Success);
        } else if (upToDate > 0) {
            toast.present(QStringLiteral("文件已是最新，无需同步"), {}, SyncCompletionToast::Information);
        }
    }
};

QString normalizePathForUi(const QString &value)
{
    return QDir::toNativeSeparators(QDir::cleanPath(QDir::fromNativeSeparators(value.trimmed())));
}

bool removeMatchingReleaseDirs(QStringList &history, const QString &normalized)
{
    const auto firstRemoved = std::remove_if(history.begin(), history.end(), [&](const QString &item) {
        return normalizePathForUi(item).compare(normalized, Qt::CaseInsensitive) == 0;
    });
    if (firstRemoved == history.end()) return false;
    history.erase(firstRemoved, history.end());
    return true;
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

}

MainWindow::MainWindow(QWidget *parent, const QString &configPath, bool manageSystemStartup)
    : RoundedMainWindow(parent)
    , m_ui(new Ui::MainWindow)
    , m_configPath(configPath.isEmpty() ? ConfigManager::defaultConfigPath() : configPath)
    , m_manageSystemStartup(manageSystemStartup)
{
    m_uptime.start();
    setupUi();
    m_syncCompletionToast = std::make_unique<SyncCompletionToast>(this);
    m_copyPreview = new RecentCopyPreview(this);
    m_copyPreview->watchDetails(m_hostTable);
    loadConfiguration();
    m_shortcuts = std::make_unique<ShortcutManager>(this);
    m_shortcuts->add({"manualSync", QStringLiteral("手动同步"), [this]() { m_ui->syncButton->click(); }, QStringLiteral("同步")});
    m_shortcuts->add({"stopSync", QStringLiteral("停止所有同步"), [this]() { m_ui->stopButton->click(); }, QStringLiteral("同步")});
    applyShortcuts();

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
    m_pollTimer->setObjectName("pollTimer");
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollForChanges);
    updateTimerInterval();
    m_pollTimer->start();

    auto *lastSyncTimer = new QTimer(this);
    connect(lastSyncTimer, &QTimer::timeout, this, &MainWindow::refreshLastSyncTimes);
    connect(lastSyncTimer, &QTimer::timeout, this, &MainWindow::updateUptime);
    lastSyncTimer->start(1000);
    updateUptime();
    Diagnostics::maintain(m_config.logRetentionDays);
    auto *retentionTimer = new QTimer(this);
    connect(retentionTimer, &QTimer::timeout, this, [this]() { Diagnostics::maintain(m_config.logRetentionDays); });
    retentionTimer->start(60 * 60 * 1000);

    statusBar()->showMessage(QString("配置文件：%1").arg(QDir::toNativeSeparators(m_configPath)), 5000);
    QTimer::singleShot(300, this, [this]() {
        if (!m_backgroundTasksPaused) {
            startOperations(m_config.autoSync);
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
    Diagnostics::watch(this);
    QJsonArray hostDetails;
    for (const auto &host : m_config.hosts) hostDetails.append(QJsonObject{{"host", host.id}, {"name", host.name},
        {"share", host.sharePath}, {"user", host.username}, {"credential_configured", !host.password.isEmpty()}, {"enabled", host.enabled}});
    Diagnostics::event("config.loaded", {{"path", m_configPath}, {"release", m_config.releaseDir},
        {"hosts", hostDetails}, {"auto_sync", m_config.autoSync}, {"interval", m_config.scanIntervalSeconds},
        {"rules", m_config.syncRules.size()}, {"exclusions", QJsonArray::fromStringList(m_config.excludedFilePatterns)}});
}

MainWindow::~MainWindow()
{
    m_isClosing = true;
    m_shortcuts.reset();
    m_syncCompletionToast->dismiss();
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
    if (event->type() == QEvent::WindowDeactivate || event->type() == QEvent::Hide) {
        if (m_hostPasswordEdit) setPasswordVisible(false);
    }
    if (event->type() == QEvent::Show || event->type() == QEvent::Hide
        || event->type() == QEvent::WindowStateChange) {
        updateUpdateIndicators();
    }
    return handled;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_ui->showPasswordButton
        && (event->type() == QEvent::Leave || event->type() == QEvent::FocusOut
            || event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide
            || event->type() == QEvent::MouseButtonRelease)) {
        setPasswordVisible(false);
    }
    if (m_releaseDirEdit) {
        auto *history = m_releaseDirEdit->view();
        if (watched == history && event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            const QModelIndex index = history->currentIndex();
            if (key->key() == Qt::Key_Delete && key->modifiers() == Qt::NoModifier && index.isValid()) {
                removeReleaseDirHistory(index.data().toString());
                return true;
            }
        }
        if (watched == history->viewport()
            && (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonRelease)) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::RightButton) {
                // 拦截右键事件，以免误选条目并切换同步目录。
                if (event->type() == QEvent::MouseButtonRelease) {
                    const QModelIndex index = history->indexAt(mouse->pos());
                    if (index.isValid()) {
                        const QString path = index.data().toString();
                        QMenu menu(this);
                        QAction *remove = menu.addAction("移除这条历史记录");
                        if (menu.exec(mouse->globalPos()) == remove) removeReleaseDirHistory(path);
                    }
                }
                return true;
            }
        }
    }
    return RoundedMainWindow::eventFilter(watched, event);
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
    if (m_remoteDebugger && m_remoteDebugger->isDeploying()) {
        m_remoteDebugger->show();
        m_remoteDebugger->raise();
        RoundedMessageBox::warning(this, "远程操作进行中", "请等待远程安装或自启设置返回结果后再退出。");
        return;
    }

    m_isClosing = true;
    m_syncCompletionToast->dismiss();
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
    setFocusProxy(m_ui->syncTabs);
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        resize(QSize(1280, 860).boundedTo(screen->availableGeometry().size() - QSize(40, 60)));
    }

    m_ui->exclusionsEdit->setFixedHeight(m_ui->exclusionsEdit->fontMetrics().lineSpacing() * 4 + 16);
    m_ui->rulePreview->setFixedHeight(m_ui->rulePreview->fontMetrics().lineSpacing() * 2 + 20);
    m_ui->typeList->setMaximumHeight(m_ui->typeList->fontMetrics().lineSpacing() * 7 + 12);
    m_ui->localLayout->addStretch();
    m_ui->settingsLayout->addStretch();
    m_ui->localGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_ui->hostEditorGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_ui->syncTabs->setFixedHeight(m_ui->syncTabs->minimumSizeHint().height());

    // The scroll area can change its content size without moving the host list.
    auto *splitter = new QSplitter(Qt::Vertical, m_ui->centralWidget);
    splitter->setObjectName("workspaceSplitter");
    splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(8);
    m_ui->rootLayout->removeWidget(m_ui->settingsScroll);
    m_ui->rootLayout->removeWidget(m_ui->tableGroup);
    splitter->addWidget(m_ui->settingsScroll);
    splitter->addWidget(m_ui->tableGroup);
    m_ui->rootLayout->addWidget(splitter);
    splitter->setStretchFactor(1, 1);
    splitter->handle(1)->setToolTip("拖动调整设置区与主机列表的高度");
    m_releaseDirEdit = m_ui->releaseDirEdit;
    m_typeList = m_ui->typeList;
    m_newTypeEdit = m_ui->newTypeEdit;
    m_hostEnabledCheck = m_ui->hostEnabledCheck;
    m_hostNameEdit = m_ui->hostNameEdit;
    m_hostPathEdit = m_ui->hostPathEdit;
    m_hostUserEdit = m_ui->hostUserEdit;
    m_hostPasswordEdit = m_ui->hostPasswordEdit;
    m_ui->showPasswordButton->installEventFilter(this);
    connect(m_ui->showPasswordButton, &QToolButton::pressed, this, [this]() { setPasswordVisible(true); });
    connect(m_ui->showPasswordButton, &QToolButton::released, this, [this]() { setPasswordVisible(false); });
    setPasswordVisible(false);
    connect(m_ui->scanHostsButton, &QPushButton::clicked, this, &MainWindow::scanNetworkHosts);
    connect(m_ui->hostnameHelpButton, &QToolButton::clicked, this, &MainWindow::showHostnameHelp);
    m_hostTable = m_ui->hostTable;
    m_releaseDirEdit->lineEdit()->setPlaceholderText("选择或输入本地 Release 目录路径...");
    m_releaseDirEdit->setToolTip("展开列表后，右键点击条目可移除历史记录，也可以选中条目后按 Delete。");
    auto *releaseHistory = m_releaseDirEdit->view();
    releaseHistory->installEventFilter(this);
    releaseHistory->viewport()->installEventFilter(this);
    m_ui->stopButton->setIcon(style()->standardIcon(QStyle::SP_MediaStop));
    for (int column : {0, 1, 3, 5}) {
        m_hostTable->horizontalHeader()->setSectionResizeMode(column, QHeaderView::ResizeToContents);
    }
    m_hostTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::Fixed);
    m_hostTable->setColumnWidth(4, qMax(140, m_hostTable->fontMetrics().horizontalAdvance("100%") + 40));
    m_hostTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_hostTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_hostTable->horizontalHeader()->setSectionResizeMode(7, QHeaderView::Fixed);
    m_hostTable->setColumnWidth(7, 160);
    m_ui->ruleTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_ui->ruleTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_ui->ruleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    statusBar()->addPermanentWidget(m_ui->statusControls);
    const int workspaceHeight = height() - menuBar()->sizeHint().height() - statusBar()->sizeHint().height()
        - m_ui->rootLayout->contentsMargins().top() - m_ui->rootLayout->contentsMargins().bottom() - splitter->handleWidth();
    const int settingsHeight = qMin(m_ui->settingsContent->sizeHint().height() + 8, workspaceHeight * 2 / 3);
    splitter->setSizes({settingsHeight, workspaceHeight - settingsHeight});
    addAction(m_ui->usageTipsAction);
    connect(m_ui->usageTipsAction, &QAction::triggered, this, &MainWindow::showUsageTips);
    connect(m_ui->preferencesAction, &QAction::triggered, this, &MainWindow::showPreferences);
    connect(m_ui->checkUpdatesAction, &QAction::triggered, this, &MainWindow::checkForUpdates);
    connect(m_ui->saveConfigAction, &QAction::triggered, this, &MainWindow::saveConfiguration);
    connect(m_ui->remoteDebuggerAction, &QAction::triggered, this, &MainWindow::showRemoteDebugger);
    connect(m_ui->diagnosticsAction, &QAction::triggered, this, [this]() {
        const QString path = Diagnostics::directory();
        if (!QDir().mkpath(path) || !QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
            showNotice("诊断目录", "无法打开诊断目录：" + QDir::toNativeSeparators(path), true);
        }
    });
    connect(m_ui->settingsSummaryLabel, &QLabel::linkActivated, this, [this]() { showPreferences(); });
    connect(m_ui->updateNoticeLabel, &QLabel::linkActivated, this, [this]() { checkForUpdates(); });
    connect(m_ui->autoSyncAction, &QAction::toggled, this, [this](bool enabled) {
        if (m_isClosing) {
            updateSettingsSummary();
            return;
        }
        AppConfig preferences = m_config;
        preferences.autoSync = enabled;
        QString error;
        if (!applyPreferences(preferences, &error)) {
            Diagnostics::event("config.preferences_failed", {{"error", error}}, true);
            updateSettingsSummary();
            RoundedMessageBox::warning(this, "保存失败", error);
        }
    });
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
    updateUpdateIndicators();

    connect(m_ui->browseButton, &QPushButton::clicked, this, &MainWindow::browseReleaseDir);
    connect(m_ui->addTypeButton, &QPushButton::clicked, this, &MainWindow::addFileType);
    connect(m_ui->removeTypeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedFileType);
    connect(m_ui->invertTypeButton, &QPushButton::clicked, this, [this]() {
        if (!m_typeList->count() || m_isClosing) return;
        {
            const QSignalBlocker blocker(m_typeList);
            for (int index = 0; index < m_typeList->count(); ++index) {
                auto *item = m_typeList->item(index);
                item->setCheckState(item->checkState() == Qt::Checked ? Qt::Unchecked : Qt::Checked);
            }
        }
        handleTypeItemChanged(nullptr);
    });
    connect(m_ui->applyExclusionsButton, &QPushButton::clicked, this, &MainWindow::saveExclusions);
    connect(m_ui->exclusionsExampleButton, &QPushButton::clicked, this, &MainWindow::showExclusionsExample);
    connect(m_ui->exclusionsEdit, &QPlainTextEdit::textChanged, this, [this]() {
        m_ui->exclusionsError->hide();
        refreshSyncTabBadges();
    });
    connect(m_typeList, &QListWidget::itemChanged, this, &MainWindow::handleTypeItemChanged);
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
    connect(m_releaseDirEdit->lineEdit(), &QLineEdit::editingFinished, this, [this]() { onReleaseDirEditFinished(false); });
    connect(m_releaseDirEdit->lineEdit(), &QLineEdit::returnPressed, this, [this]() { onReleaseDirEditFinished(true); });
    connect(m_releaseDirEdit, QOverload<int>::of(&QComboBox::activated), this, [this]() { onReleaseDirEditFinished(true); });
}

void MainWindow::showRemoteDebugger()
{
    if (m_isClosing) return;
    if (!m_remoteDebugger) {
        m_remoteDebugger = new RemoteDebugger(this);
        connect(m_remoteDebugger, &RemoteDebugger::downloadFinished, this, [this](const QString &path, const QString &error) {
            if (error.isEmpty()) showNotice("远程调试器下载完成", QString("安装包已保存到“下载”：\n%1").arg(path), false);
            else showNotice("远程调试器下载失败", error, true);
        });
        connect(m_remoteDebugger, &RemoteDebugger::deploymentFinished, this, [this]() {
            m_remoteDebugger->setHosts(m_config.hosts, -1);
        });
    }
    m_remoteDebugger->setHosts(m_config.hosts, m_hostTable->currentRow());
    m_remoteDebugger->show();
    m_remoteDebugger->raise();
    m_remoteDebugger->activateWindow();
}

void MainWindow::setPasswordVisible(bool visible)
{
    if (m_hostPasswordEdit->echoMode() != (visible ? QLineEdit::Normal : QLineEdit::Password))
        Diagnostics::event("ui.password_visibility", {{"visible", visible}});
    m_hostPasswordEdit->setEchoMode(visible ? QLineEdit::Normal : QLineEdit::Password);
    QPixmap pixmap(48, 48);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(m_ui->showPasswordButton->palette().color(QPalette::ButtonText), 3.0));
    QPainterPath outline;
    outline.moveTo(5, 24);
    outline.cubicTo(15, 8, 33, 8, 43, 24);
    outline.cubicTo(33, 40, 15, 40, 5, 24);
    painter.drawPath(outline);
    painter.drawEllipse(QPointF(24, 24), 6, 6);
    if (!visible) painter.drawLine(QPointF(8, 7), QPointF(40, 41));
    painter.end();
    m_ui->showPasswordButton->setIcon(QIcon(pixmap));
    m_ui->showPasswordButton->setIconSize(QSize(20, 20));
}

void MainWindow::scanNetworkHosts()
{
    NetworkHostDialog dialog(this);
    QStringList configuredServers;
    for (const auto &host : m_config.hosts) {
        const QString path = normalizePathForUi(host.sharePath);
        if (path.startsWith("\\\\")) configuredServers.append(hostNameFromSharePath(path));
    }
    dialog.setConfiguredServers(configuredServers);
    connect(&dialog, &NetworkHostDialog::browseSharesRequested, this, &MainWindow::openShareInExplorer);
    if (dialog.exec() != QDialog::Accepted) return;
    const QString server = dialog.selectedServer();
    if (server.isEmpty()) return;
    QString path = normalizePathForUi(m_hostPathEdit->text());
    if (path.startsWith("\\\\")) {
        const int separator = path.indexOf('\\', 2);
        path = "\\\\" + server + (separator >= 0 ? path.mid(separator) : "\\");
    } else {
        path = "\\\\" + server + "\\";
    }
    m_hostPathEdit->setText(path);
    Diagnostics::event("host.scan_selection", {{"server", server}, {"path", path}, {"saved", false}});
    if (m_hostNameEdit->text().trimmed().isEmpty()) m_hostNameEdit->setText(server);
    m_hostPathEdit->setFocus();
}

void MainWindow::showHostnameHelp()
{
    QFile resource(":/hostname-help.html");
    if (!resource.open(QIODevice::ReadOnly)) {
        RoundedMessageBox::warning(this, "帮助", "无法读取主机名图文帮助。");
        return;
    }
    RoundedDialog dialog(this);
    dialog.setWindowTitle("如何查看远程电脑的主机名");
    dialog.resize(800, 650);
    if (QScreen *screen = QGuiApplication::screenAt(frameGeometry().center()))
        dialog.resize(QSize(800, 650).boundedTo(screen->availableGeometry().size() - QSize(32, 48)));
    auto *layout = new QVBoxLayout(&dialog);
    auto *browser = new QTextBrowser(&dialog);
    browser->setHtml(QString::fromUtf8(resource.readAll()));
    layout->addWidget(browser);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    dialog.exec();
}

void MainWindow::showNotice(const QString &title, const QString &message, bool warning, const QString &details)
{
    if (m_isClosing) return;
    if ((!isVisible() || isMinimized()) && m_trayIcon && QSystemTrayIcon::supportsMessages()) {
        m_trayIcon->showMessage(title, message, warning ? QSystemTrayIcon::Warning : QSystemTrayIcon::Information, 12000);
        statusBar()->showMessage(title + " · " + message.section('\n', 0, 0), 12000);
        return;
    }

    QWidget *parent = m_remoteDebugger && m_remoteDebugger->isActiveWindow()
        ? static_cast<QWidget *>(m_remoteDebugger) : this;
    auto *notice = new RoundedMessageBox(warning ? QMessageBox::Warning : QMessageBox::Information,
                                   title, message, QMessageBox::Ok, parent);
    notice->setTextFormat(Qt::PlainText);
    if (!details.isEmpty()) notice->setDetailedText(details);
    notice->setAttribute(Qt::WA_DeleteOnClose);
    notice->setModal(false);
    notice->show();
}

void MainWindow::updateUptime()
{
    const qint64 seconds = m_uptime.elapsed() / 1000;
    const QString clock = QString("%1:%2:%3")
        .arg(seconds / 3600 % 24, 2, 10, QLatin1Char('0'))
        .arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    const qint64 days = seconds / (24 * 3600);
    m_ui->uptimeLabel->setText(days > 0 ? QString("运行 %1 天 %2").arg(days).arg(clock) : "运行 " + clock);
}

void MainWindow::saveExclusions()
{
    if (m_isClosing) return;
    QString error;
    QStringList patterns;
    const auto entries = m_ui->exclusionsEdit->toPlainText().split(QRegularExpression("[;；\\n]"), QString::SkipEmptyParts);
    if (SyncPlan::normalizeExclusions(entries, &patterns, &error)) {
        collectUiToConfig();
        if (patterns == m_config.excludedFilePatterns) return;
        AppConfig next = m_config;
        next.excludedFilePatterns = patterns;
        for (HostEntry &host : next.hosts) host.lastSyncUtc = QDateTime();
        if (ConfigManager::save(m_configPath, next, &error)) {
            m_config = std::move(next);
            m_symbolWarnings.clear();
            for (const HostEntry &host : m_config.hosts) ++m_syncContextRevisions[host.id];
            refreshLastSyncTimes();
            m_ui->exclusionsEdit->setPlainText(patterns.join('\n'));
            m_lastFingerprint.clear();
            Diagnostics::record("config.exclusions_saved", QString("count=%1").arg(patterns.size()));
            statusBar()->showMessage("排除规则已保存", 3000);
            requestAutoSync("排除规则已更新，开始自动同步");
            return;
        }
    }
    m_ui->exclusionsError->setText(error);
    m_ui->exclusionsError->show();
}

void MainWindow::showExclusionsExample()
{
    QFile example(":/exclusions-example.html");
    if (!example.open(QIODevice::ReadOnly)) {
        showNotice("排除文件示例", "无法加载排除文件示例。", true);
        return;
    }
    const QByteArray html = example.readAll();
    if (example.error() != QFile::NoError) {
        showNotice("排除文件示例", "无法读取排除文件示例。", true);
        return;
    }
    auto *dialog = new RoundedDialog(this);
    dialog->setObjectName("exclusionsExampleDialog");
    dialog->setWindowTitle("排除文件示例");
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto *layout = new QVBoxLayout(dialog);
    auto *document = new QTextBrowser(dialog);
    document->setObjectName("exclusionsExampleDocument");
    document->setAccessibleName("排除规则示例图和通配符说明");
    document->document()->setDocumentMargin(10);
    document->setHtml(QString::fromUtf8(html));
    layout->addWidget(document);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    layout->addWidget(buttons);

    QScreen *screen = QGuiApplication::screenAt(frameGeometry().center());
    const QSize preferred(690, qMax(420, document->fontMetrics().lineSpacing() * 17 + 76));
    dialog->resize(screen ? preferred.boundedTo(screen->availableGeometry().size() - QSize(32, 48)) : preferred);
    dialog->open();
}

void MainWindow::applyShortcuts()
{
    const QString issues = m_shortcuts->apply(m_config.shortcuts);
    const auto setHint = [&](QPushButton *button, const QString &action, QString hint) {
        for (const auto &binding : m_config.shortcuts)
            if (binding.action == action) hint += QStringLiteral("（%1）").arg(binding.keys);
        if (!issues.isEmpty()) hint += '\n' + issues;
        button->setToolTip(hint);
    };
    setHint(m_ui->syncButton, QStringLiteral("manualSync"), QStringLiteral("手动同步"));
    setHint(m_ui->stopButton, QStringLiteral("stopSync"), QStringLiteral("停止所有同步并关闭自动同步"));
    if (!issues.isEmpty()) {
        statusBar()->showMessage(issues);
        Diagnostics::event("shortcut.unavailable", {{"reason", issues}}, true);
    }
}

void MainWindow::showPreferences()
{
    if (m_isClosing) return;
    if (m_preferencesDialog != nullptr) {
        m_preferencesDialog->raise();
        m_preferencesDialog->activateWindow();
        return;
    }
    RoundedDialog dialog(this);
    Ui::PreferencesDialog ui;
    ui.setupUi(&dialog);
    ui.buttonBox->button(QDialogButtonBox::Save)->setText("保存");
    ui.buttonBox->button(QDialogButtonBox::Cancel)->setText("取消");
    m_preferencesDialog = &dialog;
    // 暂时释放本程序的全局注册，使原组合键可以被录入，也避免探测到自己的注册。
    m_shortcuts->suspend();
    auto *shortcuts = new ShortcutPreferences(*m_shortcuts, m_config.shortcuts, ui.shortcutsTab);
    ui.shortcutsLayout->addWidget(shortcuts);
    ui.shortcutFeedbackCheck->setChecked(m_config.showShortcutFeedback);
    ui.autoSyncCheck->setChecked(m_config.autoSync);
    ui.startWithSystemCheck->setChecked(m_config.startWithSystem);
    ui.intervalSpin->setValue(m_config.scanIntervalSeconds);
    ui.checkUpdatesOnStartupCheck->setChecked(m_config.checkUpdatesOnStartup);
    ui.silentUpdateCheck->setChecked(m_config.silentUpdate);
    ui.logRetentionSpin->setValue(m_config.logRetentionDays);
    dialog.ensurePolished();
    // 首次定高时计入整页内容，随后放开限制，让小窗口可以滚动。
    ui.generalScroll->setMinimumHeight(ui.generalContent->sizeHint().height());
    int preferredHeight = qMax(dialog.height(), dialog.sizeHint().height());
    ui.generalScroll->setMinimumHeight(0);
    if (QScreen *screen = QGuiApplication::screenAt(frameGeometry().center())) {
        const int available = screen->availableGeometry().height();
        dialog.setMinimumHeight(qMin(dialog.minimumHeight(), available));
        preferredHeight = qMin(preferredHeight, available);
    }
    dialog.resize(dialog.width(), preferredHeight);
    connect(ui.buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(ui.buttonBox, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (!shortcuts->validate()) {
            ui.preferencesTabs->setCurrentWidget(ui.shortcutsTab);
            ui.errorLabel->setText(QStringLiteral("请检查快捷键格式，或清除该绑定。"));
            ui.errorLabel->show();
            return;
        }
        AppConfig preferences = m_config;
        preferences.shortcuts = shortcuts->bindings();
        preferences.showShortcutFeedback = ui.shortcutFeedbackCheck->isChecked();
        preferences.autoSync = ui.autoSyncCheck->isChecked();
        preferences.startWithSystem = ui.startWithSystemCheck->isChecked();
        preferences.scanIntervalSeconds = ui.intervalSpin->value();
        preferences.checkUpdatesOnStartup = ui.checkUpdatesOnStartupCheck->isChecked();
        preferences.silentUpdate = ui.silentUpdateCheck->isChecked();
        preferences.logRetentionDays = ui.logRetentionSpin->value();
        QString error;
        if (!applyPreferences(preferences, &error)) {
            ui.errorLabel->setText(error);
            ui.errorLabel->show();
            return;
        }
        dialog.accept();
    });
    dialog.exec();
    m_preferencesDialog.clear();
    applyShortcuts();
}

bool MainWindow::applyPreferences(const AppConfig &preferences, QString *errorMessage)
{
    if (m_isClosing) return false;
    collectUiToConfig();
    // Keep current host records: a copy can finish while the dialog is open.
    AppConfig next = m_config;
    next.autoSync = preferences.autoSync;
    next.shortcuts = preferences.shortcuts;
    next.showShortcutFeedback = preferences.showShortcutFeedback;
    next.startWithSystem = preferences.startWithSystem;
    next.scanIntervalSeconds = qBound(1, preferences.scanIntervalSeconds, 3600);
    next.checkUpdatesOnStartup = preferences.checkUpdatesOnStartup;
    next.silentUpdate = preferences.silentUpdate;
    next.logRetentionDays = qBound(0, preferences.logRetentionDays, 365);

    const bool previousStartup = m_manageSystemStartup && isSystemAutoStartEnabled();
    const bool startupChanged = m_manageSystemStartup && previousStartup != next.startWithSystem;
    if (startupChanged && !setSystemAutoStartEnabled(next.startWithSystem, errorMessage)) return false;
    if (!ConfigManager::save(m_configPath, next, errorMessage)) {
        if (startupChanged) {
            QString rollbackError;
            if (!setSystemAutoStartEnabled(previousStartup, &rollbackError) && errorMessage != nullptr)
                *errorMessage += QString("\n恢复开机自启设置失败：%1").arg(rollbackError);
        }
        return false;
    }
    const bool enableAutoSync = !m_config.autoSync && next.autoSync;
    const bool shortcutsChanged = next.shortcuts != m_config.shortcuts;
    const bool silentUpdateChanged = m_config.silentUpdate != next.silentUpdate;
    m_config = std::move(next);
    Diagnostics::record("config.preferences_saved", QString("auto=%1 interval=%2 retention_days=%3")
                        .arg(m_config.autoSync).arg(m_config.scanIntervalSeconds).arg(m_config.logRetentionDays));
    Diagnostics::maintain(m_config.logRetentionDays);
    updateTimerInterval();
    updateSettingsSummary();
    statusBar()->showMessage("设置已保存", 3000);
    if (shortcutsChanged && !m_preferencesDialog) applyShortcuts();
    if (silentUpdateChanged) {
        if (m_config.silentUpdate) {
            m_silentInstallFailedVersion.clear();
            if (!m_availableUpdateVersion.isEmpty() && !isReadyInstallerCurrent()) startInstallerDownload(false);
        } else if (m_installerReply != nullptr && !m_installerDownloadInteractive) {
            abortInstallerDownload();
        }
    }
    if (enableAutoSync) {
        m_backgroundTasksPaused = false;
        requestAutoSync("已开启自动同步");
    }
    return true;
}

void MainWindow::updateSettingsSummary()
{
    QSignalBlocker blocker(m_ui->autoSyncAction);
    m_ui->autoSyncAction->setChecked(m_config.autoSync);
    const QString summary = QString("自动同步：%1 · 扫描间隔：%2 秒")
        .arg(m_config.autoSync ? "开启" : "关闭").arg(m_config.scanIntervalSeconds);
    m_ui->settingsSummaryLabel->setText(
        QString("<a href=\"preferences\" style=\"color: #52606d; text-decoration: none;\">%1</a>").arg(summary));
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
    refreshSyncTabBadges();
    updateRulePreview();
}

void MainWindow::refreshSyncTabBadges()
{
    auto *tabBar = m_ui->syncTabs->tabBar();
    const auto setBadge = [&](QWidget *page, int count) {
        const int tab = m_ui->syncTabs->indexOf(page);
        auto *previous = tabBar->tabButton(tab, QTabBar::RightSide);
        const QString text = QString::number(count);
        if (auto *label = qobject_cast<QLabel *>(previous); count > 0 && label && label->text() == text) return;
        if (previous) {
            tabBar->setTabButton(tab, QTabBar::RightSide, nullptr);
            previous->deleteLater();
        }
        if (count <= 0) {
            tabBar->setTabToolTip(tab, {});
            return;
        }
        auto *badge = new SyncTabCountBadge(text, tabBar, tab);
        badge->setObjectName("syncTabCountBadge");
        badge->setAttribute(Qt::WA_TransparentForMouseEvents);
        badge->setAlignment(Qt::AlignCenter);
        badge->ensurePolished();
        const QFontMetrics metrics = badge->fontMetrics();
        badge->setFixedSize(qMax(18, metrics.horizontalAdvance(text) + 10), qMax(18, metrics.height() + 2));
        tabBar->setTabButton(tab, QTabBar::RightSide, badge);
        tabBar->setTabToolTip(tab, QStringLiteral("%1：%2 项").arg(tabBar->tabText(tab), text));
    };
    int selected = 0;
    for (int row = 0; row < m_typeList->count(); ++row) {
        if (m_typeList->item(row)->checkState() == Qt::Checked) ++selected;
    }
    QStringList exclusions;
    QString error;
    const auto entries = m_ui->exclusionsEdit->toPlainText().split(QRegularExpression("[;；\\n]"), QString::SkipEmptyParts);
    if (!SyncPlan::normalizeExclusions(entries, &exclusions, &error)) exclusions = m_config.excludedFilePatterns;
    setBadge(m_ui->basicTab, selected);
    setBadge(m_ui->exclusionsTab, exclusions.size());
    setBadge(m_ui->rulesTab, m_config.syncRules.size());
}

void MainWindow::updateRulePreview()
{
    if (m_isClosing) return;
    const int row = m_ui->ruleTable->currentRow();
    const bool selected = row >= 0 && row < m_config.syncRules.size();
    m_ui->editRuleButton->setEnabled(selected);
    m_ui->removeRuleButton->setEnabled(selected);
    if (!selected) {
        m_ui->rulePreview->setPlainText("路径相对于两端的 Release 目录。./image 在其内部，../Lang 在其同级。双击规则可编辑。");
        return;
    }
    const QString relative = m_config.syncRules.at(row).relativePath;
    const QString local = SyncPlan::resolvePath(m_releaseDirEdit->currentText(), relative);
    const QString remote = SyncPlan::resolvePath(m_hostPathEdit->text(), relative);
    QString error;
    const bool valid = SyncPlan::validateRemotePath(m_hostPathEdit->text(), relative, &error);
    m_ui->rulePreview->setPlainText(QString("本地  %1\n远端  %2")
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
    m_symbolWarnings.clear();
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
    RoundedDialog dialog(this);
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
            RoundedMessageBox::information(&dialog, "选择 Release 目录", "请先在主窗口设置本地 Release 目录。");
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
            RoundedMessageBox::warning(&dialog, "规则无效", error);
            return;
        }
        result.enabled = ui.enabledCheck->isChecked();
        if (result.enabled) {
            for (const HostEntry &host : m_config.hosts) {
                if (!SyncPlan::validateRemotePath(host.sharePath, result.relativePath, &error)) {
                    RoundedMessageBox::warning(&dialog, "规则无效", QString("主机 %1：%2").arg(host.name, error));
                    return;
                }
            }
            QVector<SyncPlan::Scope> scopes;
            if (!SyncPlan::build(m_releaseDirEdit->currentText(), QString(), {}, {result}, &scopes, &error)) {
                RoundedMessageBox::warning(&dialog, "规则无效", error);
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

    auto *dialog = new RoundedDialog(this);
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
    Diagnostics::event("update.check_requested", {{"interactive", interactive}, {"action", Diagnostics::lastUserAction()}});
    if (m_isClosing) {
        return;
    }
    if (m_updateCheckInProgress) {
        m_updateCheckInteractive = m_updateCheckInteractive || interactive;
        return;
    }
    m_updateCheckInProgress = true;
    m_updateCheckInteractive = interactive;
    updateUpdateIndicators();
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
    Diagnostics::event("update.check_finished", {{"error", errorMessage}, {"available_version", m_availableUpdateVersion},
        {"installer_url", m_availableInstallerUrl.toString()}}, !errorMessage.isEmpty());
    const bool interactive = m_updateCheckInteractive;
    m_updateCheckInProgress = false;
    m_updateCheckInteractive = false;
    updateUpdateIndicators();
    if (m_isClosing) {
        return;
    }
    if (errorMessage.isEmpty() && !m_availableUpdateVersion.isEmpty() && m_config.silentUpdate
        && !isReadyInstallerCurrent() && m_installerDownloadVersion != m_availableUpdateVersion) {
        startInstallerDownload(false);
    }
    if (!interactive) {
        return;
    }
    if (!errorMessage.isEmpty()) {
        RoundedMessageBox::warning(this, "检查更新失败", errorMessage);
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
    updateUpdateIndicators();

    RoundedDialog dialog(this);
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
        setUpdateDialogStatus(QString("安装包已下载并通过校验（%1），点击“更新”立即安装，程序会自动重启。")
                                  .arg(DownloadProgress::size(QFileInfo(m_readyInstallerPath).size())), 100);
    } else if (m_installerReply != nullptr) {
        setUpdateDialogStatus(m_installerProgressText.isEmpty() ? QString("正在连接下载服务器...") : m_installerProgressText,
                              m_installerProgressText.isEmpty() ? -1 : m_installerProgressPercent);
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
        if (m_installerReply != nullptr && m_installerDownloadInteractive && !m_config.silentUpdate) {
            abortInstallerDownload();
        }
        m_installerDownloadInteractive = false;
    }
    updateUpdateIndicators();
    if (!m_isClosing && result == kInstallDialogResult) {
        installUpdateNow();
    }
}

void MainWindow::openUpdateUrl(const QUrl &url)
{
    const QUrl target = url.isValid() ? url : QUrl(QString::fromLatin1(kReleaseListUrl));
    if (!QDesktopServices::openUrl(target)) {
        RoundedMessageBox::warning(this, "打开页面失败", QString("请在浏览器中访问：\n%1").arg(target.toString()));
    }
}

void MainWindow::showUpdateHistoryLink(const QString &message)
{
    RoundedMessageBox box(this);
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
    m_installerProgressText = text;
    m_installerProgressPercent = percent < 0 ? -1 : qBound(0, percent, 100);
    if (m_updateDialogStatus != nullptr) {
        m_updateDialogStatus->setText(text);
    }
    DownloadProgress::apply(m_updateDialogProgress.data(), m_installerProgressPercent);
    updateUpdateIndicators();
}

void MainWindow::startInstallerDownload(bool interactive)
{
    Diagnostics::event("update.download_requested", {{"interactive", interactive}, {"version", m_availableUpdateVersion}, {"url", m_availableInstallerUrl.toString()}});
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
    requestInstallerChecksum();
}

void MainWindow::requestInstallerChecksum()
{
    const QUrl checksumUrl(m_installerDownloadUrl.toString() + ".sha256");
    QNetworkReply *reply = m_updateNetworkManager->get(makeDownloadRequest(checksumUrl));
    m_installerReply = reply;
    setUpdateDialogStatus("正在连接校验文件服务器...", -1);
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply](qint64 received, qint64 total) {
        if (m_installerReply != reply || m_isClosing
            || reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        setUpdateDialogStatus(QString("获取校验文件：%1").arg(DownloadProgress::text(received, total)),
                              DownloadProgress::percentage(received, total));
    });

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
    Diagnostics::event("update.download_request", {{"url", m_installerDownloadUrl.toString()}, {"expected_sha256", expectedSha256}});
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
    setUpdateDialogStatus("正在连接安装包下载服务器...", -1);

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
    connect(reply, &QNetworkReply::downloadProgress, this, [this, reply, stallTimer](qint64 received, qint64 total) {
        if (m_installerReply != reply || m_isClosing) return;
        stallTimer->start(kInstallerStallTimeoutMs);
        if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) return;
        const QString stage = total > 0 && received >= total
            ? QString("正在完成下载与校验：%1") : QString("下载安装包：%1");
        setUpdateDialogStatus(stage.arg(DownloadProgress::text(received, total)), DownloadProgress::percentage(received, total));
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

        setUpdateDialogStatus(QString("正在校验安装包（%1）...").arg(DownloadProgress::size(m_installerFile->size())), -1);
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
    if (m_installerReply) Diagnostics::event("update.download_abort", {{"version", m_installerDownloadVersion}, {"url", m_installerDownloadUrl.toString()}});
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
    m_installerProgressText.clear();
    m_installerProgressPercent = 0;
    updateUpdateIndicators();
}

void MainWindow::failInstallerDownload(const QString &errorMessage)
{
    Diagnostics::event("update.download_failed", {{"version", m_installerDownloadVersion}, {"url", m_installerDownloadUrl.toString()}, {"error", errorMessage}}, true);
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
        RoundedMessageBox::warning(this, "下载更新失败", errorMessage);
    } else {
        statusBar()->showMessage(QString("后台下载更新失败，将在下次检查时重试：%1").arg(errorMessage.section('\n', 0, 0)), 8000);
    }
}

void MainWindow::finishInstallerDownload()
{
    Diagnostics::event("update.download_completed", {{"version", m_installerDownloadVersion}, {"path", m_installerFile ? m_installerFile->fileName() : QString()}});
    const bool installNow = m_installAfterDownload;
    m_installerDownloadVersion.clear();
    m_installerDownloadUrl = QUrl();
    m_installerDownloadInteractive = false;
    m_installAfterDownload = false;
    if (m_isClosing) {
        return;
    }
    if (installNow && m_updateDialog != nullptr) {
        setUpdateDialogStatus(QString("安装包已下载并通过校验（%1），正在准备安装...")
                                  .arg(DownloadProgress::size(QFileInfo(m_readyInstallerPath).size())), 100);
        m_updateDialog->done(kInstallDialogResult);
        return;
    }
    setUpdateDialogStatus(QString("安装包已下载并通过校验（%1），点击“更新”立即安装，程序会自动重启。")
                              .arg(DownloadProgress::size(QFileInfo(m_readyInstallerPath).size())), 100);
    if (m_updateDialog != nullptr) {
        if (m_updateDialogInstallButton != nullptr) {
            m_updateDialogInstallButton->setEnabled(true);
            m_updateDialogInstallButton->setText("更新");
        }
    }
    if (m_config.silentUpdate) {
        statusBar()->showMessage(QString("新版本 %1 已下载，将在没有同步任务时自动安装。").arg(m_readyInstallerVersion), 8000);
        QTimer::singleShot(0, this, [this]() {
            installSilentUpdateIfIdle();
        });
    }
}

bool MainWindow::launchInstaller(bool silent, QString *errorMessage)
{
    Diagnostics::event("update.install_requested", {{"silent", silent}, {"version", m_readyInstallerVersion}, {"path", m_readyInstallerPath},
        {"directory", QCoreApplication::applicationDirPath()}});
    if (m_remoteDebugger && m_remoteDebugger->isDeploying()) {
        if (errorMessage) *errorMessage = "远程部署正在进行，请等待完成后再更新本程序。";
        return false;
    }
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
        && RoundedMessageBox::question(this, "安装更新", "当前有同步任务正在运行，安装更新会停止这些任务。是否继续？")
               != QMessageBox::Yes) {
        return;
    }
    QString error;
    if (!launchInstaller(false, &error)) {
        RoundedMessageBox::warning(this, "安装更新失败", QString("%1\n可前往发布页手动下载安装包。").arg(error));
        return;
    }
    m_isInstallingUpdate = true;
    realExit();
}

bool MainWindow::installSilentUpdateIfIdle()
{
    if (m_isClosing || m_isInstallingUpdate || !m_config.silentUpdate || !isReadyInstallerCurrent()
        || m_silentInstallFailedVersion == m_readyInstallerVersion || m_updateDialogOpen
        || hasRunningWorkers() || (m_remoteDebugger && m_remoteDebugger->isDeploying())
        || m_pendingSyncAfterCurrentRun || (isVisible() && isActiveWindow())
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

void MainWindow::updateUpdateIndicators()
{
    const bool hasUpdate = !m_availableUpdateVersion.isEmpty();
    m_ui->checkUpdatesAction->setEnabled(!m_isClosing && (!m_updateCheckInProgress || hasUpdate));
    m_ui->checkUpdatesAction->setText(hasUpdate
        ? QString("发现新版本 %1...").arg(m_availableUpdateVersion)
        : (m_updateCheckInProgress ? QString("正在检查更新...") : QString("检查更新...")));
    m_ui->checkUpdatesAction->setToolTip(hasUpdate
        ? QString("发现版本 %1，点击查看更新说明并选择是否更新。").arg(m_availableUpdateVersion)
        : QString("手动检查软件更新；程序每 2 小时自动检查一次。"));
    m_ui->updateNoticeLabel->setVisible(hasUpdate && !m_isClosing);
    const bool downloading = m_installerReply != nullptr && !m_installerProgressText.isEmpty();
    const QString notice = downloading ? m_installerProgressText : QString("发现新版本 %1").arg(m_availableUpdateVersion);
    m_ui->updateNoticeLabel->setText(hasUpdate
        ? QString("<a href=\"update\">%1</a>").arg(notice.toHtmlEscaped()) : QString());
    m_ui->updateNoticeLabel->setToolTip(downloading ? m_installerProgressText : m_ui->checkUpdatesAction->toolTip());
}

void MainWindow::stopUpdateChecks()
{
    if (m_updateCheckTimer != nullptr) {
        m_updateCheckTimer->stop();
    }
    m_updateCheckInProgress = false;
    m_updateCheckInteractive = false;
    if (m_updateReply != nullptr) {
        m_updateReply->abort();
        m_updateReply.clear();
    }
    abortInstallerDownload();
}

void MainWindow::onReleaseDirEditFinished(bool explicitSelection)
{
    if (m_isClosing) return;
    const QString path = normalizePathForUi(m_releaseDirEdit->currentText());
    if (path == normalizePathForUi(m_config.releaseDir)
        && (!explicitSelection || path.isEmpty() || path == m_config.releaseDirHistory.value(0))) {
        return;
    }
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
    removeMatchingReleaseDirs(m_config.releaseDirHistory, normalized);

    // 插入到最前面
    m_config.releaseDirHistory.prepend(normalized);

    // 限制历史记录数量
    while (m_config.releaseDirHistory.size() > kMaxReleaseDirHistory) {
        m_config.releaseDirHistory.removeLast();
    }

    refreshReleaseDirHistory(normalized);
}

void MainWindow::removeReleaseDirHistory(const QString &path)
{
    if (m_isClosing || path.trimmed().isEmpty()) return;
    AppConfig updated = m_config;
    if (!removeMatchingReleaseDirs(updated.releaseDirHistory, normalizePathForUi(path))) return;

    const QString currentText = m_releaseDirEdit->currentText();
    {
        const QSignalBlocker editor(m_releaseDirEdit->lineEdit());
        m_releaseDirEdit->hidePopup();
    }
    // 只保存历史变更，避免把尚未提交的输入当作新的同步目录。
    QString error;
    if (!ConfigManager::save(m_configPath, updated, &error)) {
        Diagnostics::event("config.write_failed", {{"path", m_configPath}, {"error", error}}, true);
        RoundedMessageBox::warning(this, "保存失败", QString("配置文件写入失败：%1").arg(error));
        return;
    }
    m_config.releaseDirHistory = updated.releaseDirHistory;
    refreshReleaseDirHistory(currentText);
    statusBar()->showMessage("已移除 Release 目录历史记录", 3000);
}

void MainWindow::refreshReleaseDirHistory(const QString &currentText)
{
    QSignalBlocker blocker(m_releaseDirEdit);
    m_releaseDirEdit->clear();
    m_releaseDirEdit->addItems(m_config.releaseDirHistory);
    const int currentIndex = m_releaseDirEdit->findText(currentText, Qt::MatchExactly);
    m_releaseDirEdit->setCurrentIndex(currentIndex);
    if (currentIndex < 0) m_releaseDirEdit->setEditText(currentText);
}

void MainWindow::loadConfiguration()
{
    m_config = ConfigManager::load(m_configPath);
    // Preserve the former spin box's limits before starting the polling timer.
    m_config.scanIntervalSeconds = qBound(1, m_config.scanIntervalSeconds, 3600);
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
    m_ui->exclusionsEdit->setPlainText(m_config.excludedFilePatterns.join('\n'));
    refreshReleaseDirHistory(normalizePathForUi(m_config.releaseDir));

    updateSettingsSummary();

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
        m_symbolWarnings.clear();
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
    m_checkDisplays.clear();
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
        statusItem->setTextAlignment(Qt::AlignCenter);
        auto *detailItem = new QTableWidgetItem("等待状态刷新");
        pathItem->setToolTip(sharePathTooltip(pathItem->text()));
        m_hostTable->setItem(row, 1, nameItem);
        m_hostTable->setItem(row, 2, pathItem);
        m_hostTable->setItem(row, 3, userItem);
        m_hostTable->setItem(row, 5, statusItem);
        m_hostTable->setItem(row, 6, detailItem);
        m_hostTable->setItem(row, 7, new QTableWidgetItem());

        auto *progressCell = new QWidget(m_hostTable);
        auto *progressLayout = new QHBoxLayout(progressCell);
        progressLayout->setContentsMargins(6, 2, 6, 2);
        auto *progressBar = new QProgressBar(progressCell);
        progressBar->setMinimumWidth(0);
        progressBar->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Expanding);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setFormat("%p%");
        m_copyPreview->watch(progressBar, host.id);
        progressLayout->addWidget(progressBar);
        m_hostTable->setCellWidget(row, 4, progressCell);

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
    Diagnostics::rememberSecret(m_hostPasswordEdit->text());
    Diagnostics::event("host.save_requested", {{"update", updateExisting}, {"row", m_hostTable->currentRow()},
        {"name", m_hostNameEdit->text()}, {"share", m_hostPathEdit->text()}, {"user", m_hostUserEdit->text()},
        {"credential_configured", !m_hostPasswordEdit->text().isEmpty()}, {"enabled", m_hostEnabledCheck->isChecked()}});
    const QString sharePath = normalizePathForUi(m_hostPathEdit->text());
    if (sharePath.isEmpty()) {
        RoundedMessageBox::warning(this, "提示", "请填写共享目录路径。");
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
            RoundedMessageBox::warning(this, "提示", "请在表格中选择一台主机后再更新。");
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
    Diagnostics::event("host.saved", {{"host", host.id}, {"name", host.name}, {"share", host.sharePath},
        {"user", host.username}, {"enabled", host.enabled}, {"update", updateExisting}});
    saveConfiguration();
    if (host.enabled) {
        requestAutoSync(QString("主机 [%1] 配置已更新，开始自动同步").arg(hostDisplayText(host)));
    }
}

void MainWindow::removeSelectedHost()
{
    const int row = m_hostTable->currentRow();
    if (row < 0 || row >= m_config.hosts.size()) {
        RoundedMessageBox::warning(this, "提示", "请先选择需要删除的主机。");
        return;
    }

    if (m_runningThreads.contains(m_config.hosts.at(row).id)) {
        RoundedMessageBox::warning(this, "提示", "该主机正在执行任务，请等待完成后再删除。");
        return;
    }

    m_syncContextRevisions.remove(m_config.hosts.at(row).id);
    m_copyPreview->removeHost(m_config.hosts.at(row).id);
    Diagnostics::event("host.removed", {{"host", m_config.hosts.at(row).id}, {"name", m_config.hosts.at(row).name},
        {"share", m_config.hosts.at(row).sharePath}});
    m_config.hosts.removeAt(row);
    refreshHostTable();
    clearHostEditor();
    saveConfiguration();
}

void MainWindow::clearHostEditor()
{
    setPasswordVisible(false);
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
    setPasswordVisible(false);
    const int row = m_hostTable->currentRow();
    if (row < 0 || row >= m_config.hosts.size()) {
        return;
    }

    const HostEntry &host = m_config.hosts.at(row);
    Diagnostics::event("host.selected", {{"host", host.id}, {"row", row}, {"name", host.name}, {"share", host.sharePath}});
    m_hostEnabledCheck->setChecked(host.enabled);
    m_hostNameEdit->setText(host.name);
    m_hostPathEdit->setText(normalizePathForUi(host.sharePath));
    m_hostUserEdit->setText(host.username);
    m_hostPasswordEdit->setText(host.password);
}

void MainWindow::saveConfiguration()
{
    collectUiToConfig();
    refreshSyncTabBadges();

    QString error;
    if (!ConfigManager::save(m_configPath, m_config, &error)) {
        Diagnostics::event("config.write_failed", {{"path", m_configPath}, {"error", error}}, true);
        RoundedMessageBox::warning(this, "保存失败", QString("配置文件写入失败：%1").arg(error));
        return;
    }
    if (m_manageSystemStartup && !setSystemAutoStartEnabled(m_config.startWithSystem, &error)) {
        RoundedMessageBox::warning(this, "自启设置失败", QString("开机自启设置写入失败：%1").arg(error));
        return;
    }

    if (m_remoteDebugger) m_remoteDebugger->setHosts(m_config.hosts, -1);
    statusBar()->showMessage("配置已保存", 3000);
    Diagnostics::record("config.saved", QString("hosts=%1 types=%2 rules=%3")
                        .arg(m_config.hosts.size()).arg(m_config.selectedFileTypes.size()).arg(m_config.syncRules.size()));
    Diagnostics::event("config.snapshot", {{"path", m_configPath}, {"release", m_config.releaseDir},
        {"auto_sync", m_config.autoSync}, {"startup", m_config.startWithSystem}, {"silent_update", m_config.silentUpdate},
        {"types", QJsonArray::fromStringList(m_config.selectedFileTypes)}, {"exclusions", QJsonArray::fromStringList(m_config.excludedFilePatterns)}});
}

void MainWindow::syncNow()
{
    if (m_isClosing) {
        return;
    }

    m_backgroundTasksPaused = false;
    const bool fromShortcut = m_shortcuts && m_shortcuts->isDispatching();
    if (hasRunningWorkers()) {
        if (fromShortcut && m_config.showShortcutFeedback) {
            m_syncCompletionToast->present(m_pendingSyncAfterCurrentRun ? QStringLiteral("手动同步已在队列中")
                : QStringLiteral("手动同步已排队"), QStringLiteral("当前任务结束后执行"), SyncCompletionToast::Information);
        }
        m_pendingShortcut |= fromShortcut;
        m_pendingSyncAfterCurrentRun = true;
        statusBar()->showMessage("当前正在检查/同步，完成后会自动执行一次同步", 3000);
    } else {
        startOperations(true, false, fromShortcut);
    }
    m_lastFingerprint = currentLocalFingerprint();
}

void MainWindow::stopAllOperations()
{
    Diagnostics::event("sync.stop_all_requested", {{"running", m_runningThreads.size()}, {"auto_sync", m_config.autoSync}});
    if (m_isClosing) {
        return;
    }

    m_backgroundTasksPaused = true;
    m_pendingSyncAfterCurrentRun = false;
    m_pendingShortcut = false;
    m_pendingSyncStatusMessage.clear();
    m_config.autoSync = false;
    updateSettingsSummary();
    requestWorkersToStop();
    saveConfiguration();
    const QString status = hasRunningWorkers() ? QStringLiteral("正在停止所有后台任务...") : QStringLiteral("已停止，自动同步已关闭");
    statusBar()->showMessage(status);
    if (m_shortcuts && m_shortcuts->isDispatching() && m_config.showShortcutFeedback)
        m_syncCompletionToast->present(status, {}, SyncCompletionToast::Information);
}

void MainWindow::addFileType()
{
    const QString type = normalizedExtension(m_newTypeEdit->text());
    if (type.isEmpty()) {
        RoundedMessageBox::warning(this, "提示", "请输入有效的扩展名，如 dll。");
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
        RoundedMessageBox::warning(this, "提示", "请先选择一个扩展名。");
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

    openShareInExplorer(normalizePathForUi(m_config.hosts.at(row).sharePath));
}

void MainWindow::openShareInExplorer(const QString &sharePath)
{
    if (m_isClosing || sharePath.isEmpty()) return;
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

void MainWindow::handleSymbolsVerified(const QString &hostId, const QStringList &files, bool verified, quint64 revision)
{
    if (m_isClosing || m_stoppingHostIds.contains(hostId) || m_syncContextRevisions.value(hostId) != revision) return;
    if (files.isEmpty()) {
        if (verified) m_symbolWarnings.remove(hostId);
        return;
    }
    const auto host = std::find_if(m_config.hosts.cbegin(), m_config.hosts.cend(),
                                   [&](const HostEntry &entry) { return entry.id == hostId; });
    if (host == m_config.hosts.cend() || !host->enabled) return;

    QStringList warningKey = files;
    warningKey.prepend(verified ? "pending" : "unverified");
    if (m_symbolWarnings.value(hostId) == warningKey) return;
    m_symbolWarnings.insert(hostId, warningKey);
    const QString name = host->name.isEmpty() ? host->sharePath : host->name;
    const QString message = verified
        ? QString("主机 %1 有 %2 个计划同步的 PDB 仍缺失或与本地不一致。符号文件被占用或目标共享不可写时，请解除占用或恢复共享后重试。").arg(name).arg(files.size())
        : QString("主机 %1 的远端校验失败，无法确认 %2 个计划同步的 PDB 已更新。请检查共享后重试。").arg(name).arg(files.size());
    showNotice("PDB 同步提醒", message, true, files.join('\n'));
}

bool MainWindow::handleWorkerCopySucceeded(const QString &hostId, const QDateTime &completedUtc, quint64 revision)
{
    if (m_isClosing || !completedUtc.isValid() || m_stoppingHostIds.contains(hostId)) {
        return false;
    }

    collectUiToConfig();
    if (m_syncContextRevisions.value(hostId) != revision) {
        return false;
    }

    for (HostEntry &host : m_config.hosts) {
        if (host.id == hostId) {
            host.lastSyncUtc = completedUtc.toUTC();
            refreshLastSyncTimes();
            QString error;
            if (!ConfigManager::save(m_configPath, m_config, &error)) {
                RoundedMessageBox::warning(this, "同步时间保存失败", QString("文件同步已完成，但同步时间无法写入配置：%1").arg(error));
            }
            return true;
        }
    }
    return false;
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
        startOperations(m_config.autoSync, true);
        return;
    }

    const bool changed = fingerprint != m_lastFingerprint;
    // 本地无新变化且没有任务运行时视为空闲，此时执行已下载好的无感更新。
    if (!changed && installSilentUpdateIfIdle()) {
        return;
    }
    if (changed) {
        m_lastFingerprint = fingerprint;
        if (m_config.autoSync) {
            statusBar()->showMessage("检测到本地文件变化，开始自动同步", 3000);
            startOperations(true, true);
            return;
        }
    }

    startOperations(m_config.autoSync, true);
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
    if (QTableWidgetItem *status = m_hostTable->item(row, 5)) {
        status->setBackground(color);
        status->setText(statusText);
        status->setToolTip(statusText);
    }
    const QString hostId = m_config.hosts.value(row).id;
    m_checkDisplays.remove(hostId);
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
        return QColor("#d8f0e2");
    case SyncWorker::Mismatch: {
        const int intensity = qBound(0, mismatchPercent, 100);
        const int green = 231 - intensity / 3;
        const int blue = 231 - intensity / 4;
        return QColor(255, green, blue);
    }
    case SyncWorker::Disconnected:
    case SyncWorker::Stopped:
        return QColor("#e5eaf1");
    case SyncWorker::Error:
        return QColor("#ffe3c6");
    case SyncWorker::Syncing:
        return QColor("#cee5fc");
    case SyncWorker::Checking:
    case SyncWorker::Stopping:
    default:
        return QColor("#e7f1fc");
    }
}

QString MainWindow::currentLocalFingerprint() const
{
    QStringList selectedTypes;
    for (int index = 0; index < m_typeList->count(); ++index) {
        const QListWidgetItem *item = m_typeList->item(index);
        if (item->checkState() == Qt::Checked) selectedTypes.append(normalizedExtension(item->text()));
    }
    return SyncPlan::fingerprint(m_releaseDirEdit->currentText(), selectedTypes, m_config.syncRules, m_config.excludedFilePatterns);
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
    if (m_isClosing || !m_config.autoSync) {
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

void MainWindow::startOperations(bool performCopy, bool backgroundPoll, bool fromShortcut)
{
    if (m_isClosing || m_backgroundTasksPaused) {
        return;
    }

    collectUiToConfig();
    const bool feedback = fromShortcut && m_config.showShortcutFeedback;
    auto logSkipped = [&](const QString &reason) {
        const QJsonObject result{{"reason", reason}, {"copy", performCopy}};
        if (backgroundPoll) Diagnostics::backgroundState(m_configPath + "/eligibility", {}, result, {{"trigger", "background_poll"}, {"configuration", m_configPath}});
        else Diagnostics::event("sync.skipped", result);
    };
    const QVector<int> rows = targetRows();
    if (rows.isEmpty()) {
        logSkipped("no_target_hosts");
        if (performCopy) {
            statusBar()->showMessage("没有可同步的主机被勾选。", 3000);
            if (feedback) m_syncCompletionToast->present("无法同步", "请先勾选目标主机", SyncCompletionToast::Warning);
        }
        return;
    }

    QStringList selectedTypes = m_config.selectedFileTypes;
    selectedTypes.removeDuplicates();
    if (selectedTypes.isEmpty() && std::none_of(m_config.syncRules.cbegin(), m_config.syncRules.cend(), [](const SyncRule &rule) { return rule.enabled; })) {
        logSkipped("no_types_or_rules");
        if (performCopy) {
            statusBar()->showMessage("请勾选基本同步类型，或启用至少一条附加同步规则。", 3000);
            if (feedback) m_syncCompletionToast->present("无法同步", "请选择文件类型或启用同步规则", SyncCompletionToast::Warning);
        }
        return;
    }

    if (backgroundPoll) Diagnostics::backgroundState(m_configPath + "/eligibility", {}, {{"reason", "ready"}, {"copy", performCopy}}, {{"trigger", "background_poll"}, {"configuration", m_configPath}});
    if (feedback) m_syncCompletionToast->present("已收到手动同步请求", "正在检查文件", SyncCompletionToast::Information);
    const auto round = std::make_shared<SyncRoundFeedback>();
    for (int row : rows) {
        const HostEntry host = m_config.hosts.at(row);
        if (m_runningThreads.contains(host.id)) {
            continue;
        }

        auto *thread = new QThread(this);
        auto *worker = new SyncWorker(host, m_config.releaseDir, selectedTypes, performCopy,
                                      m_config.syncRules, nullptr, m_config.excludedFilePatterns);
        worker->setProperty("diagnosticsSourceClass", sender() ? sender()->metaObject()->className() : "startup_or_direct");
        worker->setProperty("diagnosticsSourceObject", sender() ? sender()->objectName() : QString());
        worker->setProperty("diagnosticsBackground", backgroundPoll);
        worker->setProperty("diagnosticsScope", m_configPath);
        worker->moveToThread(thread);

        const quint64 revision = m_syncContextRevisions.value(host.id);
        const auto previous = m_checkDisplays.constFind(host.id);
        const bool preserveChecking = backgroundPoll && previous != m_checkDisplays.constEnd() && previous->revision == revision;
        ++round->running;
        struct OperationDisplay {
            bool copying = false;
            int pendingPercent = -1;
            QString pendingDetail;
            int finalState = SyncWorker::Checking;
            QString finalStatus;
            bool copied = false;
        };
        const auto display = std::make_shared<OperationDisplay>();
        connect(thread, &QThread::started, worker, &SyncWorker::process);
        connect(worker, &SyncWorker::progressChanged, this, [this, revision, preserveChecking, display](const QString &hostId, int percent, const QString &detail) {
            if (m_syncContextRevisions.value(hostId) != revision) return;
            if (preserveChecking && !display->copying) { display->pendingPercent = percent; display->pendingDetail = detail; return; }
            handleWorkerProgress(hostId, percent, detail);
        });
        connect(worker, &SyncWorker::recentFilesChanged, this, [this](const QString &hostId, const QStringList &files, const QString &title) {
            if (!m_isClosing) {
                m_copyPreview->setRecentFiles(hostId, files, title);
            }
        });
        connect(worker, &SyncWorker::stateChanged, this, [this, revision, backgroundPoll, preserveChecking, display](const QString &hostId,
            int state, int mismatch, const QString &status, const QString &detail, const QString &tooltip, const QByteArray &signature) {
            if (m_syncContextRevisions.value(hostId) != revision || m_stoppingHostIds.contains(hostId)) return;
            // 后台检查保留上一轮最终显示；真正复制的状态先于进度到达，之后正常显示复制过程。
            if (state == SyncWorker::Syncing) display->copying = true;
            if (preserveChecking && state == SyncWorker::Checking) return;
            const bool terminal = state != SyncWorker::Checking && state != SyncWorker::Syncing && state != SyncWorker::Stopping;
            if (terminal) {
                display->finalState = state;
                display->finalStatus = status;
            }
            const auto previous = m_checkDisplays.constFind(hostId);
            if (terminal && backgroundPoll && !display->copying && previous != m_checkDisplays.constEnd()
                && previous->revision == revision && previous->signature == signature) return;
            // 先等最终结果决定是否更新，恢复一致时仍须应用被暂存的“无需复制”完成进度。
            if (terminal && display->pendingPercent >= 0) handleWorkerProgress(hostId, display->pendingPercent, display->pendingDetail);
            handleWorkerState(hostId, state, mismatch, status, detail, tooltip);
            if (terminal) m_checkDisplays.insert(hostId, {revision, signature});
        });
        connect(worker, &SyncWorker::symbolsVerified, this, [this, revision](const QString &hostId, const QStringList &files, bool verified) {
            handleSymbolsVerified(hostId, files, verified, revision);
        });
        connect(worker, &SyncWorker::copySucceeded, this, [this, revision, display](const QString &hostId, const QDateTime &completedUtc) {
            display->copied = handleWorkerCopySucceeded(hostId, completedUtc, revision);
        });
        connect(worker, &SyncWorker::finished, this, &MainWindow::handleWorkerFinished);
        connect(worker, &SyncWorker::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        connect(thread, &QThread::finished, this, [this, host, revision, display, round, feedback]() {
            // 同一轮的工作线程可能以任意顺序退出，最后一个负责发布本轮的实际结果。
            if (m_syncContextRevisions.value(host.id) == revision && !m_stoppingHostIds.contains(host.id)) {
                const auto name = hostDisplayText(host);
                if (display->copied) round->copied.append(name);
                else if (display->finalState == SyncWorker::InSync) ++round->upToDate;
                else if (!display->finalStatus.isEmpty())
                    round->failures.append(QStringLiteral("%1（%2）").arg(name, display->finalStatus));
            }
            if (--round->running == 0 && !m_isClosing && !m_backgroundTasksPaused)
                round->showResult(*m_syncCompletionToast, feedback && m_config.showShortcutFeedback);
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
                    const bool fromShortcut = m_pendingShortcut;
                    m_pendingShortcut = false;
                    startOperations(true, false, fromShortcut);
                    return;
                }

                statusBar()->showMessage(m_backgroundTasksPaused ? "已停止，自动同步已关闭" : "所有后台任务已结束", 3000);
            }
        });

        m_runningThreads.insert(host.id, thread);
        m_cancellationFlags.insert(host.id, worker->cancellationFlag());
        if (!preserveChecking) updateHostRow(row, SyncWorker::Checking, 0,
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
        m_pollTimer->setInterval(qMax(1, m_config.scanIntervalSeconds) * 1000);
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
