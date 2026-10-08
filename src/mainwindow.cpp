#pragma execution_character_set("utf-8")
#include "mainwindow.h"

#include "configmanager.h"
#include "syncworker.h"

#include <algorithm>
#include <QAbstractItemView>
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
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QPoint>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSslSocket>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QVersionNumber>
#include <QUuid>
#include <qdatetime.h>

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
static const char *kUpdateInfoUrl = "https://gitee.com/letmeseesee/remote-copying/raw/master/update.json";
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
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_configPath(ConfigManager::defaultConfigPath())
{
    setupUi();
    loadConfiguration();

    m_pollTimer = new QTimer(this);
    connect(m_pollTimer, &QTimer::timeout, this, &MainWindow::pollForChanges);
    updateTimerInterval();
    m_pollTimer->start();

    statusBar()->showMessage(QString("配置文件：%1").arg(QDir::toNativeSeparators(m_configPath)), 5000);
    QTimer::singleShot(300, this, [this]() {
        startOperations(m_autoSyncCheck->isChecked());
    });

    // ===== 系统托盘 =====
    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(style()->standardIcon(QStyle::SP_ComputerIcon));
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
}

MainWindow::~MainWindow()
{
    m_isClosing = true;
    requestWorkersToStop();

    for (auto it = m_runningThreads.constBegin(); it != m_runningThreads.constEnd(); ++it) {
        QThread *thread = it.value();
        if (thread != nullptr && thread->isRunning()) {
            thread->quit();
            thread->wait();
        }
    }
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

void MainWindow::requestWorkersToStop()
{
    for (auto it = m_runningWorkers.constBegin(); it != m_runningWorkers.constEnd(); ++it) {
        SyncWorker *worker = it.value();
        if (worker != nullptr) {
            worker->cancel();
        }
    }

    for (auto it = m_runningThreads.constBegin(); it != m_runningThreads.constEnd(); ++it) {
        QThread *thread = it.value();
        if (thread != nullptr) {
            thread->quit();
        }
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
    setWindowTitle(QString("远程 Release 同步工具 v%1").arg(QCoreApplication::applicationVersion()));
    resize(1280, 820);

    auto *central = new QWidget(this);
    auto *rootLayout = new QVBoxLayout(central);
    rootLayout->setSpacing(12);

    auto *localGroup = new QGroupBox("本地目录与同步选项", central);
    auto *localLayout = new QVBoxLayout(localGroup);

    auto *dirLayout = new QHBoxLayout();
    auto *dirLabel = new QLabel("Release 目录：", localGroup);
    m_releaseDirEdit = new QComboBox(localGroup);
    m_releaseDirEdit->setEditable(true);
    m_releaseDirEdit->setInsertPolicy(QComboBox::NoInsert);
    m_releaseDirEdit->setMinimumWidth(400);
    m_releaseDirEdit->lineEdit()->setPlaceholderText("选择或输入本地 Release 目录路径...");
    auto *browseButton = new QPushButton("浏览...", localGroup);
    dirLayout->addWidget(dirLabel);
    dirLayout->addWidget(m_releaseDirEdit, 1);
    dirLayout->addWidget(browseButton);
    localLayout->addLayout(dirLayout);

    auto *typeRow = new QHBoxLayout();
    auto *typeLabel = new QLabel("同步类型：", localGroup);
    m_typeList = new QListWidget(localGroup);
    m_typeList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_typeList->setMaximumHeight(110);

    auto *typeButtonColumn = new QVBoxLayout();
    m_newTypeEdit = new QLineEdit(localGroup);
    m_newTypeEdit->setPlaceholderText("输入扩展名如 dll");
    auto *addTypeButton = new QPushButton("添加类型", localGroup);
    auto *removeTypeButton = new QPushButton("删除类型", localGroup);
    typeButtonColumn->addWidget(m_newTypeEdit);
    typeButtonColumn->addWidget(addTypeButton);
    typeButtonColumn->addWidget(removeTypeButton);
    typeButtonColumn->addStretch();

    typeRow->addWidget(typeLabel);
    typeRow->addWidget(m_typeList, 1);
    typeRow->addLayout(typeButtonColumn);
    localLayout->addLayout(typeRow);

    auto *optionRow = new QHBoxLayout();
    m_autoSyncCheck = new QCheckBox("检测到本地文件变化后自动同步", localGroup);
    m_startWithSystemCheck = new QCheckBox("开机自动启动", localGroup);
    m_intervalSpin = new QSpinBox(localGroup);
    m_intervalSpin->setRange(1, 3600);
    m_intervalSpin->setSuffix(" 秒");
    auto *saveConfigButton = new QPushButton("保存配置", localGroup);
    auto *refreshButton = new QPushButton("刷新状态", localGroup);
    auto *syncButton = new QPushButton("手动同步", localGroup);
    optionRow->addWidget(m_autoSyncCheck);
    optionRow->addWidget(m_startWithSystemCheck);
    optionRow->addWidget(new QLabel("扫描间隔", localGroup));
    optionRow->addWidget(m_intervalSpin);
    optionRow->addStretch();
    optionRow->addWidget(saveConfigButton);
    optionRow->addWidget(refreshButton);
    optionRow->addWidget(syncButton);
    localLayout->addLayout(optionRow);

    auto *hostEditorGroup = new QGroupBox("远程主机编辑", central);
    auto *hostEditorLayout = new QVBoxLayout(hostEditorGroup);
    auto *hostForm = new QFormLayout();
    m_hostEnabledCheck = new QCheckBox("勾选即启用同步", hostEditorGroup);
    m_hostNameEdit = new QLineEdit(hostEditorGroup);
    m_hostPathEdit = new QLineEdit(hostEditorGroup);
    m_hostUserEdit = new QLineEdit(hostEditorGroup);
    m_hostPasswordEdit = new QLineEdit(hostEditorGroup);
    m_hostPasswordEdit->setEchoMode(QLineEdit::Password);
    m_hostNameEdit->setPlaceholderText("可选，不填时默认显示主机名");
    m_hostPathEdit->setPlaceholderText("\\\\DESKTOP-ECJ97U1\\share\\LHJ\\Release");
    m_hostUserEdit->setPlaceholderText("Administrator");
    m_hostPasswordEdit->setPlaceholderText("123");
    hostForm->addRow("启用：", m_hostEnabledCheck);
    hostForm->addRow("显示名称：", m_hostNameEdit);
    hostForm->addRow("共享目录：", m_hostPathEdit);
    hostForm->addRow("账号：", m_hostUserEdit);
    hostForm->addRow("密码：", m_hostPasswordEdit);
    hostEditorLayout->addLayout(hostForm);

    auto *hostButtonRow = new QHBoxLayout();
    auto *addHostButton = new QPushButton("添加主机", hostEditorGroup);
    auto *updateHostButton = new QPushButton("更新选中主机", hostEditorGroup);
    auto *removeHostButton = new QPushButton("删除选中主机", hostEditorGroup);
    auto *clearEditorButton = new QPushButton("清空编辑", hostEditorGroup);
    hostButtonRow->addWidget(addHostButton);
    hostButtonRow->addWidget(updateHostButton);
    hostButtonRow->addWidget(removeHostButton);
    hostButtonRow->addWidget(clearEditorButton);
    hostButtonRow->addStretch();
    hostEditorLayout->addLayout(hostButtonRow);

    auto *tableGroup = new QGroupBox("远程主机状态", central);
    auto *tableLayout = new QVBoxLayout(tableGroup);
    m_hostTable = new QTableWidget(tableGroup);
    m_hostTable->setColumnCount(7);
    m_hostTable->setHorizontalHeaderLabels(QStringList()
                                           << "启用"
                                           << "名称"
                                           << "共享目录"
                                           << "账号"
                                           << "进度"
                                           << "状态"
                                           << "详情");
    m_hostTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_hostTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_hostTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_hostTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_hostTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    m_hostTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    m_hostTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    m_hostTable->verticalHeader()->setVisible(false);
    m_hostTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_hostTable->setSelectionMode(QAbstractItemView::SingleSelection);
    m_hostTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_hostTable->setContextMenuPolicy(Qt::CustomContextMenu);
    tableLayout->addWidget(m_hostTable);

    rootLayout->addWidget(localGroup);
    rootLayout->addWidget(hostEditorGroup);
    rootLayout->addWidget(tableGroup, 1);

    setCentralWidget(central);

    m_checkUpdatesButton = new QPushButton("检查更新", this);
    statusBar()->addPermanentWidget(m_checkUpdatesButton);
    connect(m_checkUpdatesButton, &QPushButton::clicked, this, &MainWindow::checkForUpdates);

    connect(browseButton, &QPushButton::clicked, this, &MainWindow::browseReleaseDir);
    connect(addTypeButton, &QPushButton::clicked, this, &MainWindow::addFileType);
    connect(removeTypeButton, &QPushButton::clicked, this, &MainWindow::removeSelectedFileType);
    connect(m_typeList, &QListWidget::itemChanged, this, &MainWindow::handleTypeItemChanged);
    connect(saveConfigButton, &QPushButton::clicked, this, &MainWindow::saveConfiguration);
    connect(refreshButton, &QPushButton::clicked, this, [this]() { startOperations(false); });
    connect(syncButton, &QPushButton::clicked, this, &MainWindow::syncNow);
    connect(addHostButton, &QPushButton::clicked, this, [this]() { addOrUpdateHost(false); });
    connect(updateHostButton, &QPushButton::clicked, this, [this]() { addOrUpdateHost(true); });
    connect(removeHostButton, &QPushButton::clicked, this, &MainWindow::removeSelectedHost);
    connect(clearEditorButton, &QPushButton::clicked, this, &MainWindow::clearHostEditor);
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
    connect(m_autoSyncCheck, &QCheckBox::toggled, this, [this](bool) {
        saveConfiguration();
    });
    connect(m_startWithSystemCheck, &QCheckBox::toggled, this, [this](bool) {
        saveConfiguration();
    });
    connect(m_releaseDirEdit->lineEdit(), &QLineEdit::editingFinished, this, &MainWindow::onReleaseDirEditFinished);
    connect(m_releaseDirEdit, QOverload<int>::of(&QComboBox::activated), this, &MainWindow::onReleaseDirEditFinished);
}

void MainWindow::checkForUpdates()
{
    if (m_isClosing || m_checkUpdatesButton == nullptr || !m_checkUpdatesButton->isEnabled()) {
        return;
    }
    if (!QSslSocket::supportsSsl()) {
        QMessageBox::warning(this, "检查更新失败", "当前发布包缺少 HTTPS 支持，请补齐与 Qt 配套的 TLS 运行库。");
        return;
    }

    if (m_updateNetworkManager == nullptr) {
        m_updateNetworkManager = new QNetworkAccessManager(this);
    }
    QNetworkRequest request(QUrl(QString::fromLatin1(kUpdateInfoUrl)));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("User-Agent", QString("RemoteReleaseSync/%1").arg(QCoreApplication::applicationVersion()).toUtf8());
    QNetworkReply *reply = m_updateNetworkManager->get(request);
    m_checkUpdatesButton->setEnabled(false);
    m_checkUpdatesButton->setText("正在检查...");

    // Qt 5.12 没有请求超时接口，用单次定时器中止迟迟未完成的请求。
    QTimer *timeout = new QTimer(reply);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, [this, reply, timeout]() {
        timeout->stop();
        reply->deleteLater();
        m_checkUpdatesButton->setEnabled(true);
        m_checkUpdatesButton->setText("检查更新");
        if (m_isClosing) {
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            const QString reason = reply->error() == QNetworkReply::OperationCanceledError
                ? QString("检查超时，请稍后重试。")
                : QString("无法获取更新信息：%1").arg(reply->errorString());
            QMessageBox::warning(this, "检查更新失败", reason);
            return;
        }
        const int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            QMessageBox::warning(this, "检查更新失败", QString("更新服务器返回 HTTP %1。").arg(statusCode));
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(reply->readAll(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            QMessageBox::warning(this, "检查更新失败", "更新信息不是有效的 JSON 对象。");
            return;
        }
        const QJsonObject info = document.object();
        if (!info.value("version").isString() || !info.value("notes").isString()
            || !info.value("downloadUrl").isString()) {
            QMessageBox::warning(this, "检查更新失败", "更新信息缺少有效的 version、notes 或 downloadUrl 字段。");
            return;
        }

        const QString latestVersionText = info.value("version").toString().trimmed();
        const QString currentVersionText = QCoreApplication::applicationVersion();
        const QRegularExpression versionPattern("\\A[0-9]+\\.[0-9]+\\.[0-9]+\\z");
        const QVersionNumber latestVersion = QVersionNumber::fromString(latestVersionText);
        const QVersionNumber currentVersion = QVersionNumber::fromString(currentVersionText);
        if (!versionPattern.match(latestVersionText).hasMatch() || latestVersion.segmentCount() != 3
            || !versionPattern.match(currentVersionText).hasMatch() || currentVersion.segmentCount() != 3) {
            QMessageBox::warning(this, "检查更新失败", "版本号必须为三段数字，例如 1.0.0。");
            return;
        }
        const QUrl downloadUrl(info.value("downloadUrl").toString().trimmed(), QUrl::StrictMode);
        if (!downloadUrl.isValid() || downloadUrl.scheme() != "https" || downloadUrl.host().isEmpty()) {
            QMessageBox::warning(this, "检查更新失败", "更新信息中的下载地址必须为有效的 HTTPS 地址。");
            return;
        }
        if (QVersionNumber::compare(latestVersion, currentVersion) <= 0) {
            QMessageBox::information(this, "检查更新", QString("当前已是最新版本（%1）。").arg(currentVersionText));
            return;
        }

        QMessageBox updateDialog(this);
        updateDialog.setWindowTitle("发现新版本");
        updateDialog.setIcon(QMessageBox::Information);
        updateDialog.setTextFormat(Qt::PlainText);
        updateDialog.setText(QString("当前版本：%1\n最新版本：%2").arg(currentVersionText, latestVersionText));
        const QString notes = info.value("notes").toString().trimmed();
        updateDialog.setInformativeText(notes.isEmpty() ? QString("暂无更新说明。") : notes);
        QPushButton *downloadButton = updateDialog.addButton("前往下载", QMessageBox::AcceptRole);
        updateDialog.addButton("稍后", QMessageBox::RejectRole);
        updateDialog.exec();
        if (updateDialog.clickedButton() == downloadButton && !QDesktopServices::openUrl(downloadUrl)) {
            QMessageBox::warning(this, "打开下载页面失败", QString("请在浏览器中访问：\n%1").arg(downloadUrl.toString()));
        }
    });
    timeout->start(15000);
}

void MainWindow::onReleaseDirEditFinished()
{
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
    const bool systemAutoStartEnabled = isSystemAutoStartEnabled();
    if (m_config.startWithSystem && !systemAutoStartEnabled) {
        QString error;
        if (!setSystemAutoStartEnabled(true, &error)) {
            m_config.startWithSystem = false;
        }
    } else if (!m_config.startWithSystem && systemAutoStartEnabled) {
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
        QSignalBlocker blocker(m_intervalSpin);
        m_intervalSpin->setValue(qMax(1, m_config.scanIntervalSeconds));
    }

    if (m_config.fileTypes.isEmpty()) {
        m_config.fileTypes = QStringList() << "dll" << "exe" << "pdb";
    }
    if (m_config.selectedFileTypes.isEmpty()) {
        m_config.selectedFileTypes = m_config.fileTypes;
    }

    refreshTypeList();
    refreshHostTable();
    clearHostEditor();
    m_lastFingerprint = currentLocalFingerprint();
}

void MainWindow::collectUiToConfig()
{
    m_config.releaseDir = normalizePathForUi(m_releaseDirEdit->currentText());
    m_config.autoSync = m_autoSyncCheck->isChecked();
    m_config.startWithSystem = m_startWithSystemCheck->isChecked();
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

        auto *progressBar = new QProgressBar(m_hostTable);
        progressBar->setRange(0, 100);
        progressBar->setValue(0);
        progressBar->setFormat("%p%");
        m_hostTable->setCellWidget(row, 4, progressBar);

        RowWidgets widgets;
        widgets.progressBar = progressBar;
        m_rowWidgets.insert(host.id, widgets);

        updateHostRow(row, SyncWorker::Checking, 0, "未检测", "等待状态刷新", "等待状态刷新", 0);
    }

    m_isRefreshingTable = false;
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
        host.id = m_config.hosts.at(row).id;
        m_config.hosts[row] = host;
    } else {
        host.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (host.name.isEmpty()) {
            host.name = defaultHostNameFromPath(host.sharePath);
        }
        m_config.hosts.push_back(host);
    }

    refreshHostTable();
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
    if (!setSystemAutoStartEnabled(m_config.startWithSystem, &error)) {
        QMessageBox::warning(this, "自启设置失败", QString("开机自启设置写入失败：%1").arg(error));
        return;
    }

    statusBar()->showMessage("配置已保存", 3000);
}

void MainWindow::syncNow()
{
    m_lastFingerprint = currentLocalFingerprint();
    if (hasRunningWorkers()) {
        m_pendingSyncAfterCurrentRun = true;
        statusBar()->showMessage("当前正在检查/同步，完成后会自动执行一次同步", 3000);
        return;
    }

    startOperations(true);
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
    if (m_isRefreshingTable || item == nullptr || item->column() != 0) {
        return;
    }

    const int row = item->row();
    if (row < 0 || row >= m_config.hosts.size()) {
        return;
    }

    m_config.hosts[row].enabled = item->checkState() == Qt::Checked;
    saveConfiguration();
    if (m_config.hosts[row].enabled) {
        requestAutoSync(QString("已勾选主机 [%1]，开始自动同步").arg(hostDisplayText(m_config.hosts[row])));
    }
}

void MainWindow::handleHostTableCellClicked(int row, int column)
{
    if (column != 2 || row < 0 || row >= m_config.hosts.size()) {
        return;
    }
    if ((QApplication::keyboardModifiers() & Qt::ControlModifier) == 0) {
        return;
    }

    const QString sharePath = normalizePathForUi(m_config.hosts.at(row).sharePath);
    if (sharePath.isEmpty()) {
        return;
    }

    if (!QProcess::startDetached("explorer.exe", QStringList() << sharePath)) {
        statusBar()->showMessage(QString("打开共享目录失败：%1").arg(sharePath), 3000);
        return;
    }

    statusBar()->showMessage(QString("已打开共享目录：%1").arg(sharePath), 3000);
}

void MainWindow::handleHostTableCellDoubleClicked(int row, int column)
{
    if (column != 1 && column != 2) {
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
    RowWidgets widgets = m_rowWidgets.value(hostId);
    if (widgets.progressBar != nullptr) {
        widgets.progressBar->setValue(percent);
    }

    for (int row = 0; row < m_config.hosts.size(); ++row) {
        if (m_config.hosts.at(row).id == hostId) {
            if (m_hostTable->item(row, 6) != nullptr) {
                m_hostTable->item(row, 6)->setText(detail);
                m_hostTable->item(row, 6)->setToolTip(detail);
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
    for (int row = 0; row < m_config.hosts.size(); ++row) {
        if (m_config.hosts.at(row).id == hostId) {
            RowWidgets widgets = m_rowWidgets.value(hostId);
            const int currentProgress = widgets.progressBar != nullptr ? widgets.progressBar->value() : 0;
            updateHostRow(row, state, mismatchPercent, statusText, detail, detailTooltip, currentProgress);
            break;
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

    const QString fingerprint = currentLocalFingerprint();
    if (m_lastFingerprint.isEmpty()) {
        m_lastFingerprint = fingerprint;
        startOperations(m_autoSyncCheck->isChecked());
        return;
    }

    const bool changed = fingerprint != m_lastFingerprint;
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
    if (m_hostTable->item(row, 6) != nullptr) {
        m_hostTable->item(row, 6)->setText(detail);
        m_hostTable->item(row, 6)->setToolTip(detailTooltip.isEmpty() ? detail : detailTooltip);
    }

    const QString hostId = m_config.hosts.value(row).id;
    RowWidgets widgets = m_rowWidgets.value(hostId);
    if (widgets.progressBar != nullptr) {
        widgets.progressBar->setValue(progressPercent);
        widgets.progressBar->setStyleSheet(QString("QProgressBar { text-align: center; } QProgressBar::chunk { background-color: %1; }")
                                               .arg(color.darker(110).name()));
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
        return QColor(180, 180, 180);
    case SyncWorker::Error:
        return QColor(255, 199, 140);
    case SyncWorker::Syncing:
        return QColor(189, 215, 238);
    case SyncWorker::Checking:
    default:
        return QColor(221, 235, 247);
    }
}

QString MainWindow::currentLocalFingerprint() const
{
    const QString rootPath = normalizePathForUi(m_releaseDirEdit->currentText());
    if (rootPath.isEmpty() || !QFileInfo::exists(rootPath)) {
        return QString();
    }

    QSet<QString> selectedTypes;
    for (int index = 0; index < m_typeList->count(); ++index) {
        QListWidgetItem *item = m_typeList->item(index);
        if (item->checkState() == Qt::Checked) {
            selectedTypes.insert(normalizedExtension(item->text()));
        }
    }

    if (selectedTypes.isEmpty()) {
        return QString();
    }

    QStringList fingerprints;
    QDirIterator iterator(rootPath, QDir::Files, QDirIterator::Subdirectories);
    const QDir baseDir(rootPath);

    while (iterator.hasNext()) {
        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (!selectedTypes.contains(info.suffix().toLower())) {
            continue;
        }

        const QString relativePath = QDir::cleanPath(baseDir.relativeFilePath(info.absoluteFilePath())).replace('\\', '/');
        fingerprints.append(QString("%1|%2|%3")
                                .arg(relativePath)
                                .arg(info.size())
                                .arg(info.lastModified().toMSecsSinceEpoch()));
    }

    std::sort(fingerprints.begin(), fingerprints.end());
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const QString &fingerprint : fingerprints) {
        hash.addData(fingerprint.toUtf8());
    }

    return QString::fromLatin1(hash.result().toHex());
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
    if (!m_autoSyncCheck->isChecked()) {
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
    if (m_isClosing) {
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
    if (selectedTypes.isEmpty()) {
        if (performCopy) {
            statusBar()->showMessage("请先勾选至少一个同步类型。", 3000);
        }
        return;
    }

    for (int row : rows) {
        const HostEntry host = m_config.hosts.at(row);
        if (m_runningThreads.contains(host.id)) {
            continue;
        }

        auto *thread = new QThread(this);
        auto *worker = new SyncWorker(host, m_config.releaseDir, selectedTypes, performCopy);
        worker->moveToThread(thread);

        connect(thread, &QThread::started, worker, &SyncWorker::process);
        connect(worker, &SyncWorker::progressChanged, this, &MainWindow::handleWorkerProgress);
        connect(worker, &SyncWorker::stateChanged, this, &MainWindow::handleWorkerState);
        connect(worker, &SyncWorker::finished, this, &MainWindow::handleWorkerFinished);
        connect(worker, &SyncWorker::finished, worker, &QObject::deleteLater);
        connect(thread, &QThread::finished, thread, &QObject::deleteLater);
        connect(thread, &QThread::finished, this, [this, host]() {
            m_runningThreads.remove(host.id);
            m_runningWorkers.remove(host.id);
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

                statusBar()->showMessage("所有后台任务已完成", 3000);
            }
        });

        m_runningThreads.insert(host.id, thread);
        m_runningWorkers.insert(host.id, worker);
        updateHostRow(row, performCopy ? SyncWorker::Syncing : SyncWorker::Checking, 0,
                      performCopy ? "同步中" : "检测中",
                      performCopy ? "准备同步..." : "准备检测...",
                      performCopy ? "准备同步..." : "准备检测...",
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
