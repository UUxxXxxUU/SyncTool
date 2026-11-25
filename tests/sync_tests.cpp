#pragma execution_character_set("utf-8")
#include "configmanager.h"
#include "appearance.h"
#include "diagnostics.h"
#include "downloadprogress.h"
#include "mainwindow.h"
#include "remotedebugger.h"
#include "shortcutmanager.h"
#include "networkhostdialog.h"
#include "syncplan.h"
#include "syncworker.h"
#include "synccompletiontoast.h"

#include <QtTest>
#include <QAbstractItemView>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QInputMethodEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QRegularExpression>
#include <QProcess>
#include <QSettings>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QTabBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QVBoxLayout>
#include <QTextBrowser>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <memory>
#include <functional>
#include <thread>
#include <algorithm>
#include <limits>
#include <vector>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <imm.h>
#include <shellapi.h>
#endif

namespace
{
#ifdef Q_OS_WIN
class NativeRoundedFrame : public RoundedMainWindow
{
public:
    using RoundedMainWindow::nativeEvent;
};
#endif

class PendingDownloadReply : public QNetworkReply
{
public:
    explicit PendingDownloadReply(QObject *parent) : QNetworkReply(parent) {}
    void abort() override { close(); }
protected:
    qint64 readData(char *, qint64) override { return -1; }
};

bool writeFile(const QString &path, const QByteArray &data = "payload")
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
struct Run
{
    int state = -1;
    QString detail;
    QString tooltip;
    int success = 0;
    int finished = 0;
    QStringList copied;
    QVector<int> progress;
};
Run runWorker(const QString &source, const QString &target, const QStringList &types,
              const QVector<SyncRule> &rules, bool copy = true)
{
    HostEntry host;
    host.id = "qa-host";
    host.sharePath = target;
    SyncWorker worker(host, source, types, copy, rules);
    Run run;
    QObject::connect(&worker, &SyncWorker::stateChanged, [&](const QString &, int state, int, const QString &, const QString &detail, const QString &tooltip) {
        run.state = state; run.detail = detail; run.tooltip = tooltip;
    });
    QObject::connect(&worker, &SyncWorker::copySucceeded, [&](const QString &, const QDateTime &) { ++run.success; });
    QObject::connect(&worker, &SyncWorker::finished, [&](const QString &) { ++run.finished; });
    QObject::connect(&worker, &SyncWorker::recentFilesChanged, [&](const QString &, const QStringList &files, const QString &) { run.copied = files; });
    QObject::connect(&worker, &SyncWorker::progressChanged, [&](const QString &, int progress, const QString &) { run.progress.append(progress); });
    worker.process();
    return run;
}

class ToastEvents : public QObject
{
public:
    QStringList messages;
    QVector<qint64> shownAt;
    QElapsedTimer clock;
    explicit ToastEvents(SyncCompletionToast *toast)
    {
        clock.start();
        connect(toast, &SyncCompletionToast::presented, this, [this](const QString &title, const QString &detail) {
            messages.append(detail.isEmpty() ? title : detail + " " + title);
            shownAt.append(clock.elapsed());
        });
    }
};

SyncCompletionToast *completionToast()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *toast = qobject_cast<SyncCompletionToast *>(widget)) return toast;
    }
    return nullptr;
}

}

class SyncTests : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> m_temp;
    static bool workersAreIdle(const MainWindow &window) { return !window.hasRunningWorkers(); }
    QString root() const { return m_temp->path(); }
    QString source() const { return root() + "/本地 source/a/Release"; }
    QString target() const { return root() + "/远端 target/b/Release"; }
    QString configPath() const { return root() + "/qa-config.json"; }
    QByteArray diagnosticLog() const
    {
        QByteArray result;
        const QDir directory(Diagnostics::directory() + "/Logs");
        for (const QString &name : directory.entryList({"*.log"}, QDir::Files)) result += readFile(directory.filePath(name));
        return result;
    }
    QString m_originalStartup;
    QUrl m_openedDirectory;
    Q_INVOKABLE void recordOpenedDirectory(const QUrl &url) { m_openedDirectory = url; }
    bool m_hadStartup = false;
    const QString startupKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    void stopTimers(MainWindow &window)
    {
        for (QTimer *timer : window.findChildren<QTimer *>()) timer->stop();
    }
    std::unique_ptr<MainWindow> window(const QVector<SyncRule> &rules = {}, bool autoSync = false)
    {
        AppConfig config;
        config.releaseDir = source();
        config.fileTypes = QStringList{"exe", "dll", "pdb"};
        config.selectedFileTypes = QStringList{"exe"};
        config.autoSync = autoSync;
        config.syncRules = rules;
        HostEntry host;
        host.id = "qa";
        host.name = "测试主机";
        host.sharePath = target();
        config.hosts.append(host);
        QString error;
        if (!ConfigManager::save(configPath(), config, &error)) qFatal("Cannot save test config");
        auto result = std::make_unique<MainWindow>(nullptr, configPath(), false);
        stopTimers(*result);
        return result;
    }
private slots:
    void initTestCase()
    {
        if (QGuiApplication::platformName() == "offscreen") {
            QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
            QApplication::setFont(QFont("Microsoft YaHei", 10));
        }
        QApplication::setWindowIcon(QIcon(":/icons/app_32.png"));
        QApplication::setApplicationVersion("1.1.8");
        QSettings settings(startupKey, QSettings::NativeFormat);
        m_hadStartup = settings.contains("RemoteReleaseSync");
        m_originalStartup = settings.value("RemoteReleaseSync").toString();
    }
    void init()
    {
        m_temp = std::make_unique<QTemporaryDir>(QDir::currentPath() + "/sync-qa-XXXXXX");
        QVERIFY(m_temp->isValid());
        QVERIFY(QDir().mkpath(source()));
        QVERIFY(QDir().mkpath(target()));
        if (!qEnvironmentVariableIsEmpty("SYNC_QA_LOG_DIR")
            && QByteArray(QTest::currentTestFunction()) != "detailedInteractionLogsRedactSecretsAndKeepThreadOrder")
            Diagnostics::start(m_temp->path() + "/diagnostics");
    }
    void cleanup()
    {
        Diagnostics::stop();
        const QString logRoot = qEnvironmentVariable("SYNC_QA_LOG_DIR");
        if (!logRoot.isEmpty()) {
            const QString data = QString::fromLatin1(QTest::currentDataTag());
            const QString destination = logRoot + '/' + QString::fromLatin1(QTest::currentTestFunction()) + (data.isEmpty() ? QString() : '/' + data);
            QVERIFY(QDir().mkpath(destination));
            const QDir source(m_temp->path() + "/diagnostics/Logs");
            for (const QString &name : source.entryList({"*.log"}, QDir::Files)) QVERIFY(QFile::copy(source.filePath(name), destination + '/' + name));
        }
        QSettings settings(startupKey, QSettings::NativeFormat);
        QCOMPARE(settings.contains("RemoteReleaseSync"), m_hadStartup);
        QCOMPARE(settings.value("RemoteReleaseSync").toString(), m_originalStartup);
        m_temp.reset();
    }
    void relativePathsAndFilters()
    {
        QString path, error;
        QVERIFY(SyncPlan::normalizeRelativePath(".\\image", &path, &error));
        QCOMPARE(path, QString("image"));
        QCOMPARE(SyncPlan::resolvePath("D:/test sync/a/Release", "../Lang"), QString("D:/test sync/a/Lang"));
        QCOMPARE(SyncPlan::resolvePath("//SERVER/b/Release", "../Lang"), QString("//SERVER/b/Lang"));
        for (const QString &invalid : {QString("D:/absolute"), QString("//server/share"), QString("/root"), QString("./*.png"), QString("./NUL.txt"), QString("./bad./x")}) {
            QVERIFY2(!SyncPlan::normalizeRelativePath(invalid, &path, &error), qPrintable(invalid));
        }
        QStringList types;
        QVERIFY(SyncPlan::normalizeTypes({".PNG", "*.qm", "png"}, &types, &error));
        QCOMPARE(types, QStringList({"png", "qm"}));
        QVERIFY(!SyncPlan::normalizeTypes({"*"}, &types, &error));
    }
    void releaseHistoryRemovalPreservesCurrentDirectory()
    {
        auto main = window();
        AppConfig config = ConfigManager::load(configPath());
        const QString current = QDir::toNativeSeparators(source());
        const QString old = QDir::toNativeSeparators(root() + "/old Release");
        config.releaseDir = current;
        config.releaseDirHistory = QStringList{current, old, QDir::fromNativeSeparators(old).toUpper()};
        QString error;
        QVERIFY2(ConfigManager::save(configPath(), config, &error), qPrintable(error));
        main->loadConfiguration();
        main->show();
        QVERIFY(QTest::qWaitForWindowExposed(main.get()));
        auto *combo = main->m_releaseDirEdit;
        auto *history = combo->view();
        QCOMPARE(combo->count(), 3);

        combo->showPopup();
        const QRect oldRow = history->visualRect(combo->model()->index(1, 0));
        QVERIFY(oldRow.isValid());
        bool menuShown = false;
        QTimer chooseRemoval;
        connect(&chooseRemoval, &QTimer::timeout, main.get(), [&]() {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            chooseRemoval.stop();
            QTimer::singleShot(2000, menu, &QMenu::close);
            if (menu->actions().size() != 1 || menu->actions().first()->text() != "移除这条历史记录") {
                menu->close();
                return;
            }
            menuShown = true;
            menu->setActiveAction(menu->actions().first());
            QTest::keyClick(menu, Qt::Key_Return);
        });
        chooseRemoval.start(10);
        QTest::mouseClick(history->viewport(), Qt::RightButton, Qt::NoModifier, oldRow.center());
        chooseRemoval.stop();
        QVERIFY(menuShown);
        QCOMPARE(combo->count(), 1);
        QCOMPARE(combo->currentText(), current);
        QCOMPARE(ConfigManager::load(configPath()).releaseDirHistory, QStringList{current});

        combo->showPopup();
        history->setCurrentIndex(combo->model()->index(0, 0));
        QTest::keyClick(history, Qt::Key_Delete);
        QCOMPARE(combo->count(), 0);
        QCOMPARE(combo->currentText(), current);
        QVERIFY(QMetaObject::invokeMethod(combo->lineEdit(), "editingFinished", Qt::DirectConnection));
        main->loadConfiguration();
        QCOMPARE(combo->count(), 0);
        QCOMPARE(combo->currentText(), current);
        QCOMPARE(ConfigManager::load(configPath()).releaseDir, current);

        QVERIFY(QMetaObject::invokeMethod(combo->lineEdit(), "returnPressed", Qt::DirectConnection));
        QCOMPARE(combo->count(), 1);
        QCOMPARE(ConfigManager::load(configPath()).releaseDirHistory, QStringList{current});
    }

    void oldConfigurationAndRulesRoundTrip()
    {
        const QByteArray legacy = "{\"releaseDir\":\"D:/legacy/Release\",\"fileTypes\":[\"exe\",\"dll\"],"
                                  "\"hosts\":[{\"id\":\"old\",\"sharePath\":\"//SERVER/b/Release\"}]}";
        QVERIFY(writeFile(configPath(), legacy));
        AppConfig config = ConfigManager::load(configPath());
        QCOMPARE(config.releaseDir, QString("D:/legacy/Release"));
        QCOMPARE(config.selectedFileTypes, QStringList({"exe", "dll"}));
        QVERIFY(config.syncRules.isEmpty());
        config.selectedFileTypes.clear();
        config.syncRules = {{"../Lang", {"qm"}, true}, {"./image", {}, false}};
        QString error;
        QVERIFY(ConfigManager::save(configPath(), config, &error));
        const AppConfig reloaded = ConfigManager::load(configPath());
        QVERIFY(reloaded.selectedFileTypes.isEmpty());
        QVERIFY(reloaded.syncRules == config.syncRules);
        QCOMPARE(reloaded.hosts.first().sharePath, config.hosts.first().sharePath);
    }
    void filteredCopyWithSiblingAndNestedRules()
    {
        QVERIFY(writeFile(source() + "/app.exe", "EXE"));
        QVERIFY(writeFile(source() + "/skip.txt"));
        QVERIFY(writeFile(source() + "/image/子目录/图标.PNG", "PNG"));
        QVERIFY(writeFile(source() + "/image/skip.jpg"));
        QVERIFY(writeFile(source() + "/Lang/zh.qm", "QM"));
        QVERIFY(writeFile(source() + "/../resources/theme/no-extension", "ALL"));
        QVERIFY(writeFile(source() + "/../resources/theme/empty", ""));
        const QVector<SyncRule> rules{{"./image", {"png"}}, {"./Lang", {"qm"}}, {"../resources/theme", {}}};
        const Run run = runWorker(source(), target(), {"exe"}, rules);
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(run.finished, 1);
        QCOMPARE(run.success, 1);
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("EXE"));
        QCOMPARE(readFile(target() + "/image/子目录/图标.PNG"), QByteArray("PNG"));
        QCOMPARE(readFile(target() + "/Lang/zh.qm"), QByteArray("QM"));
        QCOMPARE(readFile(target() + "/../resources/theme/no-extension"), QByteArray("ALL"));
        QVERIFY(QFileInfo::exists(target() + "/../resources/theme/empty"));
        QVERIFY(!QFileInfo::exists(target() + "/skip.txt"));
        QVERIFY(!QFileInfo::exists(target() + "/image/skip.jpg"));
        QCOMPARE(run.copied.size(), 5);
        QVERIFY(std::is_sorted(run.progress.cbegin(), run.progress.cend()));
        QCOMPARE(run.progress.last(), 100);
    }
    void rulesOnlyAndOverlappingUnion()
    {
        QVERIFY(writeFile(source() + "/image/a.png", "PNG"));
        QVERIFY(writeFile(source() + "/image/b.qm", "QM"));
        const QVector<SyncRule> rules{{"./image", {"png"}}, {"image", {"png", "qm"}}, {"./missing", {}, false}};
        const Run run = runWorker(source(), target(), {}, rules);
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(run.copied.size(), 2);
        QCOMPARE(readFile(target() + "/image/a.png"), QByteArray("PNG"));
        QCOMPARE(readFile(target() + "/image/b.qm"), QByteArray("QM"));
        const Run repeat = runWorker(source(), target(), {}, rules);
        QCOMPARE(repeat.state, int(SyncWorker::InSync));
        QCOMPARE(repeat.success, 0); // No copy means no new successful-copy timestamp.
    }
    void basicAndAdditionalOverlap()
    {
        QVERIFY(writeFile(source() + "/image/a.png"));
        const Run run = runWorker(source(), target(), {"png"}, {{"./image", {"png"}}});
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(run.copied.size(), 1);
    }
    void checkDoesNotCopyAndExtraFilesRemain()
    {
        QVERIFY(writeFile(source() + "/Lang/zh.qm", "new-language"));
        QVERIFY(writeFile(target() + "/Lang/extra.qm", "retain"));
        QVERIFY(writeFile(target() + "/unrelated/ignored.qm", "unrelated"));
        const QVector<SyncRule> rules{{"./Lang", {"qm"}}};
        const Run check = runWorker(source(), target(), {}, rules, false);
        QCOMPARE(check.state, int(SyncWorker::Mismatch));
        QVERIFY(!QFileInfo::exists(target() + "/Lang/zh.qm"));
        QVERIFY(check.tooltip.contains("extra.qm"));
        QVERIFY(!check.tooltip.contains("ignored.qm"));
        const Run copy = runWorker(source(), target(), {}, rules);
        QVERIFY2(copy.state == SyncWorker::InSync, qPrintable(copy.detail));
        QCOMPARE(readFile(target() + "/Lang/extra.qm"), QByteArray("retain"));
        QCOMPARE(readFile(target() + "/unrelated/ignored.qm"), QByteArray("unrelated"));
        QCOMPARE(runWorker(source(), target(), {}, rules, false).state, int(SyncWorker::Mismatch));
    }
    void updatesAndFileRules()
    {
        QVERIFY(writeFile(source() + "/../config.ini", "a-new-configuration"));
        QVERIFY(writeFile(target() + "/../config.ini", "old"));
        const Run run = runWorker(source(), target(), {}, {{"../config.ini", {}}});
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(readFile(target() + "/../config.ini"), QByteArray("a-new-configuration"));
    }
    void fingerprintCoversAdditionalPaths()
    {
        QVERIFY(writeFile(source() + "/app.exe", "original"));
        QVERIFY(writeFile(source() + "/../Lang/zh.qm", "initial"));
        const QVector<SyncRule> rules{{"../Lang", {"qm"}}};
        const QString first = SyncPlan::fingerprint(source(), {"exe"}, rules);
        QVERIFY(writeFile(source() + "/../Lang/zh.qm", "changed-language"));
        const QString changed = SyncPlan::fingerprint(source(), {"exe"}, rules);
        QVERIFY(first != changed);
        QVERIFY(writeFile(source() + "/../Lang/ignore.txt"));
        QCOMPARE(SyncPlan::fingerprint(source(), {"exe"}, rules), changed);
        QVERIFY(QFile::remove(source() + "/../Lang/zh.qm"));
        QVERIFY(SyncPlan::fingerprint(source(), {"exe"}, rules) != changed);
    }
    void missingAndConflictingPathsFailBeforeCopy()
    {
        QCOMPARE(runWorker(QString(), target(), {"exe"}, {}).state, int(SyncWorker::Error));
        QCOMPARE(runWorker(source(), QString(), {"exe"}, {}).state, int(SyncWorker::Error));
        QVERIFY(writeFile(source() + "/app.exe"));
        QCOMPARE(runWorker(source(), target(), {"exe"}, {{"../missing", {}}}).state, int(SyncWorker::Error));
        QVERIFY(!QFileInfo::exists(target() + "/app.exe"));
        QVERIFY(writeFile(source() + "/Lang/a.qm"));
        QVERIFY(writeFile(target() + "/Lang", "a-file-not-directory"));
        QCOMPARE(runWorker(source(), target(), {"exe"}, {{"./Lang", {}}}).state, int(SyncWorker::Error));
        QVERIFY(!QFileInfo::exists(target() + "/app.exe"));
    }
    void sharedRootBoundary()
    {
        QVector<SyncPlan::Scope> scopes;
        QString error;
        QVERIFY(writeFile(source() + "/../Lang/zh.qm"));
        QVERIFY(!SyncPlan::build(source(), "//SERVER/b/Release", {}, {{"../../Lang", {}}}, &scopes, &error));
        QVERIFY2(error.contains("超出"), qPrintable(error));
    }
    void cancellationBeforeAndDuringCopy()
    {
        HostEntry host;
        host.sharePath = target();
        SyncWorker before(host, source(), {"exe"}, true);
        QSignalSpy beforeFinished(&before, &SyncWorker::finished);
        before.cancel();
        before.process();
        QCOMPARE(beforeFinished.size(), 1);
        // Many separate directories ensure there is still pending work at cancellation.
        for (int i = 0; i < 80; ++i) QVERIFY(writeFile(source() + QString("/d%1/a.exe").arg(i), QByteArray(128 * 1024, 'x')));
        SyncWorker worker(host, source(), {"exe"}, true);
        int state = -1, finished = 0, success = 0;
        QObject::connect(&worker, &SyncWorker::stateChanged, [&](const QString &, int value, int, const QString &, const QString &, const QString &) { state = value; });
        QObject::connect(&worker, &SyncWorker::finished, [&](const QString &) { ++finished; });
        QObject::connect(&worker, &SyncWorker::copySucceeded, [&](const QString &, const QDateTime &) { ++success; });
        std::thread cancel;
        QObject::connect(&worker, &SyncWorker::progressChanged, [&](const QString &, int, const QString &) {
            if (!cancel.joinable()) cancel = std::thread([flag = worker.cancellationFlag()]() { QThread::msleep(40); flag->store(true); });
        });
        worker.process();
        if (cancel.joinable()) cancel.join();
        QCOMPARE(state, int(SyncWorker::Stopped));
        QCOMPARE(finished, 1);
        QCOMPARE(success, 0);
        QVERIFY(!QFileInfo::exists(target() + "/d79/a.exe"));
    }
    void realUncShareCopy()
    {
        QVERIFY(writeFile(source() + "/app.exe", "UNC-EXE"));
        QVERIFY(writeFile(source() + "/../Lang/中文.qm", "UNC-LANG"));
        QString unc = target();
        const QString drive = unc.left(1);
        unc = "//localhost/" + drive + "$" + unc.mid(2);
        const Run run = runWorker(source(), unc, {"exe"}, {{"../Lang", {"qm"}}});
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("UNC-EXE"));
        QCOMPARE(readFile(target() + "/../Lang/中文.qm"), QByteArray("UNC-LANG"));
    }
    void commandLengthBatches()
    {
        for (int index = 0; index < 240; ++index) {
            QVERIFY(writeFile(source() + QString("/%1-").arg(index) + QString(110, 'x') + ".exe", "small-file"));
        }
        const Run run = runWorker(source(), target(), {"exe"}, {});
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(run.copied.size(), 240);
        QCOMPARE(QDir(target()).entryList({"*.exe"}, QDir::Files).size(), 240);
    }
#ifdef Q_OS_WIN
    void shortcutsDoNotBlockSync()
    {
        const QString app = source() + "/app.exe";
        const QString oldTarget = root() + "/old.exe";
        const QString remoteShortcut = target() + "/app.exe - 快捷方式.lnk";
        QVERIFY(writeFile(app, "EXE"));
        QVERIFY(writeFile(oldTarget));
        QVERIFY(QFile::link(oldTarget, source() + "/old.lnk"));
        QVERIFY(QFile::remove(oldTarget));
        QVERIFY(QFile::link(app, remoteShortcut));

        const Run run = runWorker(source(), target(), {}, {{".", {}}});
        QVERIFY2(run.state == SyncWorker::InSync, qPrintable(run.detail));
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("EXE"));
        QCOMPARE(run.copied, QStringList{"app.exe"});
        QVERIFY(QFileInfo::exists(remoteShortcut));

        QVector<SyncPlan::Scope> scopes;
        QString error;
        QVERIFY(!SyncPlan::build(source(), target(), {}, {{"old.lnk", {}}}, &scopes, &error));
        QVERIFY2(error.contains("快捷方式"), qPrintable(error));
        QVERIFY(!SyncPlan::build(source(), target(), {"*.LNK"}, {}, &scopes, &error));
        QVERIFY2(error.contains("快捷方式"), qPrintable(error));
    }
#endif
    void junctionIsRejected()
    {
        QVERIFY(writeFile(source() + "/real/file.exe"));
        const QString link = source() + "/linked";
        QProcess command;
        command.start("cmd.exe", {"/c", "mklink", "/J", QDir::toNativeSeparators(link), QDir::toNativeSeparators(source() + "/real")});
        QVERIFY(command.waitForFinished(5000));
        QCOMPARE(command.exitCode(), 0);
        const Run run = runWorker(source(), target(), {"exe"}, {});
        const bool removed = QDir().rmdir(link);
        QVERIFY(removed);
        QCOMPARE(run.state, int(SyncWorker::Error));
        QVERIFY(!QFileInfo::exists(target() + "/real/file.exe"));
    }
    void lockedDestinationReportsFailure()
    {
#ifdef Q_OS_WIN
        QVERIFY(writeFile(source() + "/app.exe", "updated-content"));
        const QString file = target() + "/app.exe";
        QVERIFY(writeFile(file, "old"));
        HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file).utf16()),
                                    GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(handle != INVALID_HANDLE_VALUE);
        const Run run = runWorker(source(), target(), {"exe"}, {});
        CloseHandle(handle);
        QCOMPARE(run.state, int(SyncWorker::Error));
        QCOMPARE(run.success, 0);
        QCOMPARE(readFile(file), QByteArray("old"));
#endif
    }
    void uiMigrationAndRuleEditing()
    {
        QVERIFY(writeFile(source() + "/image/icon.png"));
        auto main = window();
        main->show();
        if (QGuiApplication::platformName() == "offscreen") main->resize(1280, 860);
        auto *tabs = main->findChild<QTabWidget *>("syncTabs");
        auto *table = main->findChild<QTableWidget *>("ruleTable");
        QVERIFY(tabs && table);
        QCOMPARE(main->centralWidget()->objectName(), QString("centralWidget"));
        QCOMPARE(tabs->count(), 3);
        tabs->setCurrentWidget(main->findChild<QWidget *>("rulesTab"));
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("pathEdit")->setText("./image");
            dialog->findChild<QLineEdit *>("typesEdit")->setText(".PNG;*.jpg");
            QVERIFY(dialog->findChild<QLabel *>("previewLabel")->text().contains("image"));
            const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
            if (!screenshot.isEmpty()) {
                QDir().mkpath(screenshot);
                dialog->grab().save(screenshot + "/rule-editor.png");
            }
            dialog->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Save)->click();
        });
        main->findChild<QPushButton *>("addRuleButton")->click();
        QCOMPARE(table->rowCount(), 1);
        QCOMPARE(ConfigManager::load(configPath()).syncRules.first().fileTypes, QStringList({"jpg", "png"}));
        table->item(0, 0)->setCheckState(Qt::Unchecked);
        QVERIFY(!ConfigManager::load(configPath()).syncRules.first().enabled);
        main->findChild<QTableWidget *>("hostTable")->selectRow(0);
        QVERIFY(main->findChild<QPlainTextEdit *>("rulePreview")->toPlainText().contains("image"));
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QLineEdit *>("typesEdit")->clear();
            dialog->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Save)->click();
        });
        main->findChild<QPushButton *>("editRuleButton")->click();
        QVERIFY(ConfigManager::load(configPath()).syncRules.first().fileTypes.isEmpty());
        if (QGuiApplication::platformName() == "windows") {
            const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
            QVERIFY(main->width() <= available.width());
            QVERIFY(main->height() <= available.height());
            const auto *hosts = main->findChild<QTableWidget *>("hostTable");
            QVERIFY(hosts->isVisible());
            QVERIFY(main->rect().contains(hosts->mapTo(main.get(), hosts->rect().bottomRight())));
        }
        const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshot.isEmpty()) {
            QDir().mkpath(screenshot);
            main->grab().save(screenshot + "/rules-tab.png");
            tabs->setCurrentIndex(0);
            main->grab().save(screenshot + "/basic-tab.png");
        }
        main->findChild<QPushButton *>("removeRuleButton")->click();
        QCOMPARE(table->rowCount(), 0);
        QVERIFY(ConfigManager::load(configPath()).syncRules.isEmpty());
    }
    void usageTipsAndHostCopyStillWork()
    {
        auto main = window();
        main->show();
        auto *action = main->findChild<QAction *>("usageTipsAction");
        QVERIFY(action);
        action->trigger();
        QDialog *tips = main->findChild<QDialog *>("UsageTipsDialog");
        QVERIFY(tips);
        action->trigger();
        QCOMPARE(main->findChildren<QDialog *>("UsageTipsDialog").size(), 1);
        const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshot.isEmpty()) tips->grab().save(screenshot + "/usage-tips.png");
        tips->accept();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        main->findChild<QAction *>("usageTipsAction")->trigger();
        QVERIFY(main->findChild<QDialog *>("UsageTipsDialog"));
        auto *hostPath = main->findChild<QLineEdit *>("hostPathEdit");
        auto *hosts = main->findChild<QTableWidget *>("hostTable");
        hosts->selectRow(0);
        hostPath->setText("\\\\SERVER-QA\\share\\Release");
        main->findChild<QPushButton *>("updateHostButton")->click();
        QVERIFY(QMetaObject::invokeMethod(main.get(), "handleHostTableCellDoubleClicked", Q_ARG(int, 0), Q_ARG(int, 1)));
        QCOMPARE(QApplication::clipboard()->text(), QString("SERVER-QA"));
    }
    void completionToastUpdatesAndExpiresWithoutTakingFocus()
    {
        QWidget anchor;
        QLineEdit input(&anchor);
        anchor.resize(300, 100);
        anchor.show();
        input.setFocus();
        QTest::qWait(50);
        QWidget *focused = QApplication::focusWidget();
#ifdef Q_OS_WIN
        const HWND foreground = GetForegroundWindow();
#endif
        SyncCompletionToast toast(&anchor);
        ToastEvents events(&toast);
        auto *label = toast.findChild<QLabel *>("messageLabel");
        QVERIFY(label);
        toast.present("正在检查文件", "SERVER-A", SyncCompletionToast::Information);
        toast.present("同步完成", "SERVER-A", SyncCompletionToast::Success);
        QVERIFY(toast.isVisible());
        QCOMPARE(label->text(), QString("SERVER-A"));
        QCOMPARE(toast.findChild<QLabel *>("titleLabel")->text(), QString("同步完成"));
        QCOMPARE(QApplication::focusWidget(), focused);
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == "windows") {
            QCOMPARE(GetForegroundWindow(), foreground);
            const LONG_PTR style = GetWindowLongPtrW(reinterpret_cast<HWND>(toast.winId()), GWL_EXSTYLE);
            QVERIFY(style & WS_EX_NOACTIVATE);
            QVERIFY(style & WS_EX_TRANSPARENT);
            QVERIFY(style & WS_EX_TOOLWINDOW);
        }
#endif
        QVERIFY(toast.windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(toast.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
        QVERIFY(toast.testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(!label->wordWrap());
        const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshot.isEmpty()) {
            QDir().mkpath(screenshot);
            QVERIFY(toast.grab().save(screenshot + "/completion-toast.png"));
        }
        // Hiding the owner to the tray must leave the notification visible.
        anchor.hide();
        QVERIFY(toast.isVisible());
        const QRect icon = toast.findChild<QLabel *>("successIcon")->geometry();
        const QRect title = toast.findChild<QLabel *>("titleLabel")->geometry();
        const QImage first = toast.grab().toImage();
        QTest::qWait(700);
        const QImage peak = toast.grab().toImage();
        QTest::qWait(1000);
        const QImage settled = toast.grab().toImage();
        const auto pixels = [&](const QRect &logical) {
            const qreal scale = toast.devicePixelRatioF();
            return QRectF(logical.x() * scale, logical.y() * scale,
                          logical.width() * scale, logical.height() * scale).toAlignedRect();
        };
        QCOMPARE(first.copy(pixels(title)), peak.copy(pixels(title)));
        QCOMPARE(peak.copy(pixels(title)), settled.copy(pixels(title)));
#ifdef Q_OS_WIN
        BOOL effects = FALSE;
        if (SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &effects, 0))
            QCOMPARE(peak.copy(pixels(icon)) != settled.copy(pixels(icon)), effects != FALSE);
#endif
        QCOMPARE(events.messages, QStringList({"SERVER-A 正在检查文件", "SERVER-A 同步完成"}));
        QVERIFY(toast.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!toast.isVisible(), 2000);
        QVERIFY(events.clock.elapsed() - events.shownAt.at(1) >= 2950);
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == "windows")
            QVERIFY(GetForegroundWindow() != reinterpret_cast<HWND>(toast.winId()));
#endif
    }
    void completionToastFitsLongNamesAndDismissClearsQueue()
    {
        QWidget anchor;
        SyncCompletionToast toast(&anchor);
        ToastEvents events(&toast);
        toast.present("同步完成", QString("很长的主机名称 <SERVER> ").repeated(20) + "\nEND", SyncCompletionToast::Success);
        auto *label = toast.findChild<QLabel *>("messageLabel");
        QVERIFY(label);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QVERIFY(!label->text().contains('\n'));
        QVERIFY(label->text().contains(QChar(0x2026)));
        QVERIFY(label->fontMetrics().horizontalAdvance(label->text()) <= label->contentsRect().width());
        QScreen *screen = QGuiApplication::screenAt(toast.frameGeometry().center());
        QVERIFY(screen);
        QVERIFY(screen->availableGeometry().contains(toast.frameGeometry()));
        QCOMPARE(toast.frameGeometry().top() - screen->availableGeometry().top(),
                 qRound(screen->availableGeometry().height() * 0.12));
        const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshot.isEmpty()) {
            QDir().mkpath(screenshot);
            QVERIFY(toast.grab().save(screenshot + "/completion-toast-long.png"));
        }
        toast.dismiss();
        QVERIFY(!toast.isVisible());
        QTest::qWait(1100);
        QCOMPARE(events.messages.size(), 1);
        QVERIFY(!toast.isVisible());
        toast.present("同步完成", "NEW", SyncCompletionToast::Success);
        QCOMPARE(events.messages.last(), QString("NEW 同步完成"));
        toast.dismiss();
    }
    void successfulCopiesNotifyPerHostWhileHiddenButChecksStayQuiet()
    {
        QVERIFY(writeFile(source() + "/app.exe", "toast-copy"));
        auto main = window();
        auto *toast = completionToast();
        QVERIFY(toast);
        ToastEvents events(toast);
        main->findChild<QLineEdit *>("hostNameEdit")->setText("第二台主机");
        main->findChild<QLineEdit *>("hostPathEdit")->setText(target() + "-second");
        main->findChild<QPushButton *>("addHostButton")->click();
        QCOMPARE(ConfigManager::load(configPath()).hosts.size(), 2);
        QVERIFY(!main->isVisible());
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_COMPARE_WITH_TIMEOUT(events.messages.size(), 1, 15000);
        QVERIFY(events.messages.first().contains("测试主机"));
        QVERIFY(events.messages.first().contains("第二台主机"));
        QVERIFY(events.messages.first().endsWith("2 台主机同步完成"));
        QVERIFY(toast->isVisible());
        QVERIFY(!main->isVisible());
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("toast-copy"));
        QCOMPARE(readFile(target() + "-second/app.exe"), QByteArray("toast-copy"));
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!toast->isVisible(), 4000);
        // A subsequent no-op sync and an explicit check must produce no toast.
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        main->findChild<QPushButton *>("refreshButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QTest::qWait(100);
        QCOMPARE(events.messages.size(), 1);
        QVERIFY(!toast->isVisible());
    }
    void failedHostDoesNotSuppressSuccessfulHostNotification()
    {
        QVERIFY(writeFile(source() + "/app.exe", "one-success"));
        QVERIFY(writeFile(target() + "-blocked", "not a directory"));
        auto main = window();
        auto *toast = completionToast();
        QVERIFY(toast);
        ToastEvents events(toast);
        main->findChild<QLineEdit *>("hostNameEdit")->setText("失败主机");
        main->findChild<QLineEdit *>("hostPathEdit")->setText(target() + "-blocked");
        main->findChild<QPushButton *>("addHostButton")->click();
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(events.messages.size(), 1, 3000);
        QVERIFY(events.messages.first().contains("失败主机"));
        QVERIFY(events.messages.first().endsWith("1 台同步完成，1 台未完成"));
        const auto hosts = ConfigManager::load(configPath()).hosts;
        QVERIFY(hosts.at(0).lastSyncUtc.isValid());
        QVERIFY(!hosts.at(1).lastSyncUtc.isValid());
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("one-success"));
        QCOMPARE(readFile(target() + "-blocked"), QByteArray("not a directory"));
    }
    void cancelledSyncDoesNotNotify()
    {
        for (int i = 0; i < 80; ++i)
            QVERIFY(writeFile(source() + QString("/d%1/app.exe").arg(i), QByteArray(1024, 'x')));
        auto main = window();
        auto *toast = completionToast();
        QVERIFY(toast);
        ToastEvents events(toast);
        bool stopped = false;
        QTimer stop;
        stop.setInterval(5);
        connect(&stop, &QTimer::timeout, [&]() {
            if (main->findChild<QTableWidget *>("hostTable")->item(0, 5)->text() == "同步中") {
                stop.stop(); stopped = true;
                main->findChild<QPushButton *>("stopButton")->click();
            }
        });
        stop.start();
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(stopped, 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QTest::qWait(100);
        QVERIFY(events.messages.isEmpty());
        QVERIFY(!toast->isVisible());
        QVERIFY(!ConfigManager::load(configPath()).hosts.first().lastSyncUtc.isValid());
    }
    void autoSyncIncludesSiblingRuleAndStopDisablesIt()
    {
        QVERIFY(writeFile(source() + "/../Lang/zh.qm", "initial"));
        auto main = window({{"../Lang", {"qm"}}}, true);
        auto *toast = completionToast();
        QVERIFY(toast);
        ToastEvents events(toast);
        // The startup singleShot runs the same public workflow as normal startup.
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/../Lang/zh.qm"), QByteArray("initial"), 15000);
        QVERIFY(writeFile(source() + "/../Lang/zh.qm", "updated-language"));
        QTest::qWait(250);
        QTRY_VERIFY_WITH_TIMEOUT((QMetaObject::invokeMethod(main.get(), "pollForChanges"),
            readFile(target() + "/../Lang/zh.qm") == QByteArray("updated-language")), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(events.messages.size(), 2, 15000);
        QCOMPARE(events.messages, QStringList({"测试主机 同步完成", "测试主机 同步完成"}));
        main->findChild<QPushButton *>("stopButton")->click();
        QVERIFY(!main->findChild<QAction *>("autoSyncAction")->isChecked());
        QVERIFY(!ConfigManager::load(configPath()).autoSync);
    }
    void menusAndSummaryReplaceCommonControls()
    {
        auto main = window();
        main->show();
        auto *settings = main->findChild<QMenu *>("settingsMenu");
        auto *help = main->findChild<QMenu *>("helpMenu");
        auto *tools = main->findChild<QMenu *>("toolsMenu");
        QVERIFY(settings && help && tools);
        QCOMPARE(main->menuBar()->actions().size(), 3);
        for (const char *name : {"remoteDebuggerAction", "diagnosticsAction"})
            QVERIFY(tools->actions().contains(main->findChild<QAction *>(name)));
        for (const char *name : {"autoSyncAction", "preferencesAction", "saveConfigAction"})
            QVERIFY(settings->actions().contains(main->findChild<QAction *>(name)));
        for (const char *name : {"usageTipsAction", "checkUpdatesAction"})
            QVERIFY(help->actions().contains(main->findChild<QAction *>(name)));
        QVERIFY(!main->findChild<QCheckBox *>("autoSyncCheck"));
        QVERIFY(!main->findChild<QCheckBox *>("startWithSystemCheck"));
        QVERIFY(!main->findChild<QPushButton *>("checkUpdatesButton"));
        QVERIFY(!main->findChild<QPushButton *>("usageTipsButton"));
        QVERIFY(!main->findChild<QPushButton *>("saveConfigButton"));
        auto *summary = main->findChild<QLabel *>("settingsSummaryLabel");
        QVERIFY(summary && summary->text().contains("自动同步：关闭"));
        QVERIFY(summary->text().contains("5 秒"));
        QVERIFY(main->findChild<QLabel *>("updateNoticeLabel")->isHidden());
        main->findChild<QAction *>("saveConfigAction")->trigger();
        QVERIFY(!ConfigManager::load(configPath()).autoSync);
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->objectName(), QString("PreferencesDialog"));
            dialog->reject();
        });
        // Exercise the actual status-bar entry point, not just its menu action.
        QVERIFY(QMetaObject::invokeMethod(summary, "linkActivated", Q_ARG(QString, "preferences")));
        const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshot.isEmpty()) {
            QDir().mkpath(screenshot);
            main->grab().save(screenshot + "/main-menus.png");
            settings->popup(main->menuBar()->mapToGlobal(QPoint(0, main->menuBar()->height())));
            QTest::qWait(30);
            settings->grab().save(screenshot + "/settings-menu.png");
            settings->hide();
            help->popup(main->menuBar()->mapToGlobal(QPoint(70, main->menuBar()->height())));
            QTest::qWait(30);
            help->grab().save(screenshot + "/help-menu.png");
            help->hide();
        }
    }
    void manualShortcutRecordingConflictsCancelReloadAndClear()
    {
        QVERIFY(writeFile(source() + "/hotkey.exe", "manual-shortcut"));
        auto main = window();
        main->show();
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        QVERIFY(ConfigManager::load(configPath()).shortcuts.isEmpty());
        QSignalSpy clicks(main->findChild<QPushButton *>("syncButton"), &QPushButton::clicked);
        auto edit = [&](Qt::Key key, Qt::KeyboardModifiers mods, bool save, const QString &conflict, bool clear) {
            QTimer::singleShot(0, main.get(), [&, key, mods, save, conflict, clear]() {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                QTimer::singleShot(5000, dialog, &QDialog::reject);
                dialog->findChild<QTabWidget *>("preferencesTabs")->setCurrentWidget(dialog->findChild<QWidget *>("shortcutsTab"));
                auto *field = dialog->findChild<QLineEdit *>("manualSyncKeys");
                auto *warning = dialog->findChild<QLabel *>("manualSyncWarning");
                QVERIFY(field && warning);
                if (clear) dialog->findChild<QPushButton *>("manualSyncClear")->click();
                else {
                    dialog->findChild<QPushButton *>("manualSyncRecord")->click();
                    QTest::keyClick(field, key, mods);
                    QCOMPARE(field->text(), QKeySequence(int(key) | int(mods)).toString(QKeySequence::PortableText));
                }
                if (conflict.isEmpty()) QVERIFY(warning->text().isEmpty());
                else QVERIFY(warning->text().contains(conflict));
                auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox");
                QVERIFY(buttons->button(QDialogButtonBox::Save)->isEnabled());
                buttons->button(save ? QDialogButtonBox::Save : QDialogButtonBox::Cancel)->click();
            });
            main->showPreferences();
        };
        edit(Qt::Key_S, Qt::ControlModifier, true, QStringLiteral("保存配置"), false);
        auto bindings = ConfigManager::load(configPath()).shortcuts;
        QCOMPARE(bindings.size(), 1);
        QCOMPARE(bindings.first().keys, QString("Ctrl+S"));
        QSignalSpy saved(main->findChild<QAction *>("saveConfigAction"), &QAction::triggered);
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        QTest::keyClick(main.get(), Qt::Key_S, Qt::ControlModifier);
        QCOMPARE(saved.size(), 1);
        QCOMPARE(clicks.size(), 0);
        edit(Qt::Key_F7, Qt::ControlModifier, false, {}, false);
        QCOMPARE(ConfigManager::load(configPath()).shortcuts.first().keys, QString("Ctrl+S"));
        edit(Qt::Key_F7, Qt::ControlModifier, true, {}, false);
        QCOMPARE(clicks.size(), 0);
        main.reset();
        main = std::make_unique<MainWindow>(nullptr, configPath(), false);
        stopTimers(*main);
        main->show();
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        QSignalSpy afterReload(main->findChild<QPushButton *>("syncButton"), &QPushButton::clicked);
        QTest::keyClick(main.get(), Qt::Key_F7, Qt::ControlModifier);
        QCOMPARE(afterReload.size(), 1);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/hotkey.exe"), QByteArray("manual-shortcut"), 15000);
        edit(Qt::Key_F7, Qt::ControlModifier, true, {}, true);
        QVERIFY(ConfigManager::load(configPath()).shortcuts.isEmpty());
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        QTest::keyClick(main.get(), Qt::Key_F7, Qt::ControlModifier);
        QCOMPARE(afterReload.size(), 1);
    }

    void stopShortcutUsesTheExistingStopButton()
    {
        auto main = window({}, true);
        HostEntry disabled = main->m_config.hosts.first();
        disabled.id = "qa-disabled";
        disabled.enabled = false;
        main->m_config.hosts.append(disabled);
        main->refreshHostTable();
        main->show();
        QTimer::singleShot(0, main.get(), [&]() {
            auto *preferences = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(preferences);
            QTimer::singleShot(5000, preferences, &QDialog::reject);
            auto *tabs = preferences->findChild<QTabWidget *>("preferencesTabs");
            QVERIFY(tabs);
            tabs->setCurrentWidget(preferences->findChild<QWidget *>("shortcutsTab"));
            auto *group = preferences->findChild<QComboBox *>("shortcutGroup");
            auto *command = preferences->findChild<QComboBox *>("shortcutCommand");
            QVERIFY(group && command);
            QCOMPARE(group->currentText(), QString("同步"));
            QCOMPARE(command->count(), 2);
            command->setCurrentText("停止所有同步");
            auto *keys = preferences->findChild<QLineEdit *>("stopSyncKeys");
            QVERIFY(keys && keys->isVisible() && keys->text().isEmpty());
            keys->setText("Ctrl+F8");
            preferences->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Save)->click();
        });
        main->showPreferences();
        const auto bindings = ConfigManager::load(configPath()).shortcuts;
        QCOMPARE(bindings.size(), 1);
        QCOMPARE(bindings.first().action, QString("stopSync"));
        main->m_pendingSyncAfterCurrentRun = true;
        main->m_pendingShortcut = true;
        auto *button = main->findChild<QPushButton *>("stopButton");
        QSignalSpy clicks(button, &QPushButton::clicked);
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        QTest::keyClick(main.get(), Qt::Key_F8, Qt::ControlModifier);
        QCOMPARE(clicks.count(), 1);
        QVERIFY(main->m_backgroundTasksPaused);
        QVERIFY(!main->m_pendingSyncAfterCurrentRun);
        QVERIFY(!main->m_pendingShortcut);
        const AppConfig saved = ConfigManager::load(configPath());
        QVERIFY(!saved.autoSync);
        QCOMPARE(saved.hosts.size(), 2);
        QVERIFY(saved.hosts.at(0).enabled);
        QVERIFY(!saved.hosts.at(1).enabled);
        QCOMPARE(main->m_hostTable->item(0, 0)->checkState(), Qt::Checked);
        QCOMPARE(main->m_hostTable->item(1, 0)->checkState(), Qt::Unchecked);
    }

    void shortcutCommandsResolveInBindingOrderAndRespectWindowScope()
    {
        QWidget main;
        QDialog separate(&main);
        separate.resize(240, 100);
        QPushButton child(QStringLiteral("操作"), &separate);
        QAction elsewhere(QStringLiteral("其他窗口操作"), &separate);
        elsewhere.setShortcut(QKeySequence("Ctrl+F8"));
        separate.addAction(&elsewhere);
        int first = 0, second = 0;
        ShortcutManager manager(&main);
        manager.add({"first", "First", [&]() { ++first; }});
        manager.add({"second", "Second", [&]() { ++second; }});
        QVector<ShortcutBinding> bindings{{"second", "Ctrl+F8", false}, {"first", "Ctrl+F8", false}};
        QVERIFY(manager.warning(bindings, bindings.first(), false).isEmpty());
        QVERIFY(manager.warning(bindings, bindings.last(), false).contains("Second"));
        auto global = bindings.first();
        global.global = true;
        QVERIFY(manager.warning(bindings, global, false).contains(elsewhere.text()));
        manager.apply(bindings);
        main.show();
        main.activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(&main));
        QTest::keyClick(&main, Qt::Key_F8, Qt::ControlModifier);
        QCOMPARE(first, 0);
        QCOMPARE(second, 1);
        separate.show();
        separate.raise();
        separate.activateWindow();
        child.setFocus();
        QTRY_COMPARE(QApplication::activeWindow(), static_cast<QWidget *>(&separate));
        QSignalSpy other(&elsewhere, &QAction::triggered);
        QTest::keyClick(&child, Qt::Key_F8, Qt::ControlModifier);
        QCOMPARE(other.size(), 1);
        QCOMPARE(second, 1);
    }

    void shortcutFeedbackTracksRequestsAndPreference()
    {
        auto main = window();
        auto config = ConfigManager::load(configPath());
        QVERIFY(config.showShortcutFeedback);
        config.shortcuts = {{"manualSync", "Ctrl+F7", false}};
        main.reset();
        QVERIFY(ConfigManager::save(configPath(), config));
        const auto reopen = [&]() {
            main = std::make_unique<MainWindow>(nullptr, configPath(), false);
            stopTimers(*main);
            main->show();
            main->activateWindow();
            QVERIFY(QTest::qWaitForWindowActive(main.get()));
        };
        reopen();
        auto *toast = completionToast();
        QVERIFY(toast);
        QSignalSpy notices(toast, &SyncCompletionToast::presented);
        const auto press = [&]() { QTest::keyClick(main.get(), Qt::Key_F7, Qt::ControlModifier); };
        for (int i = 0; i < 12; ++i)
            QVERIFY(writeFile(source() + QString("/file%1.exe").arg(i), "feedback"));
        press();
        press();
        press();
        QVERIFY(notices.size() >= 3);
        QCOMPARE(notices.at(0).at(0).toString(), QString("已收到手动同步请求"));
        QCOMPARE(notices.at(1).at(0).toString(), QString("手动同步已排队"));
        QCOMPARE(notices.at(2).at(0).toString(), QString("手动同步已在队列中"));
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(notices.size(), 6);
        QCOMPARE(notices.at(3).at(0).toString(), QString("同步完成"));
        QCOMPARE(notices.last().at(0).toString(), QString("文件已是最新，无需同步"));
        QCOMPARE(readFile(target() + "/file11.exe"), QByteArray("feedback"));
        auto *host = main->findChild<QTableWidget *>("hostTable")->item(0, 0);
        host->setCheckState(Qt::Unchecked);
        press();
        QCOMPARE(notices.last().at(1).toString(), QString("请先勾选目标主机"));
        host->setCheckState(Qt::Checked);
        const auto setFeedback = [&](bool enabled) {
            QTimer::singleShot(0, main.get(), [enabled]() {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                auto *check = dialog->findChild<QCheckBox *>("shortcutFeedbackCheck");
                check->setChecked(enabled);
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            });
            main->showPreferences();
        };
        setFeedback(false);
        main.reset();
        QVERIFY(!ConfigManager::load(configPath()).showShortcutFeedback);
        reopen();
        QSignalSpy afterReload(completionToast(), &SyncCompletionToast::presented);
        QVERIFY(writeFile(source() + "/new.exe", "disabled-feedback"));
        press();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(readFile(target() + "/new.exe"), QByteArray("disabled-feedback"));
        QCOMPARE(afterReload.size(), 1);
        QCOMPARE(afterReload.first().at(0).toString(), QString("同步完成"));
        press();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(afterReload.size(), 1);
        setFeedback(true);
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        press();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(afterReload.last().at(0).toString(), QString("文件已是最新，无需同步"));
    }

    void globalShortcutHandlesOccupiedKeyAndHiddenWindow()
    {
#ifdef Q_OS_WIN
        QVERIFY(writeFile(source() + "/global.exe", "tray-shortcut"));
        auto main = window();
        QWidget owner;
        const HWND hwnd = reinterpret_cast<HWND>(owner.winId());
        struct Reservation {
            HWND hwnd;
            bool held;
            ~Reservation() { if (held) UnregisterHotKey(hwnd, 77); }
        } reservation{hwnd, RegisterHotKey(hwnd, 77, MOD_NOREPEAT, VK_F23) != FALSE};
        QVERIFY(reservation.held);
        QTimer::singleShot(0, main.get(), [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QTimer::singleShot(5000, dialog, &QDialog::reject);
            dialog->findChild<QLineEdit *>("manualSyncKeys")->setText("F23");
            dialog->findChild<QComboBox *>("manualSyncScope")->setCurrentIndex(1);
            QVERIFY(dialog->findChild<QLabel *>("manualSyncWarning")->text().contains(QStringLiteral("占用")));
            dialog->findChild<QTabWidget *>("preferencesTabs")->setCurrentWidget(dialog->findChild<QWidget *>("shortcutsTab"));
            const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
            if (!screenshots.isEmpty()) {
                QVERIFY(QDir().mkpath(screenshots));
                QVERIFY(dialog->grab().save(screenshots + "/shortcut-conflict.png"));
            }
            dialog->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Save)->click();
        });
        main->showPreferences();
        const auto saved = ConfigManager::load(configPath()).shortcuts;
        QCOMPARE(saved.size(), 1);
        QVERIFY(saved.first().global);
        QVERIFY(main->findChild<QPushButton *>("syncButton")->toolTip().contains(QStringLiteral("不可用")));
        QVERIFY(UnregisterHotKey(hwnd, 77));
        reservation.held = false;
        main.reset();
        main = std::make_unique<MainWindow>(nullptr, configPath(), false);
        stopTimers(*main);
        main->show();
        main->hide();
        QVERIFY(!main->isVisible());
        auto *button = main->findChild<QPushButton *>("syncButton");
        QVERIFY(!button->toolTip().contains(QStringLiteral("不可用")));
        QSignalSpy triggered(button, &QPushButton::clicked);
        QSignalSpy notices(completionToast(), &SyncCompletionToast::presented);
        // 在已由本测试窗口注册的键上产生按下、重复按下、松开，经 Windows 消息循环投递。
        INPUT input[3] = {};
        for (auto &event : input) { event.type = INPUT_KEYBOARD; event.ki.wVk = VK_F23; }
        input[2].ki.dwFlags = KEYEVENTF_KEYUP;
        QCOMPARE(SendInput(3, input, sizeof(INPUT)), UINT(3));
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 1, 3000);
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/global.exe"), QByteArray("tray-shortcut"), 15000);
        QCOMPARE(triggered.size(), 1);
        QVERIFY(!main->isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(notices.first().at(0).toString(), QString("已收到手动同步请求"));
        QCOMPARE(notices.last().at(0).toString(), QString("同步完成"));
        main->showMinimized();
        QVERIFY(main->isMinimized());
        QCOMPARE(SendInput(3, input, sizeof(INPUT)), UINT(3));
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 2, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(notices.last().at(0).toString(), QString("文件已是最新，无需同步"));
        QVERIFY(main->isMinimized());
        main.reset();
        reservation.held = RegisterHotKey(hwnd, 77, MOD_NOREPEAT, VK_F23) != FALSE;
        QVERIFY(reservation.held);
#endif
    }

    void foregroundShortcutPreservesPinyinComposition()
    {
#ifdef Q_OS_WIN
        if (!qEnvironmentVariableIsSet("SYNC_QA_PINYIN")) QSKIP("Requires an active Chinese Pinyin input method");
        auto main = window();
        AppConfig config = ConfigManager::load(configPath());
        config.shortcuts = {{"manualSync", "F23", true}};
        main.reset();
        QVERIFY(ConfigManager::save(configPath(), config));
        main = std::make_unique<MainWindow>(nullptr, configPath(), false);
        stopTimers(*main);
        main->show();
        main->activateWindow();
        QVERIFY(QTest::qWaitForWindowActive(main.get()));
        auto *editor = main->findChild<QLineEdit *>("hostNameEdit");
        editor->clear();
        editor->setFocus();
        QTRY_VERIFY(editor->hasFocus());
        const HWND inputWindow = GetFocus();
        const HIMC context = ImmGetContext(inputWindow);
        QVERIFY(context);
        struct RestoreIme {
            HWND window;
            HIMC context;
            BOOL open;
            DWORD conversion = 0, sentence = 0;
            ~RestoreIme() {
                ImmSetConversionStatus(context, conversion, sentence);
                ImmSetOpenStatus(context, open);
                ImmReleaseContext(window, context);
            }
        } ime{inputWindow, context, ImmGetOpenStatus(context)};
        QVERIFY(ImmGetConversionStatus(context, &ime.conversion, &ime.sentence));
        ImmSetOpenStatus(context, TRUE);
        ImmSetConversionStatus(context, IME_CMODE_NATIVE, ime.sentence);
        struct Composition : QObject {
            QString preedit;
            bool eventFilter(QObject *, QEvent *event) override {
                if (event->type() == QEvent::InputMethod)
                    preedit = static_cast<QInputMethodEvent *>(event)->preeditString();
                return false;
            }
        } composition;
        editor->installEventFilter(&composition);
        const auto press = [&](WORD vk) {
            if (GetForegroundWindow() != reinterpret_cast<HWND>(main->winId()) || !editor->hasFocus()) return false;
            INPUT keys[2]{};
            for (auto &key : keys) { key.type = INPUT_KEYBOARD; key.ki.wVk = vk; }
            keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
            return SendInput(2, keys, sizeof(INPUT)) == 2;
        };
        QVERIFY(press('N'));
        QVERIFY(press('I'));
        QTRY_COMPARE_WITH_TIMEOUT(composition.preedit, QString("ni"), 2000);
        const QString preedit = composition.preedit;
        const LONG compositionBytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
        QSignalSpy notices(completionToast(), &SyncCompletionToast::presented);
        QVERIFY(press(VK_F23));
        QTRY_VERIFY_WITH_TIMEOUT(!notices.isEmpty(), 3000);
        QCOMPARE(composition.preedit, preedit);
        QCOMPARE(ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0), compositionBytes);
        QVERIFY(press(VK_SPACE));
        QTRY_VERIFY_WITH_TIMEOUT(editor->text().contains(QRegularExpression("[\\x{4E00}-\\x{9FFF}]")), 2000);
        QVERIFY(editor->hasFocus());
#else
        QSKIP("Windows Pinyin input method");
#endif
    }

    void preferencesUpdateRowsRemainReadable()
    {
        auto main = window();
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            const auto checkLayout = [&]() {
                auto *check = dialog->findChild<QCheckBox *>("silentUpdateCheck");
                auto *hint = dialog->findChild<QLabel *>("updateHintLabel");
                auto *tabs = dialog->findChild<QTabWidget *>("preferencesTabs");
                auto *buttons = dialog->findChild<QDialogButtonBox *>("buttonBox");
                auto *scroll = dialog->findChild<QScrollArea *>("generalScroll");
                QVERIFY(check && hint && tabs && buttons);
                QVERIFY(scroll);
                auto *screen = QGuiApplication::screenAt(dialog->frameGeometry().center());
                if (screen && dialog->height() < screen->availableGeometry().height())
                    QCOMPARE(scroll->verticalScrollBar()->maximum(), 0);
                const QString directory = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
                for (const QSize &size : {dialog->size(), QSize(544, 464), dialog->minimumSize()}) {
                    dialog->resize(size);
                    QApplication::processEvents();
                    if (scroll) scroll->ensureWidgetVisible(hint, 0, check->sizeHint().height());
                    QApplication::processEvents();
                    if (size == QSize(544, 464) && !directory.isEmpty()) {
                        QVERIFY(QDir().mkpath(directory));
                        QVERIFY(dialog->grab().save(directory + "/preferences-compact.png"));
                    }
                    QVERIFY2(check->height() >= check->sizeHint().height(), "silent update checkbox clipped");
                    QVERIFY2(hint->height() >= hint->heightForWidth(hint->width()), "update explanation clipped");
                    QVERIFY(check->geometry().bottom() + 1 < hint->geometry().top());
                    QVERIFY(check->visibleRegion().contains(check->rect().center()));
                    QVERIFY(hint->visibleRegion().contains(hint->rect().center()));
                    for (auto role : {QDialogButtonBox::Save, QDialogButtonBox::Cancel}) {
                        auto *button = buttons->button(role);
                        QVERIFY(dialog->rect().contains(QRect(button->mapTo(dialog, QPoint()), button->size())));
                    }
                }
                QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
                QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(1).center());
                QCOMPARE(tabs->currentIndex(), 1);
                QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(0).center());
                QCOMPARE(tabs->currentIndex(), 0);
                QFont enlarged = hint->font();
                enlarged.setPixelSize(hint->fontMetrics().height() * 2);
                hint->setFont(enlarged);
                QTRY_VERIFY(hint->heightForWidth(hint->width()) > hint->fontMetrics().height());
                QTRY_VERIFY(hint->height() >= hint->heightForWidth(hint->width()));
            };
            checkLayout();
            dialog->reject();
        });
        main->findChild<QAction *>("preferencesAction")->trigger();
    }

    void preferencesSaveCancelAndReload()
    {
        QVERIFY(writeFile(source() + "/app.exe", "preferences-save"));
        auto main = window();
        auto *action = main->findChild<QAction *>("preferencesAction");
        QVERIFY(action);
        const QByteArray before = readFile(configPath());
        auto edit = [&](bool save) {
            QTimer::singleShot(0, [&, save]() {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                QVERIFY(dialog);
                QVERIFY(!dialog->findChild<QCheckBox *>("autoSyncCheck")->isChecked());
                QCOMPARE(dialog->findChild<QSpinBox *>("intervalSpin")->value(), 5);
                dialog->findChild<QCheckBox *>("autoSyncCheck")->setChecked(true);
                dialog->findChild<QCheckBox *>("startWithSystemCheck")->setChecked(true);
                dialog->findChild<QCheckBox *>("checkUpdatesOnStartupCheck")->setChecked(true);
                dialog->findChild<QCheckBox *>("silentUpdateCheck")->setChecked(true);
                dialog->findChild<QSpinBox *>("intervalSpin")->setValue(17);
                if (!save) {
                    QTest::qWait(400); // Startup checks must still use the old preferences.
                    QCOMPARE(readFile(configPath()), before);
                    QVERIFY(!QFileInfo::exists(target() + "/app.exe"));
                }
                const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
                if (save && !screenshot.isEmpty()) {
                    QDir().mkpath(screenshot);
                    QVERIFY(dialog->grab().save(screenshot + "/preferences.png"));
                    if (QGuiApplication::platformName() == "windows")
                        QVERIFY(QGuiApplication::primaryScreen()->availableGeometry().contains(dialog->frameGeometry()));
                }
                dialog->findChild<QDialogButtonBox *>("buttonBox")->button(
                    save ? QDialogButtonBox::Save : QDialogButtonBox::Cancel)->click();
            });
            action->trigger();
        };
        edit(false);
        QCOMPARE(readFile(configPath()), before);
        QVERIFY(!main->findChild<QAction *>("autoSyncAction")->isChecked());
        edit(true);
        const AppConfig saved = ConfigManager::load(configPath());
        QVERIFY(saved.autoSync && saved.startWithSystem && saved.checkUpdatesOnStartup && saved.silentUpdate);
        QCOMPARE(saved.scanIntervalSeconds, 17);
        QCOMPARE(main->findChild<QTimer *>("pollTimer")->interval(), 17000);
        QVERIFY(main->findChild<QAction *>("autoSyncAction")->isChecked());
        QVERIFY(main->findChild<QLabel *>("settingsSummaryLabel")->text().contains("开启"));
        QVERIFY(main->findChild<QLabel *>("settingsSummaryLabel")->text().contains("17 秒"));
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/app.exe"), QByteArray("preferences-save"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        main.reset();
        main = std::make_unique<MainWindow>(nullptr, configPath(), false);
        stopTimers(*main);
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            for (const char *name : {"autoSyncCheck", "startWithSystemCheck", "checkUpdatesOnStartupCheck", "silentUpdateCheck"})
                QVERIFY(dialog->findChild<QCheckBox *>(name)->isChecked());
            QCOMPARE(dialog->findChild<QSpinBox *>("intervalSpin")->value(), 17);
            dialog->reject();
        });
        main->findChild<QAction *>("preferencesAction")->trigger();
    }
    void failedPreferenceSaveKeepsCurrentSettings()
    {
        QVERIFY(writeFile(source() + "/app.exe", "must-not-copy"));
        auto main = window();
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            // Make the isolated test config path unwritable as a file.
            QVERIFY(QFile::rename(configPath(), configPath() + ".before"));
            QVERIFY(QDir().mkpath(configPath()));
            dialog->findChild<QCheckBox *>("autoSyncCheck")->setChecked(true);
            dialog->findChild<QSpinBox *>("intervalSpin")->setValue(19);
            dialog->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Save)->click();
            QVERIFY(dialog->isVisible());
            auto *error = dialog->findChild<QLabel *>("errorLabel");
            QVERIFY(error->isVisible() && !error->text().isEmpty());
            QVERIFY(!main->findChild<QAction *>("autoSyncAction")->isChecked());
            QCOMPARE(main->findChild<QTimer *>("pollTimer")->interval(), 5000);
            QVERIFY(main->findChild<QLabel *>("settingsSummaryLabel")->text().contains("5 秒"));
            QVERIFY(!QFileInfo::exists(target() + "/app.exe"));
            dialog->reject();
            QVERIFY(QDir().rmdir(configPath()));
            QVERIFY(QFile::rename(configPath() + ".before", configPath()));
        });
        main->findChild<QAction *>("preferencesAction")->trigger();
        QVERIFY(!ConfigManager::load(configPath()).autoSync);
    }
    void preferencesPreserveCopiesCompletedWhileEditing()
    {
        QVERIFY(writeFile(source() + "/app.exe", "finish-during-edit"));
        auto main = window();
        main->findChild<QPushButton *>("syncButton")->click();
        QDateTime completed;
        QTimer::singleShot(0, [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            dialog->findChild<QSpinBox *>("intervalSpin")->setValue(9);
            QTRY_VERIFY_WITH_TIMEOUT(ConfigManager::load(configPath()).hosts.first().lastSyncUtc.isValid(), 15000);
            completed = ConfigManager::load(configPath()).hosts.first().lastSyncUtc;
            dialog->findChild<QDialogButtonBox *>("buttonBox")->button(QDialogButtonBox::Save)->click();
        });
        main->findChild<QAction *>("preferencesAction")->trigger();
        QVERIFY(completed.isValid());
        const AppConfig saved = ConfigManager::load(configPath());
        QCOMPARE(saved.hosts.first().lastSyncUtc, completed);
        QCOMPARE(saved.scanIntervalSeconds, 9);
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("finish-during-edit"));
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
    }
    void autoSyncMenuControlsCopiesAndStopUpdatesSummary()
    {
        QVERIFY(writeFile(source() + "/app.exe", "initial"));
        auto main = window();
        auto *action = main->findChild<QAction *>("autoSyncAction");
        QVERIFY(action);
        action->trigger();
        QVERIFY(action->isChecked());
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/app.exe"), QByteArray("initial"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        action->trigger();
        QVERIFY(!action->isChecked());
        QVERIFY(!ConfigManager::load(configPath()).autoSync);
        QVERIFY(writeFile(source() + "/app.exe", "changed-with-auto-off"));
        QVERIFY(QMetaObject::invokeMethod(main.get(), "pollForChanges"));
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("initial"));
        action->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/app.exe"), QByteArray("changed-with-auto-off"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        main->findChild<QPushButton *>("stopButton")->click();
        QVERIFY(!action->isChecked());
        QVERIFY(!ConfigManager::load(configPath()).autoSync);
        QVERIFY(main->findChild<QLabel *>("settingsSummaryLabel")->text().contains("自动同步：关闭"));
    }
    void scanIntervalBoundsRemainCompatible()
    {
        for (int interval : {0, 1, 5, 3600, 3601, 2147483647}) {
            AppConfig config;
            config.releaseDir = source();
            config.autoSync = false;
            config.scanIntervalSeconds = interval;
            QString error;
            QVERIFY(ConfigManager::save(configPath(), config, &error));
            MainWindow main(nullptr, configPath(), false);
            stopTimers(main);
            const int expected = qBound(1, interval, 3600);
            QCOMPARE(main.findChild<QTimer *>("pollTimer")->interval(), expected * 1000);
            QVERIFY(main.findChild<QLabel *>("settingsSummaryLabel")->text().contains(QString::number(expected) + " 秒"));
            main.findChild<QAction *>("saveConfigAction")->trigger();
            QCOMPARE(ConfigManager::load(configPath()).scanIntervalSeconds, expected);
        }
    }

    void passwordVisibilityAndUptime()
    {
        auto main = window();
        main->show();
        auto *password = main->findChild<QLineEdit *>("hostPasswordEdit");
        auto *reveal = main->findChild<QToolButton *>("showPasswordButton");
        auto *uptime = main->findChild<QLabel *>("uptimeLabel");
        QVERIFY(password && reveal && uptime);
        password->setText("qa-only-credential");
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QVERIFY(!reveal->isCheckable());
        QVERIFY(!reveal->icon().isNull());
        QTest::mousePress(reveal, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QCOMPARE(password->displayText(), QString("qa-only-credential"));
        QTest::mouseRelease(reveal, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QTest::mousePress(reveal, Qt::LeftButton);
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QEvent inactive(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(main.get(), &inactive);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
        QTest::mouseRelease(reveal, Qt::LeftButton);
        QCOMPARE(password->text(), QString("qa-only-credential"));
        const QString initial = uptime->text();
        for (auto *timer : main->findChildren<QTimer *>()) {
            if (timer->interval() == 1000) timer->start();
        }
        QTRY_VERIFY_WITH_TIMEOUT(uptime->text() != initial, 3500);
        QVERIFY(uptime->text().startsWith("运行 "));
    }

    void excludeAndInvertOnRealShare()
    {
        QVERIFY(writeFile(source() + "/app.exe"));
        QVERIFY(writeFile(source() + "/good.dll"));
        QVERIFY(writeFile(source() + "/SKIP.dll"));
        QVERIFY(writeFile(source() + "/SkipExtra.dll"));
        QVERIFY(writeFile(source() + "/assets/example.lib"));
        QVERIFY(writeFile(source() + "/assets/example.pdb"));
        QVERIFY(writeFile(source() + "/assets/keep.lib"));
        QVERIFY(writeFile(source() + "/assets/log1.txt"));
        QVERIFY(writeFile(source() + "/assets/log11.txt"));
        QVERIFY(writeFile(target() + "/assets/example.pdb", "existing symbol stays"));
        QString unc = target();
        unc = "//localhost/" + unc.left(1) + "$" + unc.mid(2);

        auto main = window({{"./assets", {}}});
        AppConfig config = ConfigManager::load(configPath());
        config.hosts[0].sharePath = unc;
        QVERIFY(ConfigManager::save(configPath(), config));
        main.reset(new MainWindow(nullptr, configPath(), false));
        stopTimers(*main);
        auto *edit = main->findChild<QPlainTextEdit *>("exclusionsEdit");
        auto *apply = main->findChild<QPushButton *>("applyExclusionsButton");
        QVERIFY(edit && apply);
        edit->setPlainText("../invalid");
        apply->click();
        QVERIFY(!main->findChild<QLabel *>("exclusionsError")->isHidden());
        QVERIFY(ConfigManager::load(configPath()).excludedFilePatterns.isEmpty());
        edit->setPlainText("skip*.dll; example.lib\npdb\nlog?.txt");
        apply->click();
        QCOMPARE(ConfigManager::load(configPath()).excludedFilePatterns,
                 QStringList({"example.lib", "log?.txt", "pdb", "skip*.dll"}));
        main->findChild<QPushButton *>("invertTypeButton")->click();
        QCOMPARE(ConfigManager::load(configPath()).selectedFileTypes, QStringList({"dll", "pdb"}));
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target() + "/assets/keep.lib"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QVERIFY(QFileInfo::exists(target() + "/good.dll"));
        QVERIFY(!QFileInfo::exists(target() + "/app.exe"));
        QVERIFY(!QFileInfo::exists(target() + "/SKIP.dll"));
        QVERIFY(!QFileInfo::exists(target() + "/SkipExtra.dll"));
        QVERIFY(!QFileInfo::exists(target() + "/assets/example.lib"));
        QVERIFY(!QFileInfo::exists(target() + "/assets/log1.txt"));
        QVERIFY(QFileInfo::exists(target() + "/assets/log11.txt"));
        QCOMPARE(readFile(target() + "/assets/example.pdb"), QByteArray("existing symbol stays"));
        const auto patterns = ConfigManager::load(configPath()).excludedFilePatterns;
        const auto fingerprint = SyncPlan::fingerprint(source(), {"dll", "pdb"}, config.syncRules, patterns);
        QVERIFY(writeFile(source() + "/SKIP.dll", "excluded source changed"));
        QCOMPARE(SyncPlan::fingerprint(source(), {"dll", "pdb"}, config.syncRules, patterns), fingerprint);
    }

    void synchronizationTabBadgesCountValidEntries()
    {
        auto main = window({{"./Lang", {"qm"}}});
        auto *tabs = main->findChild<QTabWidget *>("syncTabs");
        auto *types = main->findChild<QListWidget *>("typeList");
        auto *exclusions = main->findChild<QPlainTextEdit *>("exclusionsEdit");
        QVERIFY(tabs && types && exclusions);
        const auto badge = [tabs](const char *page) {
            const int index = tabs->indexOf(tabs->findChild<QWidget *>(page));
            const auto *label = qobject_cast<QLabel *>(tabs->tabBar()->tabButton(index, QTabBar::RightSide));
            return label ? label->text() : QString();
        };
        QCOMPARE(badge("basicTab"), QString("1"));
        QCOMPARE(badge("rulesTab"), QString("1"));
        QVERIFY(badge("exclusionsTab").isEmpty());
        main->show();
        auto *tabBar = tabs->tabBar();
        const int basic = tabs->indexOf(tabs->findChild<QWidget *>("basicTab"));
        auto *basicBadge = tabBar->tabButton(basic, QTabBar::RightSide);
        QVERIFY(basicBadge);
        QTRY_COMPARE(basicBadge->y(), tabBar->tabRect(basic).top() + 3);
        QCOMPARE(tabBar->tabRect(basic).right() - basicBadge->geometry().right(), 6);
        tabs->setCurrentIndex(tabs->indexOf(tabs->findChild<QWidget *>("rulesTab")));
        const QPoint badgeCenter = basicBadge->geometry().center();
        QCOMPARE(tabBar->tabAt(badgeCenter), basic);
        QVERIFY(basicBadge->testAttribute(Qt::WA_TransparentForMouseEvents));
        QTest::mouseClick(tabBar, Qt::LeftButton, Qt::NoModifier, badgeCenter);
        QCOMPARE(tabs->currentIndex(), basic);
        exclusions->setPlainText("*.pdb；skip.dll\n*.PDB");
        QCOMPARE(badge("exclusionsTab"), QString("2"));
        QStringList patterns;
        for (int suffix = 0; suffix < 12; ++suffix) patterns.append(QString("skip%1.dll").arg(suffix));
        exclusions->setPlainText(patterns.join('\n'));
        QCOMPARE(badge("exclusionsTab"), QString("12"));
        for (int tab = 0; tab < tabs->count(); ++tab) {
            auto *indicator = tabBar->tabButton(tab, QTabBar::RightSide);
            QVERIFY(indicator);
            QTRY_COMPARE(indicator->y(), tabBar->tabRect(tab).top() + 3);
            QCOMPARE(tabBar->tabRect(tab).right() - indicator->geometry().right(), 6);
        }
        exclusions->setPlainText("wrong/path");
        QVERIFY(badge("exclusionsTab").isEmpty());
        for (int row = 0; row < types->count(); ++row) types->item(row)->setCheckState(Qt::Unchecked);
        QVERIFY(badge("basicTab").isEmpty());
    }

    void tabSwitchKeepsLayoutAndShowsExclusionExample()
    {
        QVERIFY(writeFile(source() + "/assets/zh.qm"));
        auto main = window({{"./assets", {"pdb", "qm"}}});
        main->show();
        if (QGuiApplication::platformName() == "windows") QVERIFY(QTest::qWaitForWindowExposed(main.get()));
        QTest::qWait(50);
        auto *tabs = main->findChild<QTabWidget *>("syncTabs");
        auto *hosts = main->findChild<QTableWidget *>("hostTable");
        auto *settings = main->findChild<QScrollArea *>("settingsScroll");
        QVERIFY(tabs && hosts && settings);
        if (!qEnvironmentVariableIsEmpty("SYNC_QA_SCALE_FACTOR")) {
            QCOMPARE(main->devicePixelRatioF(), qEnvironmentVariable("SYNC_QA_SCALE_FACTOR").toDouble());
        }
        const QRect frame = main->frameGeometry();
        const QRect hostGeometry(hosts->mapTo(main.get(), QPoint()), hosts->size());
        const QRect tabGeometry(tabs->mapTo(main.get(), QPoint()), tabs->size());
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) QVERIFY(QDir().mkpath(screenshots));

        qInfo() << "Screen available" << QGuiApplication::primaryScreen()->availableGeometry().size()
                << "logical DPI" << QGuiApplication::primaryScreen()->logicalDotsPerInch()
                << "DPR" << main->devicePixelRatioF() << "window" << frame.size();
        if (QGuiApplication::platformName() == "windows") {
            const QSize available = QGuiApplication::primaryScreen()->availableGeometry().size();
            QCOMPARE(frame.size().boundedTo(available), frame.size());
        }
        for (const int index : {1, 2, 0, 1}) {
            QTest::mouseClick(tabs->tabBar(), Qt::LeftButton, Qt::NoModifier, tabs->tabBar()->tabRect(index).center());
            QTest::qWait(20);
            QCOMPARE(tabs->currentIndex(), index);
            QCOMPARE(main->frameGeometry(), frame);
            QCOMPARE(hosts->mapTo(main.get(), QPoint()), hostGeometry.topLeft());
            QCOMPARE(hosts->width(), hostGeometry.width());
            // The status-bar size grip can round the bottom extent to either adjacent pixel.
            QVERIFY(qAbs(hosts->height() - hostGeometry.height()) <= 1);
            QCOMPARE(QRect(tabs->mapTo(main.get(), QPoint()), tabs->size()), tabGeometry);
            if (!screenshots.isEmpty())
                QVERIFY(main->grab().save(screenshots + QString("/layout-tab-%1.png").arg(index)));
        }
        QCOMPARE(settings->horizontalScrollBar()->maximum(), 0);

        auto *button = main->findChild<QPushButton *>("exclusionsExampleButton");
        QVERIFY(button);
        settings->ensureWidgetVisible(button);
        QTest::mouseClick(button, Qt::LeftButton);
        auto *dialog = main->findChild<QDialog *>("exclusionsExampleDialog");
        QVERIFY(dialog && dialog->isVisible());
        const auto *document = dialog->findChild<QTextBrowser *>("exclusionsExampleDocument");
        QVERIFY(document);
        QCoreApplication::processEvents();
        if (QGuiApplication::primaryScreen()->availableGeometry().height() >= 620) {
            QCOMPARE(document->verticalScrollBar()->maximum(), 0);
        }
        for (const char *text : {"基本同步", "附加同步规则", "排除名单", "同步结果", "*.pdb", "log*.txt", "log?.txt"}) {
            QVERIFY(document->toPlainText().contains(QString::fromUtf8(text)));
        }
        if (!screenshots.isEmpty()) QVERIFY(dialog->grab().save(screenshots + "/exclusions-example.png"));
        dialog->reject();
    }

    void incompletePdbWarningOnRealShare()
    {
#ifdef Q_OS_WIN
        QVERIFY(writeFile(source() + "/module.dll", "new DLL"));
        QVERIFY(writeFile(source() + "/module.pdb", "new symbols"));
        QVERIFY(writeFile(target() + "/module.pdb", "old"));
        QString unc = target();
        unc = "//localhost/" + unc.left(1) + "$" + unc.mid(2);
        auto originalWindow = window();
        AppConfig config = ConfigManager::load(configPath());
        config.hosts[0].sharePath = unc;
        config.selectedFileTypes = QStringList({"dll", "pdb"});
        QVERIFY(ConfigManager::save(configPath(), config));
        originalWindow.reset();
        MainWindow main(nullptr, configPath(), false);
        stopTimers(main);
        main.show();
        const auto symbol = QDir::toNativeSeparators(target() + "/module.pdb").toStdWString();
        const HANDLE lock = CreateFileW(symbol.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        QVERIFY(lock != INVALID_HANDLE_VALUE);
        struct FileLock {
            HANDLE handle;
            ~FileLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } guard{lock};

        auto *sync = main.findChild<QPushButton *>("syncButton");
        sync->click();
        QTRY_COMPARE_WITH_TIMEOUT(main.findChildren<QMessageBox *>().size(), 1, 15000);
        auto *warning = main.findChild<QMessageBox *>();
        QCOMPARE(warning->windowTitle(), QString("PDB 同步提醒"));
        QCOMPARE(warning->detailedText(), QString("module.pdb"));
        QCOMPARE(readFile(target() + "/module.dll"), QByteArray("new DLL"));
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(main), 15000);
        warning->close();
        QTRY_VERIFY(main.findChildren<QMessageBox *>().isEmpty());
        sync->click();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(main), 15000);
        QVERIFY(main.findChildren<QMessageBox *>().isEmpty());

        CloseHandle(lock);
        guard.handle = INVALID_HANDLE_VALUE;
        QCOMPARE(readFile(target() + "/module.pdb"), QByteArray("old"));
        sync->click();
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/module.pdb"), QByteArray("new symbols"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(main), 15000);
        QVERIFY(main.findChildren<QMessageBox *>().isEmpty());
#endif
    }

    void backgroundChecksPreserveCompletedDisplayAndManualChecksShowStages()
    {
        QVERIFY(writeFile(source() + "/app.exe", "initial"));
        auto main = window({}, true);
        main->m_backgroundTasksPaused = true;
        QTest::qWait(350);
        main->m_backgroundTasksPaused = false;
        main->m_config.hosts[0].sharePath = "//localhost/" + target().left(1) + "$" + target().mid(2);
        main->applyConfigToUi();
        main->syncNow();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        auto *table = main->findChild<QTableWidget *>("hostTable");
        auto *progress = main->m_rowWidgets.value("qa").progressBar;
        QVERIFY(table && progress);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("已同步"));
        QCOMPARE(progress->value(), 100);
        const QString completedStatus = table->item(0, 5)->text();
        const QString completedDetail = table->item(0, 6)->text();
        const QString completedTooltip = table->item(0, 6)->toolTip();
        QStringList statuses, details;
        int timerChecks = 0, timerRuns = 0;
        QObject displayObserver;
        connect(table->model(), &QAbstractItemModel::dataChanged, &displayObserver, [&](const QModelIndex &first, const QModelIndex &last) {
            if (first.row() <= 0 && last.row() >= 0 && first.column() <= 6 && last.column() >= 5) {
                statuses.append(table->item(0, 5)->text());
                details.append(table->item(0, 6)->text());
            }
        });
        QSignalSpy changedProgress(progress, &QProgressBar::valueChanged);
        for (int i = 0; i < 5; ++i) {
            main->pollForChanges();
            QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        }
        connect(main->m_pollTimer, &QTimer::timeout, &displayObserver, [&]() {
            ++timerChecks;
            if (!main->m_runningThreads.isEmpty()) ++timerRuns;
        });
        main->m_config.scanIntervalSeconds = 2;
        main->updateTimerInterval();
        main->updateSettingsSummary();
        main->m_pollTimer->start();
        QTest::qWait(6100);
        main->m_pollTimer->stop();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QVERIFY(timerChecks >= 3);
        QCOMPARE(timerRuns, timerChecks);
        qInfo() << "Real two-second UNC checks without display changes" << timerChecks;
        for (const QString &status : statuses) QCOMPARE(status, completedStatus);
        for (const QString &detail : details) QCOMPARE(detail, completedDetail);
        QCOMPARE(table->item(0, 6)->toolTip(), completedTooltip);
        QCOMPARE(changedProgress.size(), 0);
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            main->show();
            QVERIFY(QTest::qWaitForWindowExposed(main.get()));
            QVERIFY(main->grab().save(screenshots + "/background-completed.png"));
        }

        main->findChild<QPushButton *>("refreshButton")->click();
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("检测中"));
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QVERIFY(details.join('\n').contains(QStringLiteral("正在检查共享目录")));
        QVERIFY(!details.join('\n').contains(QStringLiteral("正在连接共享目录")));
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("完全一致"));

        statuses.clear();
        changedProgress.clear();
        QVERIFY(writeFile(source() + "/app.exe", "changed-longer-payload"));
        main->pollForChanges();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QVERIFY(statuses.contains(QStringLiteral("同步中")));
        QVERIFY(!statuses.contains(QStringLiteral("检测中")));
        QVERIFY(!statuses.contains(QStringLiteral("检查中")));
        QVERIFY(changedProgress.size() >= 2);
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("changed-longer-payload"));
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("已同步"));
        QCOMPARE(progress->value(), 100);
    }

    void backgroundChecksShowChangedDifferencesFailuresAndRecovery()
    {
        QVERIFY(writeFile(source() + "/app.exe", "missing"));
        QVERIFY(writeFile(target() + "/alpha.exe", "extra"));
        auto main = window();
        main->m_backgroundTasksPaused = true;
        QTest::qWait(350);
        main->m_backgroundTasksPaused = false;
        main->pollForChanges();
        auto *table = main->findChild<QTableWidget *>("hostTable");
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("检测中"));
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("不一致"));
        QVERIFY(table->item(0, 6)->toolTip().contains("alpha.exe"));
        QVERIFY(QFile::rename(target() + "/alpha.exe", target() + "/beta.exe"));
        main->pollForChanges();
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("不一致"));
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QVERIFY(table->item(0, 6)->toolTip().contains("beta.exe"));
        QVERIFY(!table->item(0, 6)->toolTip().contains("alpha.exe"));

        QVERIFY(QFile::remove(target() + "/beta.exe"));
        QCOMPARE(runWorker(source(), target(), {"exe"}, {}).state, int(SyncWorker::InSync));
        main->pollForChanges();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("完全一致"));

        main->m_releaseDirEdit->setEditText(source() + "-missing");
        main->pollForChanges();
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("检测中"));
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("本地目录无效"));
        main->m_releaseDirEdit->setEditText(source());
        main->pollForChanges();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("完全一致"));
        main->refreshHostTable();
        main->pollForChanges();
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("检测中"));
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("完全一致"));
    }

    void backgroundNoCopyRecoveryUpdatesCompletedProgress()
    {
#ifdef Q_OS_WIN
        QVERIFY(writeFile(source() + "/app.exe", "new-payload-for-copy"));
        QVERIFY(writeFile(target() + "/app.exe", "old"));
        struct LockedFile {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~LockedFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } locked;
        locked.handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(target() + "/app.exe").utf16()),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked.handle != INVALID_HANDLE_VALUE);
        auto main = window({}, true);
        main->m_backgroundTasksPaused = true;
        QTest::qWait(350);
        main->m_backgroundTasksPaused = false;
        main->syncNow();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        auto *table = main->findChild<QTableWidget *>("hostTable");
        auto *progress = main->m_rowWidgets.value("qa").progressBar;
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("同步失败"));
        QVERIFY(progress->value() < 100);
        CloseHandle(locked.handle);
        locked.handle = INVALID_HANDLE_VALUE;
        QCOMPARE(runWorker(source(), target(), {"exe"}, {}).state, int(SyncWorker::InSync));
        main->pollForChanges();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("已同步"));
        QCOMPARE(progress->value(), 100);
#endif
    }

    void oldBackgroundResultDoesNotOverwriteChangedConfiguration()
    {
        QVERIFY(writeFile(source() + "/app.exe", "initial"));
        auto main = window({}, true);
        main->m_backgroundTasksPaused = true;
        QTest::qWait(350);
        main->m_backgroundTasksPaused = false;
        main->syncNow();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        auto *table = main->findChild<QTableWidget *>("hostTable");
        const QString completed = table->item(0, 6)->text();
        QVERIFY(writeFile(target() + "/extra.exe", "extra"));
        main->pollForChanges();
        QVERIFY(!main->m_runningThreads.isEmpty());
        main->m_releaseDirEdit->setEditText(source() + "-missing");
        main->collectUiToConfig();
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 6)->text(), completed);
        main->pollForChanges();
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("检测中"));
        QTRY_VERIFY_WITH_TIMEOUT(main->m_runningThreads.isEmpty(), 15000);
        QCOMPARE(table->item(0, 5)->text(), QStringLiteral("本地目录无效"));
    }

    void shareConnectionHintsFollowRealConnectionAttempts()
    {
        Diagnostics::start(root() + "/diagnostics");
        HostEntry host;
        host.id = "qa-connection-hint";
        host.sharePath = "//localhost/" + target().left(1) + "$" + target().mid(2);
        QVERIFY(QFileInfo(host.sharePath).isDir());
        QStringList details;
        auto check = [&]() {
            SyncWorker worker(host, source(), {"exe"}, false);
            int finalState = -1;
            connect(&worker, &SyncWorker::stateChanged, [&](const QString &, int state, int, const QString &, const QString &detail, const QString &) {
                finalState = state;
                details.append(detail);
            });
            worker.process();
            return finalState;
        };
        QCOMPARE(check(), int(SyncWorker::InSync));
        QVERIFY(details.join('\n').contains(QStringLiteral("正在检查共享目录")));
        QVERIFY(!details.join('\n').contains(QStringLiteral("正在连接共享目录")));
        QVERIFY(!diagnosticLog().contains("sync.share_connecting session="));
        details.clear();
        host.sharePath = "//localhost/RemoteReleaseSync-QA-missing-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QCOMPARE(check(), int(SyncWorker::Disconnected));
        QVERIFY(details.join('\n').contains(QStringLiteral("正在连接共享目录")));
        QCOMPARE(diagnosticLog().count("sync.share_connecting session="), 1);
    }

    void unchangedPollingDoesNotRepeatSkippedLogs()
    {
        Diagnostics::start(root() + "/diagnostics");
        auto main = window();
        main->m_backgroundTasksPaused = true;
        QTest::qWait(350);
        main->m_config.hosts[0].enabled = false;
        main->applyConfigToUi();
        main->m_backgroundTasksPaused = false;
        main->pollForChanges();
        const QByteArray first = diagnosticLog();
        for (int i = 0; i < 1000; ++i) {
            main->statusBar()->clearMessage();
            main->pollForChanges();
        }
        const QByteArray repeated = diagnosticLog().mid(first.size());
        QVERIFY2(!repeated.contains("sync.skipped session="), qPrintable(QString::number(repeated.size())));
        QVERIFY(!repeated.contains("sync.background_state session="));
        QVERIFY(!repeated.contains("ui.status session="));
        main->syncNow();
        main->syncNow();
        QCOMPARE(diagnosticLog().mid(first.size()).count("sync.skipped session="), 2);
    }

    void unchangedBackgroundWorkersKeepManualAndActualCopyLogs()
    {
        Diagnostics::start(root() + "/diagnostics");
        QVERIFY(writeFile(source() + "/app.exe", "initial"));
        QCOMPARE(runWorker(source(), target(), {"exe"}, {}).state, int(SyncWorker::InSync));
        HostEntry host; host.id = "quiet-worker"; host.sharePath = target();
        auto poll = [&]() {
            SyncWorker worker(host, source(), {"exe"}, true);
            worker.setProperty("diagnosticsBackground", true);
            worker.setProperty("diagnosticsScope", configPath());
            QSignalSpy finished(&worker, &SyncWorker::finished);
            worker.process();
            return finished.size();
        };
        QCOMPARE(poll(), 1);
        const QByteArray first = diagnosticLog();
        for (int i = 0; i < 100; ++i) QCOMPARE(poll(), 1);
        QCOMPARE(diagnosticLog().mid(first.size()).size(), 0);
        QCOMPARE(runWorker(source(), target(), {"exe"}, {}).finished, 1);
        QCOMPARE(runWorker(source(), target(), {"exe"}, {}).finished, 1);
        QCOMPARE(diagnosticLog().mid(first.size()).count("sync.start session="), 2);
        QVERIFY(writeFile(source() + "/app.exe", "changed-longer-payload"));
        const int beforeCopy = diagnosticLog().size();
        QCOMPARE(poll(), 1);
        const QByteArray copied = diagnosticLog().mid(beforeCopy);
        QVERIFY(copied.contains("sync.start session="));
        QVERIFY(copied.contains("sync.robocopy_start session="));
        QVERIFY(copied.contains("sync.finished session="));
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("changed-longer-payload"));
        const int afterCopy = diagnosticLog().size();
        for (int i = 0; i < 20; ++i) QCOMPARE(poll(), 1);
        QCOMPARE(diagnosticLog().mid(afterCopy).size(), 0);
    }

    void repeatedBackgroundFailuresKeepChangesAndRecovery()
    {
        Diagnostics::start(root() + "/diagnostics");
        HostEntry host; host.id = "quiet-error"; host.password = "QuietLog-Secret";
        auto poll = [&]() {
            SyncWorker worker(host, source(), {"exe"}, true);
            worker.setProperty("diagnosticsBackground", true);
            worker.setProperty("diagnosticsScope", configPath());
            worker.process();
        };
        poll();
        const int first = diagnosticLog().size();
        for (int i = 0; i < 50; ++i) poll();
        QCOMPARE(diagnosticLog().mid(first).size(), 0);
        host.id = "another-host";
        poll();
        QCOMPARE(diagnosticLog().mid(first).count("sync.result session="), 1);
        host.id = "quiet-error";
        host.username = "changed-account";
        poll();
        const QByteArray changed = diagnosticLog().mid(first);
        QVERIFY(changed.contains("sync.background_repeat session="));
        QVERIFY(changed.contains("\"repeats\":50"));
        host.sharePath = target();
        poll();
        QVERIFY(diagnosticLog().contains("\"recovered\":true"));
        QVERIFY(!diagnosticLog().contains(host.password.toUtf8()));
        const int recovered = diagnosticLog().size();
        for (int i = 0; i < 20; ++i) poll();
        QCOMPARE(diagnosticLog().mid(recovered).size(), 0);
    }

    void repeatedBackgroundCopyFailuresKeepSuccessfulRetryLogs()
    {
#ifdef Q_OS_WIN
        Diagnostics::start(root() + "/diagnostics");
        QVERIFY(writeFile(source() + "/app.exe", "new-payload-for-copy"));
        const QString file = target() + "/app.exe";
        QVERIFY(writeFile(file, "old"));
        struct LockedFile {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~LockedFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } locked;
        locked.handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(file).utf16()), GENERIC_READ, 0,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        QVERIFY(locked.handle != INVALID_HANDLE_VALUE);
        HostEntry host; host.id = "locked-copy"; host.sharePath = target();
        auto poll = [&]() {
            SyncWorker worker(host, source(), {"exe"}, true);
            worker.setProperty("diagnosticsBackground", true);
            worker.setProperty("diagnosticsScope", configPath());
            Run result;
            connect(&worker, &SyncWorker::stateChanged, [&](const QString &, int state, int, const QString &, const QString &, const QString &) { result.state = state; });
            worker.process();
            return result.state;
        };
        QCOMPARE(poll(), int(SyncWorker::Error));
        const int first = diagnosticLog().size();
        for (int i = 0; i < 3; ++i) QCOMPARE(poll(), int(SyncWorker::Error));
        QCOMPARE(diagnosticLog().mid(first).size(), 0);
        CloseHandle(locked.handle);
        locked.handle = INVALID_HANDLE_VALUE;
        QCOMPARE(poll(), int(SyncWorker::InSync));
        const QByteArray recovered = diagnosticLog().mid(first);
        QVERIFY(recovered.contains("sync.robocopy_start session="));
        QVERIFY(recovered.contains("sync.finished session="));
        QVERIFY(recovered.contains("\"repeats\":3"));
        QVERIFY(recovered.contains("\"recovered\":true"));
        QCOMPARE(readFile(file), QByteArray("new-payload-for-copy"));
#endif
    }

    void backgroundMismatchChangesWithSameCountsAreLogged()
    {
        Diagnostics::start(root() + "/diagnostics");
        QVERIFY(writeFile(source() + "/first.exe", "not-copied"));
        HostEntry host; host.id = "stable-count"; host.sharePath = target();
        auto poll = [&]() {
            SyncWorker worker(host, source(), {"exe"}, false);
            worker.setProperty("diagnosticsBackground", true);
            worker.setProperty("diagnosticsScope", configPath());
            worker.process();
        };
        poll();
        const int first = diagnosticLog().size();
        poll();
        QCOMPARE(diagnosticLog().mid(first).size(), 0);
        QVERIFY(QFile::rename(source() + "/first.exe", source() + "/second.exe"));
        poll();
        QCOMPARE(diagnosticLog().mid(first).count("sync.background_state session="), 1);
        QVERIFY(!QFileInfo::exists(target() + "/second.exe"));
    }

    void backgroundErrorsUseRealFiveMinuteSummaryWindow()
    {
        if (!qEnvironmentVariableIsSet("SYNC_QA_LOG_LIVE")) QSKIP("Requires explicit five-minute log-window QA mode");
        Diagnostics::start(root() + "/diagnostics");
        const QString key = configPath() + "/five-minute";
        const QJsonObject failure{{"state", "Disconnected"}, {"detail", "QA connection failure"}};
        auto repeat = [&](const QString &run) { return Diagnostics::backgroundState(key, "same-configuration", failure, {{"host", "qa-summary"}, {"run", run}}, true); };
        QVERIFY(repeat("first"));
        QVERIFY(!repeat("repeat-1"));
        QVERIFY(!repeat("repeat-2"));
        const int before = diagnosticLog().size();
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::qWait(300100);
        QVERIFY(!repeat("repeat-3"));
        const QByteArray summary = diagnosticLog().mid(before);
        QCOMPARE(summary.count("sync.background_repeat session="), 1);
        QVERIFY(summary.contains("\"repeats\":3"));
        QVERIFY(summary.contains("\"reason\":\"interval\""));
        const int after = diagnosticLog().size();
        QVERIFY(!repeat("repeat-4"));
        QCOMPARE(diagnosticLog().size(), after);
        QVERIFY(Diagnostics::backgroundState(key, "same-configuration", {{"state", "InSync"}}, {{"host", "qa-summary"}}, false));
        QVERIFY(diagnosticLog().mid(after).contains("\"recovered\":true"));
        QVERIFY(diagnosticLog().mid(after).contains("\"repeats\":1"));
        qInfo() << "Real summary window elapsed ms" << elapsed.elapsed();
    }

    void realTwoSecondPollingRemainsQuietForOneMinute()
    {
        if (!qEnvironmentVariableIsSet("SYNC_QA_LOG_LIVE")) QSKIP("Requires explicit one-minute idle log QA mode");
        Diagnostics::start(root() + "/diagnostics");
        auto main = window({}, true);
        main->m_backgroundTasksPaused = true;
        QTest::qWait(350);
        main->m_config.hosts[0].enabled = false;
        main->applyConfigToUi();
        main->m_backgroundTasksPaused = false;
        main->pollForChanges();
        const int first = diagnosticLog().size();
        QSignalSpy polls(main->m_pollTimer, &QTimer::timeout);
        main->m_pollTimer->start(2000);
        QElapsedTimer elapsed;
        elapsed.start();
        QTest::qWait(60100);
        main->m_pollTimer->stop();
        QVERIFY(polls.size() >= 28);
        const QByteArray repeated = diagnosticLog().mid(first);
        QVERIFY(!repeated.contains("sync.skipped session="));
        QVERIFY(!repeated.contains("sync.background_state session="));
        QVERIFY(!repeated.contains("ui.status session="));
        qInfo() << "Actual idle timer checks" << polls.size() << "elapsed ms" << elapsed.elapsed();
    }

    void detailedInteractionLogsRedactSecretsAndKeepThreadOrder()
    {
        const QString root = m_temp->path() + "/diagnostics";
        Diagnostics::start(root);
        struct StopLog { ~StopLog() { Diagnostics::stop(); } } stop;
        const QString secret = QString::fromUtf8("NeverPersist-秘密-\\\"Token");
        Diagnostics::rememberSecret(secret);
        QDialog dialog;
        dialog.setWindowTitle("Interaction QA");
        auto *layout = new QVBoxLayout(&dialog);
        auto *normal = new QLineEdit(&dialog);
        normal->setObjectName("qaPathEdit");
        auto *password = new QLineEdit(&dialog);
        password->setObjectName("hostPasswordEdit");
        password->setEchoMode(QLineEdit::Password);
        auto *check = new QCheckBox("Record this click", &dialog);
        check->setObjectName("qaCheck");
        auto *combo = new QComboBox(&dialog);
        combo->setObjectName("qaCombo");
        combo->addItems({"first", "second"});
        auto *action = new QAction("QA action", &dialog);
        action->setObjectName("qaAction");
        layout->addWidget(normal); layout->addWidget(password); layout->addWidget(check); layout->addWidget(combo);
        dialog.show();
        Diagnostics::watch(&dialog);
        normal->setFocus();
        QTest::keyClicks(normal, "D:\\release");
        QTest::keyClick(normal, Qt::Key_Tab);
        password->setFocus();
        QTest::keyClicks(password, "HiddenPassword96");
        QTest::keyClick(password, Qt::Key_Tab);
        QTest::mouseClick(check, Qt::LeftButton);
        combo->setFocus();
        QTest::keyClick(combo, Qt::Key_Down);
        action->trigger();
        QMessageBox box(QMessageBox::Warning, "QA validation", "QA validation failed", QMessageBox::Ok, &dialog);
        QTimer::singleShot(0, &box, &QDialog::accept);
        box.exec();
        Diagnostics::event("qa.nested", {{"password", "NeverPersistNested"}, {"message", secret},
            {"url", "https://qa-user:qa-pass@example.invalid/?token=NeverPersistUrl"}});
        Diagnostics::record("qa.legacy", "legacy=" + secret);
        Diagnostics::event("qa.headers", {{"message", "Authorization: Bearer NeverPersistHeader\npassword=NeverPersistAssignment"}});
        qWarning().noquote() << "QA warning" << secret;
        std::vector<std::thread> writers;
        for (int i = 0; i < 4; ++i) writers.emplace_back([i]() {
            for (int n = 0; n < 25; ++n) Diagnostics::event("qa.concurrent", {{"writer", i}, {"n", n}});
        });
        for (auto &writer : writers) writer.join();
        Diagnostics::rememberSecret("\\");
        Diagnostics::event("qa.backslash", {{"path", "C:\\qa\\item"}, {"quote", "a\"b"}});
        QByteArray logs;
        for (const QString &name : QDir(root + "/Logs").entryList({"*.log"}, QDir::Files)) {
            QFile file(root + "/Logs/" + name);
            QVERIFY(file.open(QIODevice::ReadOnly));
            logs += file.readAll();
        }
        QVERIFY(logs.contains("ui.input_commit"));
        QVERIFY(logs.contains("qaPathEdit"));
        QVERIFY(logs.contains("ui.click"));
        QVERIFY(logs.contains("ui.combo_select"));
        QVERIFY(logs.contains("ui.action"));
        QVERIFY(logs.contains("ui.dialog_finished"));
        QVERIFY(logs.contains("QA validation failed"));
        QVERIFY(logs.contains("qt.message"));
        QVERIFY(logs.contains("[REDACTED]"));
        QVERIFY(!logs.contains(secret.toUtf8()));
        const QByteArray escaped = QJsonDocument(QJsonArray{secret}).toJson(QJsonDocument::Compact);
        QVERIFY(!logs.contains(escaped.mid(2, escaped.size() - 4)));
        QVERIFY(!logs.contains("HiddenPassword96"));
        QVERIFY(!logs.contains("NeverPersistNested"));
        QVERIFY(!logs.contains("NeverPersistUrl"));
        QVERIFY(!logs.contains("NeverPersistHeader"));
        QVERIFY(!logs.contains("NeverPersistAssignment"));
        QVERIFY(!logs.contains("qa-user:qa-pass"));
        QSet<quint64> sequences;
        int concurrent = 0;
        for (const QByteArray &line : logs.split('\n')) {
            if (line.trimmed().isEmpty()) continue;
            const auto match = QRegularExpression("seq=(\\d+)").match(QString::fromUtf8(line));
            QVERIFY(match.hasMatch());
            const quint64 value = match.captured(1).toULongLong();
            QVERIFY(!sequences.contains(value));
            sequences.insert(value);
            if (line.contains("qa.concurrent")) ++concurrent;
            if (line.contains("qa.backslash")) {
                const auto object = QJsonDocument::fromJson(line.mid(line.indexOf('{'))).object();
                QVERIFY(!object.isEmpty());
                QCOMPARE(object.value("quote").toString(), QString("a\"b"));
            }
        }
        QCOMPARE(concurrent, 100);
    }

    void networkDiscoveryHandlesLoopbackCancellationAndSelection()
    {
        struct RestoreProxy {
            QNetworkProxy proxy;
            bool system;
            ~RestoreProxy() { QNetworkProxy::setApplicationProxy(proxy); QNetworkProxyFactory::setUseSystemConfiguration(system); }
        } restore{QNetworkProxy::applicationProxy(), QNetworkProxyFactory::usesSystemConfiguration()};
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 1));
        NetworkHostDialog dialog(nullptr, false);
        std::atomic<bool> wrongThread{false};
        connect(dialog.m_table->model(), &QAbstractItemModel::dataChanged, &dialog, [&dialog, &wrongThread]() {
            if (QThread::currentThread() != dialog.thread()) wrongThread.store(true);
        }, Qt::DirectConnection);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QTest::qWait(50);
        dialog.m_generation = 1;
        dialog.m_operationId = "qa-network";
        dialog.m_elapsed.start();
        dialog.m_cancelled.store(false);
        dialog.m_scanning = true;
        dialog.m_total = 2;
        dialog.m_ranges.append({QHostAddress("127.0.0.1"), 0x7f000001, 0x7f000002, "QA loopback"});
        dialog.pump();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.m_inFlight, 0, 5000);
        QVERIFY(dialog.m_table->rowCount() >= 1);
        bool localShareFound = false;
        for (int row = 0; row < dialog.m_table->rowCount(); ++row) {
            if (dialog.m_table->item(row, 1)->text() == "127.0.0.1")
                localShareFound = dialog.m_table->item(row, 2)->text() == QStringLiteral("可连接");
        }
        QVERIFY(localShareFound);
        dialog.m_table->selectRow(0);
        QVERIFY(!dialog.selectedServer().isEmpty());
        QVERIFY(dialog.m_scanButton->isEnabled());
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) QVERIFY(dialog.grab().save(screenshots + "/network-discovery.png"));
        dialog.m_ranges = {{QHostAddress("127.0.0.1"), 0x7f000001, 0x7f0000ff, "QA loopback"}};
        dialog.m_rangeIndex = 0;
        dialog.m_completed = 0;
        dialog.m_total = 255;
        dialog.m_scanning = true;
        dialog.m_loggedFinished = false;
        dialog.pump();
        dialog.cancelScan();
        QTRY_COMPARE_WITH_TIMEOUT(dialog.m_inFlight, 0, 5000);
        QVERIFY(dialog.m_cancelled.load());
        QVERIFY(dialog.m_completed < dialog.m_total);
        QVERIFY(dialog.m_scanButton->isEnabled());
        QVERIFY(!wrongThread.load());
    }

    void scanResultFiltersAndShareBrowsingPreserveSelectionSafety()
    {
        NetworkHostDialog dialog(nullptr, false);
        auto *tabs = dialog.m_adapterTabs;
        tabs->setTabData(tabs->addTab("全部"), -1);
        tabs->setTabData(tabs->addTab("网卡 A"), 1);
        tabs->setTabData(tabs->addTab("网卡 B"), 2);
        tabs->setCurrentIndex(0);
        dialog.setConfiguredServers({"DESKTOP-QA.", "fe80--1s2.ipv6-literal.net"});
        const int named = dialog.insertHost({QHostAddress("192.0.2.1"), {}, "网卡 A", 1}, true);
        const int unnamed = dialog.insertHost({QHostAddress("192.0.2.2"), {}, "网卡 A", 1}, false);
        const int ipv6 = dialog.insertHost({QHostAddress("fe80::1%2"), {}, "网卡 B", 2}, true);
        dialog.setResolvedName(named, "desktop-qa");
        dialog.setResolvedName(named, "desktop-qa.example");
        QCOMPARE(dialog.m_table->item(named, 4)->text(), QString("已配置"));
        QCOMPARE(dialog.m_table->item(ipv6, 4)->text(), QString("已配置"));
        dialog.m_table->item(unnamed, 0)->setData(NetworkHostDialog::DnsPending, true);
        dialog.m_hideUnnamed->setChecked(true);
        QVERIFY(!dialog.m_table->isRowHidden(unnamed));
        dialog.finishNameLookup(unnamed, NetworkHostDialog::DnsPending);
        dialog.finishNameLookup(ipv6, NetworkHostDialog::DnsPending);
        QVERIFY(dialog.m_table->isRowHidden(unnamed));
        QVERIFY(!dialog.m_table->isRowHidden(ipv6));
        dialog.m_onlySmb->setChecked(true);
        dialog.m_onlyConfigured->setChecked(true);
        dialog.m_table->selectRow(named);
        QCOMPARE(dialog.selectedServer(), QString("desktop-qa.example"));
        dialog.m_search->setText("FE80");
        QVERIFY(dialog.selectedServer().isEmpty());
        QVERIFY(!dialog.m_selectButton->isEnabled());
        QCOMPARE(dialog.m_visibleCount, 1);
        tabs->setCurrentIndex(1);
        QCOMPARE(dialog.m_visibleCount, 0);
        dialog.m_search->setText("TOP-QA");
        QCOMPARE(dialog.m_visibleCount, 1);
        QSignalSpy browse(&dialog, &NetworkHostDialog::browseSharesRequested);
        QSignalSpy accepted(&dialog, &QDialog::accepted);
        dialog.browseShares(named, 1);
        dialog.browseShares(ipv6, 0);
        QCOMPARE(browse.count(), 0);
        dialog.browseShares(named, 0);
        QCOMPARE(browse.count(), 1);
        QCOMPARE(browse.takeFirst().at(0).toString(), QStringLiteral("\\\\desktop-qa.example\\"));
        tabs->setCurrentIndex(2);
        dialog.m_search->clear();
        dialog.browseShares(ipv6, 0);
        QCOMPARE(browse.count(), 1);
        QCOMPARE(browse.takeFirst().at(0).toString(), QStringLiteral("\\\\fe80--1s2.ipv6-literal.net\\"));
        QCOMPARE(accepted.count(), 0);
        dialog.m_table->selectRow(ipv6);
        dialog.m_selectButton->click();
        QCOMPARE(accepted.count(), 1);
    }

    void networkDiscoveryFindsExplicitlyAuthorizedHost()
    {
        const QString server = qEnvironmentVariable("SYNC_QA_AUTHORIZED_REMOTE");
        if (server.isEmpty()) QSKIP("Requires an explicitly authorized real network target");
        const QString artifacts = qEnvironmentVariable("SYNC_QA_REMOTE_ARTIFACT_DIR");
        const QHostAddress expected(qEnvironmentVariable("SYNC_QA_REMOTE_ADDRESS"));
        QVERIFY(!expected.isNull() && QDir::isAbsolutePath(artifacts));
        QVERIFY(QDir().mkpath(artifacts));
        NetworkHostDialog dialog;
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QTRY_VERIFY_WITH_TIMEOUT(!dialog.m_scanning && dialog.m_inFlight == 0, 180000);
        QTest::qWait(5500);
        dialog.m_adapterTabs->setCurrentIndex(0);
        int selectedRow = -1;
        for (int row = 0; row < dialog.m_table->rowCount(); ++row) {
            if (QHostAddress(dialog.m_table->item(row, 1)->text()) == expected) { selectedRow = row; break; }
        }
        QJsonObject result{{"server", server}, {"expected_ip", expected.toString()},
            {"interfaces", dialog.m_interfaces->text()}, {"total", double(dialog.m_total)},
            {"checked", double(dialog.m_completed)}, {"found", dialog.m_table->rowCount()}, {"target_found", selectedRow >= 0}};
        if (selectedRow >= 0) {
            dialog.m_table->selectRow(selectedRow);
            result.insert("target_name", dialog.m_table->item(selectedRow, 0)->text());
            result.insert("target_smb", dialog.m_table->item(selectedRow, 2)->text());
            result.insert("selected_server", dialog.selectedServer());
        }
        QVERIFY(writeFile(artifacts + "/network-result.json", QJsonDocument(result).toJson(QJsonDocument::Indented)));
        QVERIFY(dialog.grab().save(artifacts + "/network-result.png"));
        QVERIFY(!dialog.m_cancelled.load());
        QCOMPARE(dialog.m_completed, dialog.m_total);
        QVERIFY(selectedRow >= 0);
        QCOMPARE(dialog.m_table->item(selectedRow, 2)->text(), QStringLiteral("可连接"));
        QCOMPARE(dialog.selectedServer().toUpper(), server.toUpper());
    }

    void hostnameHelpContainsImageAndFitsTheScreen()
    {
        auto main = window();
        main->show();
        bool guidance = false, image = false, fits = false, captured = true;
        QTimer::singleShot(50, main.get(), [&]() {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *browser = dialog->findChild<QTextBrowser *>()) {
                const QString text = browser->toPlainText();
                guidance = text.contains("Windows 10") && text.contains("Windows 11") && text.contains("hostname") && text.contains("DESKTOP-U6HE6A0");
                const QVariant resource = browser->document()->resource(QTextDocument::ImageResource, QUrl(":/hostname-guide.png"));
                image = resource.isValid() && !resource.isNull();
                fits = dialog->height() <= QGuiApplication::primaryScreen()->availableGeometry().height();
                const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
                if (!screenshots.isEmpty()) captured = dialog->grab().save(screenshots + "/hostname-help.png");
            }
            dialog->reject();
        });
        QTimer::singleShot(2000, main.get(), []() {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        main->showHostnameHelp();
        QVERIFY(guidance);
        QVERIFY(image);
        QVERIFY(fits);
        QVERIFY(captured);
    }

    void remoteTargetChangesAndInvalidConfigurationNeverLaunchDeployment()
    {
        RemoteDebugger dialog;
        HostEntry first, second;
        first.id = "first"; first.name = "First"; first.sharePath = "invalid-local-path"; first.username = "qa-admin";
        second = first; second.id = "second"; second.name = "Second";
        dialog.setHosts({first, second}, 1);
        auto *combo = dialog.findChild<QComboBox *>("remoteHostCombo");
        QVERIFY(combo);
        auto *startupUser = dialog.findChild<QLineEdit *>("startupUserEdit");
        auto *stopLegacy = dialog.findChild<QCheckBox *>("stopLegacyStartupCheck");
        QVERIFY(startupUser && stopLegacy);
        startupUser->setText("qa-desktop");
        stopLegacy->setChecked(true);
        auto *credentialHint = dialog.findChild<QLabel *>("credentialHint");
        QVERIFY(credentialHint && credentialHint->text().contains("密码为空"));
        QCOMPARE(combo->currentIndex(), 1);
        second.username = "changed-user";
        dialog.setHosts({first, second}, -1);
        QCOMPARE(combo->currentIndex(), 1);
        QCOMPARE(dialog.m_hosts.at(1).username, QString("changed-user"));
        QCOMPARE(startupUser->text(), QString("qa-desktop"));
        dialog.deploy("enable");
        QVERIFY(!dialog.isDeploying());
        QVERIFY(dialog.findChild<QLabel *>("remoteStatus")->text().contains("无效"));
        dialog.setHosts({first}, -1);
        QCOMPARE(combo->currentIndex(), -1);
        QVERIFY(startupUser->text().isEmpty());
        QVERIFY(!stopLegacy->isChecked());
        QVERIFY(!dialog.findChild<QPushButton *>("installRemoteButton")->isEnabled());
        first.sharePath = QStringLiteral("//reserved.invalid/share");
        first.username.clear();
        dialog.setHosts({first}, 0);
        QVERIFY(credentialHint->text().contains("尚未填写"));
        for (const char *button : {"installRemoteButton", "enableRemoteStartupButton", "disableRemoteStartupButton"}) {
            dialog.findChild<QPushButton *>(button)->click();
            QVERIFY(!dialog.isDeploying());
            QVERIFY(dialog.findChild<QLabel *>("remoteStatus")->text().contains("尚未填写远程管理用户名"));
        }
        first.username = "test";
        first.password = "test-only";
        dialog.setHosts({first}, 0);
        QVERIFY(credentialHint->isHidden());
        dialog.deploy("enable");
        QVERIFY(!dialog.isDeploying());
        QVERIFY(dialog.findChild<QLabel *>("remoteStatus")->text().contains("远端 Windows 桌面的账号"));
    }

    void deploymentOnExplicitlyAuthorizedSavedRemoteHost()
    {
        const QString server = qEnvironmentVariable("SYNC_QA_AUTHORIZED_REMOTE");
        const QString operation = qEnvironmentVariable("SYNC_QA_REMOTE_OPERATION");
        if (server.isEmpty() || operation.isEmpty()) QSKIP("Requires an explicitly authorized real host and operation");
        Diagnostics::start(root() + "/diagnostics");
        QVERIFY(QStringList({"install", "enable", "disable"}).contains(operation));
        const QString savedConfig = qEnvironmentVariable("SYNC_QA_SAVED_CONFIG");
        const QString guardScript = qEnvironmentVariable("SYNC_QA_REMOTE_STATE_SCRIPT");
        const QString artifacts = qEnvironmentVariable("SYNC_QA_REMOTE_ARTIFACT_DIR");
        QVERIFY(QFileInfo(savedConfig).isFile() && QFileInfo(guardScript).isFile() && QDir::isAbsolutePath(artifacts));
        QVERIFY(QDir().mkpath(artifacts));
        const AppConfig config = ConfigManager::load(savedConfig);
        HostEntry selected;
        bool found = false;
        for (const auto &host : config.hosts) {
            const QString path = QString(host.sharePath).replace('/', '\\');
            if (path.startsWith("\\\\" + server + '\\', Qt::CaseInsensitive)) { selected = host; found = true; break; }
        }
        QVERIFY(found);
        const QString powershell = qEnvironmentVariable("SystemRoot") + "/System32/WindowsPowerShell/v1.0/powershell.exe";
        QVERIFY(QFileInfo(powershell).isFile());
        auto taskState = [&](const QString &action, bool enabled) {
            QProcess process;
            process.start(powershell, {"-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", guardScript,
                "-Action", action, "-Enabled", enabled ? "true" : "false"});
            if (!process.waitForStarted(5000) || !process.waitForFinished(90000)) return QJsonObject();
            return QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        };
        const auto before = taskState("inspect", false);
        QVERIFY(before.value("success").toBool() && before.value("enabled").isBool());
        const bool originalEnabled = before.value("enabled").toBool();
        bool restored = false;
        struct RestoreStartup { std::function<void()> restore; ~RestoreStartup() { restore(); } } restore{[&]() {
            if (restored) return;
            const auto state = taskState("restore", originalEnabled);
            if (!state.value("success").toBool()) QTest::qFail("Unable to restore the authorized host's original startup flag", __FILE__, __LINE__);
        }};
        RemoteDebugger dialog;
        dialog.setHosts({selected}, 0);
        if (operation != "disable") {
            dialog.findChild<QLineEdit *>("startupUserEdit")->setText(
                qEnvironmentVariable("SYNC_QA_REMOTE_DESKTOP_USER", selected.username));
        }
        auto *versions = dialog.findChild<QComboBox *>("debuggerVersion");
        auto *architectures = dialog.findChild<QComboBox *>("debuggerArchitecture");
        QVERIFY(versions && architectures);
        const int version = versions->findData(18);
        const int architecture = architectures->findText("x64");
        QVERIFY(version >= 0 && architecture >= 0);
        versions->setCurrentIndex(version);
        architectures->setCurrentIndex(architecture);
        const QString installerKey = "remoteTools/installers/VS_RemoteTools_2026_x64";
        struct RestoreInstaller { QString key; bool had; QVariant value; ~RestoreInstaller() { QSettings s; if (had) s.setValue(key, value); else s.remove(key); } }
            setting{installerKey, QSettings().contains(installerKey), QSettings().value(installerKey)};
        if (operation == "install") {
            const QString installer = qEnvironmentVariable("SYNC_QA_REMOTE_INSTALLER");
            QVERIFY(QFileInfo(installer).isFile());
            QSettings().setValue(installerKey, installer);
        }
        const QString buttonName = operation == "install" ? "installRemoteButton"
            : operation == "enable" ? "enableRemoteStartupButton" : "disableRemoteStartupButton";
        auto *button = dialog.findChild<QPushButton *>(buttonName);
        QVERIFY(button && button->isEnabled());
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QJsonObject response;
        QSignalSpy finished(&dialog, &RemoteDebugger::deploymentFinished);
        for (int attempt = 0; attempt < 2; ++attempt) {
            response = {};
            finished.clear();
            const int logOffset = diagnosticLog().size();
            QTest::mouseClick(button, Qt::LeftButton);
            QVERIFY(dialog.isDeploying());
            QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 1250000);
            for (const QByteArray &line : diagnosticLog().mid(logOffset).split('\n')) {
                if (!line.contains("remote.result session=")) continue;
                const auto item = QJsonDocument::fromJson(line.mid(line.indexOf('{'))).object();
                response = item.value("response").toObject();
            }
            QVERIFY(response.value("success").isBool());
            if (!response.value("message").toString().contains("请再次点击")) break;
        }
        const auto after = taskState("inspect", originalEnabled);
        QJsonObject result{{"server", server}, {"operation", operation}, {"before", before}, {"after", after}, {"response", response}};
        QVERIFY(writeFile(artifacts + '/' + operation + "-result.json", QJsonDocument(result).toJson(QJsonDocument::Indented)));
        QVERIFY(dialog.grab().save(artifacts + '/' + operation + "-result.png"));
        const auto restoredState = taskState("restore", originalEnabled);
        restored = restoredState.value("success").toBool() && restoredState.value("enabled").toBool() == originalEnabled;
        result.insert("restored", restoredState);
        QVERIFY(writeFile(artifacts + '/' + operation + "-result.json", QJsonDocument(result).toJson(QJsonDocument::Indented)));
        QVERIFY(restored);
        QVERIFY2(response.value("success").toBool(), qPrintable(response.value("message").toString()));
        QVERIFY(after.value("success").toBool());
        QCOMPARE(after.value("enabled").toBool(), operation != "disable");
    }

    void remoteDeploymentButtonsLaunchRealScript_data()
    {
        QTest::addColumn<QString>("buttonName");
        QTest::addColumn<bool>("unicodePath");
        for (const QString &button : {QString("installRemoteButton"), QString("enableRemoteStartupButton"), QString("disableRemoteStartupButton")}) {
            QTest::newRow(qPrintable(button + "-ascii")) << button << false;
            QTest::newRow(qPrintable(button + "-unicode")) << button << true;
        }
    }

    void remoteDeploymentButtonsLaunchRealScript()
    {
        QFETCH(QString, buttonName);
        QFETCH(bool, unicodePath);
        struct RestoreEnvironment {
            QByteArray temp = qgetenv("TEMP"), tmp = qgetenv("TMP");
            ~RestoreEnvironment() { if (temp.isNull()) qunsetenv("TEMP"); else qputenv("TEMP", temp); if (tmp.isNull()) qunsetenv("TMP"); else qputenv("TMP", tmp); }
        } environment;
        const QString temporary = root() + (unicodePath ? "/中文 临时'目录" : "/temporary");
        QVERIFY(QDir().mkpath(temporary));
        QVERIFY(qputenv("TEMP", QDir::toNativeSeparators(temporary).toLocal8Bit()));
        QVERIFY(qputenv("TMP", QDir::toNativeSeparators(temporary).toLocal8Bit()));
        QCOMPARE(QDir::cleanPath(QDir::tempPath()), QDir::cleanPath(temporary));
        RemoteDebugger dialog;
        HostEntry host;
        host.id = "qa-local-launch"; host.name = "本机脚本启动验证";
        host.sharePath = "//127.0.0.1/QA-unused"; host.username = "qa-unused"; host.password = "QA-stdin-only-秘密";
        dialog.setHosts({host}, 0);
        dialog.findChild<QLineEdit *>("startupUserEdit")->setText("qa-desktop");
        auto *versions = dialog.findChild<QComboBox *>("debuggerVersion");
        auto *architectures = dialog.findChild<QComboBox *>("debuggerArchitecture");
        QVERIFY(versions && architectures);
        // 无效版本在脚本连接主机前被拒绝，可安全验证真实按钮和完整的本机进程链路。
        versions->setItemData(versions->currentIndex(), 0);
        const QString settingKey = "remoteTools/installers/VS_RemoteTools_" + versions->currentText().section(' ', -1) + '_' + architectures->currentText();
        struct RestoreSetting {
            QString key; bool present; QVariant value;
            ~RestoreSetting() { QSettings settings; if (present) settings.setValue(key, value); else settings.remove(key); }
        } setting{settingKey, QSettings().contains(settingKey), QSettings().value(settingKey)};
        const QString installer = root() + "/QA-unused.exe";
        QVERIFY(writeFile(installer, "not executed"));
        QSettings().setValue(settingKey, installer);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QSignalSpy finished(&dialog, &RemoteDebugger::deploymentFinished);
        auto *button = dialog.findChild<QPushButton *>(buttonName);
        QVERIFY(button && button->isEnabled());
        QTest::mouseClick(button, Qt::LeftButton);
        QVERIFY(dialog.isDeploying());
        QVERIFY(!dialog.m_deployment->arguments().join(' ').contains(host.password));
        const auto artifacts = QDir(temporary).entryList({"rrs-deploy-*"}, QDir::AllEntries | QDir::NoDotAndDotDot);
        QCOMPARE(artifacts.size(), 1);
        const QString artifact = QDir(temporary).filePath(artifacts.first());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        QVERIFY(!dialog.isDeploying());
        QVERIFY(!QFileInfo::exists(artifact));
        const QString status = dialog.findChild<QLabel *>("remoteStatus")->text();
        QVERIFY2(status.contains("版本或架构无效"), qPrintable(status));
        QVERIFY(!status.contains(QChar::ReplacementCharacter));
        QVERIFY(!status.contains("CommandNotFoundException"));
        QVERIFY(!status.contains("远程任务可能仍在执行"));
        QVERIFY(button->isEnabled());
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) { QVERIFY(QDir().mkpath(screenshots)); QVERIFY(dialog.grab().save(screenshots + '/' + buttonName + (unicodePath ? "-unicode.png" : "-ascii.png"))); }
    }

    void remoteDeploymentLocalFailuresRemainReadable_data()
    {
        QTest::addColumn<QString>("scenario");
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("expected");
        QTest::addColumn<bool>("hasResult");
        QTest::newRow("missing-file") << QString("missing") << QByteArray() << QString("本机部署脚本启动失败，未连接远程主机") << true;
        QTest::newRow("locked-file") << QString("locked") << QByteArray("throw 'must not execute'") << QString("本机部署脚本启动失败，未连接远程主机") << true;
        QTest::newRow("controller-exception") << QString("normal") << QString("throw '中文控制脚本异常'").toUtf8() << QString("中文控制脚本异常") << true;
        QTest::newRow("stderr-without-result") << QString("normal") << QString("[Console]::Error.Write('中文错误，不带换行'); exit 23").toUtf8() << QString("中文错误，不带换行") << false;
        QTest::newRow("remote-stage-without-result") << QString("remote") << QByteArray("[Console]::WriteLine('{\"type\":\"progress\",\"remote_contact\":true,\"message\":\"QA remote stage\"}'); exit 19") << QString("已进入远程连接阶段，执行状态未知") << false;
        QTest::newRow("result-without-newline") << QString("normal") << QString("[Console]::Write('{\"type\":\"result\",\"success\":true,\"message\":\"中文回执，不带换行\"}')").toUtf8() << QString("中文回执，不带换行") << true;
        QTest::newRow("authentication-result-with-stage") << QString("remote") << QString(
            "[Console]::WriteLine('{\"type\":\"progress\",\"remote_contact\":true,\"message\":\"验证管理凭据\"}')\n"
            "[Console]::WriteLine('{\"type\":\"result\",\"success\":false,\"message\":\"拒绝访问\"}')").toUtf8()
            << QString("拒绝访问\n最后报告的进度：验证管理凭据") << true;
    }

    void remoteDeploymentLocalFailuresRemainReadable()
    {
        QFETCH(QString, scenario);
        QFETCH(QByteArray, contents);
        QFETCH(QString, expected);
        QFETCH(bool, hasResult);
        RemoteDebugger dialog;
        dialog.m_deploymentId = "qa-local-failure";
        dialog.m_deploymentElapsed.start();
        dialog.m_deploymentDirectory = std::make_unique<QTemporaryDir>(root() + "/中文 启动'诊断-XXXXXX");
        QVERIFY(dialog.m_deploymentDirectory->isValid());
        const QString directory = dialog.m_deploymentDirectory->path();
        QString path = directory + "/controller.ps1";
        QFile locked;
        if (scenario == "locked") {
            path = root() + "/中文 被占用'脚本.ps1";
            locked.setFileName(path);
            QVERIFY(locked.open(QIODevice::ReadWrite));
            QCOMPARE(locked.write(QByteArray::fromHex("efbbbf") + contents), qint64(contents.size() + 3));
            QVERIFY(locked.flush());
        } else if (scenario != "missing") {
            QVERIFY(writeFile(path, QByteArray::fromHex("efbbbf") + contents));
        }
        QSignalSpy finished(&dialog, &RemoteDebugger::deploymentFinished);
        dialog.startDeploymentProcess(path, "{\"operation\":\"qa-unused\"}");
        QVERIFY(dialog.isDeploying());
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 15000);
        locked.close();
        QVERIFY(!dialog.isDeploying());
        QVERIFY(!QFileInfo::exists(directory));
        QCOMPARE(dialog.m_deploymentHasResult, hasResult);
        QCOMPARE(dialog.m_remoteContactStarted, scenario == "remote");
        const QString status = dialog.findChild<QLabel *>("remoteStatus")->text();
        QVERIFY2(status.contains(expected), qPrintable(status));
        QVERIFY(!status.contains(QChar::ReplacementCharacter));
        QVERIFY(!status.contains("????"));
        QVERIFY(!status.contains("临时任务或文件可能未完成清理"));
        if (!hasResult && scenario != "remote") QVERIFY(status.contains("未进入远程连接阶段"));
    }

    void remoteDeploymentMissingPowerShellCleansPreparedDirectory()
    {
        RemoteDebugger dialog;
        dialog.m_deploymentDirectory = std::make_unique<QTemporaryDir>(root() + "/missing-powershell-XXXXXX");
        QVERIFY(dialog.m_deploymentDirectory->isValid());
        const QString directory = dialog.m_deploymentDirectory->path();
        const QString path = directory + "/controller.ps1";
        QVERIFY(writeFile(path, "not executed"));
        const QByteArray systemRoot = qgetenv("SystemRoot");
        QVERIFY(qputenv("SystemRoot", root().toLocal8Bit()));
        dialog.startDeploymentProcess(path, "{}");
        if (systemRoot.isNull()) qunsetenv("SystemRoot"); else qputenv("SystemRoot", systemRoot);
        QVERIFY(!dialog.isDeploying());
        QVERIFY(!QFileInfo::exists(directory));
        QVERIFY(dialog.findChild<QLabel *>("remoteStatus")->text().contains("未找到 Windows PowerShell"));
    }

    void logRetentionRemovesOnlyManagedFiles()
    {
        const QString dir = root() + "/test logs";
        QVERIFY(writeFile(dir + "/synctool-2000-01-01.log"));
        QVERIFY(writeFile(dir + "/synctool-2000-01-01.1.log"));
        QVERIFY(writeFile(dir + "/synctool-2000-01-01.unrelated.log"));
        QVERIFY(writeFile(dir + "/unrelated.log"));
        const QString today = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
        QVERIFY(writeFile(dir + "/synctool-" + today + ".log"));
        Diagnostics::maintainLogs(dir, 0);
        QVERIFY(QFileInfo::exists(dir + "/synctool-2000-01-01.log"));
        Diagnostics::maintainLogs(dir, 30);
        QVERIFY(!QFileInfo::exists(dir + "/synctool-2000-01-01.log"));
        QVERIFY(!QFileInfo::exists(dir + "/synctool-2000-01-01.1.log"));
        QVERIFY(QFileInfo::exists(dir + "/synctool-2000-01-01.unrelated.log"));
        QVERIFY(QFileInfo::exists(dir + "/unrelated.log"));
        QVERIFY(QFileInfo::exists(dir + "/synctool-" + today + ".log"));
        AppConfig config;
        config.logRetentionDays = 14;
        QVERIFY(ConfigManager::save(configPath(), config));
        QCOMPARE(ConfigManager::load(configPath()).logRetentionDays, 14);
    }

    void debuggerDiscoveryAndClipboard()
    {
        if (!qEnvironmentVariableIsSet("SYNC_QA_DEBUGGER_INSTALLED")) QSKIP("Requires installed Visual Studio Remote Debugger files");
        RemoteDebugger dialog;
        dialog.show();
        auto *table = dialog.findChild<QTableWidget *>("debuggerPaths");
        QTRY_VERIFY_WITH_TIMEOUT(table->rowCount() >= 2, 10000);
        QStringList architectures;
        for (int row = 0; row < table->rowCount(); ++row) {
            QVERIFY(QFileInfo::exists(table->item(row, 3)->text()));
            QVERIFY(!table->item(row, 1)->text().isEmpty());
            architectures.append(table->item(row, 2)->text());
        }
        QVERIFY(architectures.contains("x86"));
        QVERIFY(architectures.contains("x64"));
        table->item(0, 0)->setCheckState(Qt::Checked);
        const QString exe = table->item(0, 3)->text();
        const QString folder = QFileInfo(exe).absolutePath();
        {
            QDesktopServices::setUrlHandler("file", this, "recordOpenedDirectory");
            struct ResetUrlHandler {
                ~ResetUrlHandler() { QDesktopServices::unsetUrlHandler("file"); }
            } guard;
            m_openedDirectory = QUrl();
            QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                              table->visualItemRect(table->item(0, 3)).center());
            QCOMPARE(m_openedDirectory, QUrl::fromLocalFile(folder));
            m_openedDirectory = QUrl();
            table->setFocus();
            QTest::keyClick(table, Qt::Key_Return);
            QCOMPARE(m_openedDirectory, QUrl::fromLocalFile(folder));
            QCOMPARE(table->item(0, 0)->checkState(), Qt::Checked);
        }
        dialog.findChild<QPushButton *>("copyPathsButton")->click();
        QCOMPARE(QApplication::clipboard()->text(), exe);
        dialog.findChild<QPushButton *>("copyFoldersButton")->click();
        QCOMPARE(QApplication::clipboard()->mimeData()->urls(), QList<QUrl>({QUrl::fromLocalFile(folder)}));
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == "windows") {
            QVERIFY(IsClipboardFormatAvailable(CF_HDROP));
            QVERIFY(OpenClipboard(nullptr));
            const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
            wchar_t folderPath[32768] = {};
            const UINT count = drop ? DragQueryFileW(drop, 0xffffffff, nullptr, 0) : 0;
            const UINT length = drop ? DragQueryFileW(drop, 0, folderPath, 32768) : 0;
            CloseClipboard();
            QCOMPARE(count, UINT(1));
            QVERIFY(length > 0);
            QCOMPARE(QDir::fromNativeSeparators(QString::fromWCharArray(folderPath)), folder);
        }
#endif
    }

    void debuggerArgumentsAreCopyable()
    {
        RemoteDebugger dialog;
        dialog.show();
        QTRY_VERIFY(dialog.findChild<QPushButton *>("rescanButton")->isEnabled());
        const auto *edit = dialog.findChild<QLineEdit *>("argumentsEdit");
        auto *copy = dialog.findChild<QPushButton *>("copyArgumentsButton");
        auto *scroll = dialog.findChild<QScrollArea *>("debuggerScroll");
        QVERIFY(edit && edit->isReadOnly() && copy && scroll);
        scroll->ensureWidgetVisible(copy);
        const QString expected("/noauth /anyuser /nosecuritywarn /port 4026 /timeout 999999999");
        QCOMPARE(edit->text(), expected);
        QTest::mouseClick(copy, Qt::LeftButton);
        for (int retry = 0; retry < 3 && QApplication::clipboard()->text() != expected; ++retry) {
            QTest::qWait(100);
            QTest::mouseClick(copy, Qt::LeftButton);
        }
        QCOMPARE(QApplication::clipboard()->text(), expected);
        const auto *details = dialog.findChild<QLabel *>("argumentDetails");
        QVERIFY(details && details->text().contains("999999999 秒"));
        QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            scroll->verticalScrollBar()->setValue(0);
            QVERIFY(dialog.grab().save(screenshots + "/remote-debugger.png"));
        }
    }

    void downloadProgressHandlesKnownAndUnknownSizes()
    {
        QProgressBar bar;
        DownloadProgress::apply(&bar, -1);
        QCOMPARE(bar.minimum(), 0);
        QCOMPARE(bar.maximum(), 0);
        QVERIFY(DownloadProgress::text(512, -1).contains(QString::fromUtf8("总大小未知")));
        QCOMPARE(DownloadProgress::text(512, 1024), QString::fromUtf8("512 B / 1.0 KB（50%）"));
        DownloadProgress::apply(&bar, DownloadProgress::percentage(512, 1024));
        QCOMPARE(bar.maximum(), 100);
        QCOMPARE(bar.value(), 50);
        DownloadProgress::apply(&bar, -1);
        QCOMPARE(bar.maximum(), 0);
        DownloadProgress::apply(&bar, 100);
        QCOMPARE(bar.maximum(), 100);
        QCOMPARE(bar.value(), 100);
        QCOMPARE(DownloadProgress::percentage(-1, 100), 0);
        QCOMPARE(DownloadProgress::percentage(100, 0), -1);
        QCOMPARE(DownloadProgress::percentage((std::numeric_limits<qint64>::max)(), (std::numeric_limits<qint64>::max)()), 100);
        QCOMPARE(DownloadProgress::size(1024 * 1024), QString("1.0 MB"));
    }

    void updateProgressSurvivesDialogReopening()
    {
        auto main = window();
        main->m_availableUpdateVersion = QCoreApplication::applicationVersion();
        const QString manifest = qEnvironmentVariable("SYNC_QA_MANIFEST_PATH");
        if (!manifest.isEmpty()) {
            QFile file(manifest);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto document = QJsonDocument::fromJson(file.readAll());
            QVERIFY(document.isObject());
            const auto info = document.object();
            QCOMPARE(info.value("version").toString(), main->m_availableUpdateVersion);
            main->m_availableUpdateNotes = info.value("notes").toString();
            QVERIFY(!main->m_availableUpdateNotes.isEmpty());
            for (const auto &value : info.value("sections").toArray()) {
                UpdateNoteSection section;
                section.title = value.toObject().value("title").toString();
                for (const auto &item : value.toObject().value("items").toArray()) section.items.append(item.toString());
                main->m_availableUpdateSections.append(section);
            }
            QVERIFY(!main->m_availableUpdateSections.isEmpty());
        }
        main->m_installerDownloadVersion = main->m_availableUpdateVersion;
        main->m_installerReply = new PendingDownloadReply(main.get());
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) QVERIFY(QDir().mkpath(screenshots));
        auto inspectDialog = [&](const QString &expected, int maximum, int value, const QString &image) {
            bool matched = false;
            QTimer inspect;
            inspect.setSingleShot(true);
            connect(&inspect, &QTimer::timeout, main.get(), [&]() {
                auto *dialog = main->m_updateDialog.data();
                if (!dialog) return;
                auto *bar = dialog->findChild<QProgressBar *>("progressBar");
                auto *label = dialog->findChild<QLabel *>("statusLabel");
                matched = bar && label && label->text() == expected && bar->maximum() == maximum
                    && (maximum == 0 || bar->value() == value);
                if (!manifest.isEmpty()) {
                    for (auto *button : dialog->findChildren<QToolButton *>("sectionButton")) button->setChecked(true);
                    QTest::qWait(50);
                }
                if (matched && !screenshots.isEmpty()) matched = dialog->grab().save(screenshots + image);
                if (matched && !manifest.isEmpty() && !screenshots.isEmpty()) {
                    auto *scroll = dialog->findChild<QScrollArea *>("scrollArea");
                    if (scroll) { scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum()); QTest::qWait(50); }
                    matched = dialog->grab().save(screenshots + image + ".bottom.png");
                }
                dialog->reject();
            });
            inspect.start(50);
            main->showAvailableUpdate();
            inspect.stop();
            return matched;
        };
        const QString known = QString("下载安装包：%1").arg(DownloadProgress::text(1024 * 1024, 2 * 1024 * 1024));
        main->setUpdateDialogStatus(known, 50);
        QVERIFY(main->findChild<QLabel *>("updateNoticeLabel")->text().contains(known));
        QVERIFY(inspectDialog(known, 100, 50, "/update-progress-known.png"));
        QVERIFY(main->m_installerReply);
        QVERIFY(inspectDialog(known, 100, 50, "/update-progress-reopened.png"));
        const QString unknown = QString("下载安装包：%1").arg(DownloadProgress::text(512 * 1024, -1));
        main->setUpdateDialogStatus(unknown, -1);
        QVERIFY(inspectDialog(unknown, 0, 0, "/update-progress-unknown.png"));
    }

    void roundedFramesPreserveMessageButtonsAndProgressBoundaries()
    {
        RoundedMessageBox message;
        auto *accept = message.addButton("确定", QMessageBox::AcceptRole);
        auto *history = message.addButton("历史版本下载", QMessageBox::ActionRole);
        QVERIFY(accept && history && accept != history);
        QCOMPARE(message.buttonRole(history), QMessageBox::ActionRole);
        message.setText("消息框详情回归");
        message.setDetailedText("module.pdb");
        message.show();
        QTest::qWait(20);
        QVERIFY(message.isVisible());
        message.hide();
        auto main = window();
        main->show();
        QTest::qWait(50);
        QVERIFY(main->windowFlags().testFlag(Qt::FramelessWindowHint));
        QVERIFY(main->testAttribute(Qt::WA_TranslucentBackground));
        QVERIFY(main->findChild<QWidget *>("roundedTitleBar"));
        auto *table = main->findChild<QTableWidget *>("hostTable");
        QVERIFY(table && table->rowCount() == 1);
        QWidget *cell = table->cellWidget(0, 4);
        QVERIFY(cell);
        auto *bar = cell->findChild<QProgressBar *>();
        QVERIFY(bar);
        const QRect bounds(bar->mapTo(table->viewport(), QPoint()), bar->size());
        QVERIFY(table->visualRect(table->model()->index(0, 4)).contains(bounds));
        QVERIFY(!bounds.intersects(table->visualItemRect(table->item(0, 5))));
        QCOMPARE(main->grab().toImage().pixelColor(0, 0).alpha(), 0);
        QToolButton *maximize = nullptr;
        for (auto *button : main->findChild<QWidget *>("roundedTitleBar")->findChildren<QToolButton *>()) {
            if (button->accessibleName() == QString("最大化")) maximize = button;
        }
        QVERIFY(maximize);
        maximize->click();
        QTRY_VERIFY(main->isMaximized());
        QVERIFY(main->grab().toImage().pixelColor(0, 0).alpha() > 0);
        maximize->click();
        QTRY_VERIFY(!main->isMaximized());
        QCOMPARE(main->grab().toImage().pixelColor(0, 0).alpha(), 0);
    }

    void nativeMaximizeBoundsKeepPhysicalUnits()
    {
#ifdef Q_OS_WIN
        NativeRoundedFrame frame;
        frame.setMinimumSize(220, 180);
        frame.resize(600, 400);
        const HWND hwnd = reinterpret_cast<HWND>(frame.winId());
        QVERIFY(hwnd);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        QVERIFY(monitor && GetMonitorInfo(monitor, &info));
        const LONG width = info.rcWork.right - info.rcWork.left;
        const LONG height = info.rcWork.bottom - info.rcWork.top;
        MINMAXINFO limits = {};
        limits.ptReserved = {13, 17};
        limits.ptMaxSize = {width / 2, height / 2};
        limits.ptMaxTrackSize = limits.ptMaxSize;
        MSG message = {};
        message.hwnd = hwnd;
        message.message = WM_GETMINMAXINFO;
        message.lParam = reinterpret_cast<LPARAM>(&limits);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        qintptr result = -1;
#else
        long result = -1;
#endif
        QVERIFY(frame.nativeEvent("windows_generic_MSG", &message, &result));
        QCOMPARE(result, decltype(result)(0));
        QCOMPARE(limits.ptMaxSize.x, width);
        QCOMPARE(limits.ptMaxSize.y, height);
        QCOMPARE(limits.ptMaxPosition.x, info.rcWork.left - info.rcMonitor.left);
        QCOMPARE(limits.ptMaxPosition.y, info.rcWork.top - info.rcMonitor.top);
        QVERIFY(limits.ptMaxTrackSize.x >= width && limits.ptMaxTrackSize.y >= height);
        QVERIFY(limits.ptMinTrackSize.x >= frame.minimumWidth() * frame.devicePixelRatioF());
        QVERIFY(limits.ptMinTrackSize.y >= frame.minimumHeight() * frame.devicePixelRatioF());
        QCOMPARE(limits.ptReserved.x, LONG(13));
        QCOMPARE(limits.ptReserved.y, LONG(17));

        frame.setMaximumSize(400, 300);
        QVERIFY(frame.nativeEvent("windows_generic_MSG", &message, &result));
        QVERIFY(limits.ptMaxSize.x <= qCeil(frame.maximumWidth() * frame.devicePixelRatioF()));
        QVERIFY(limits.ptMaxSize.y <= qCeil(frame.maximumHeight() * frame.devicePixelRatioF()));
        QVERIFY(limits.ptMaxSize.x >= limits.ptMinTrackSize.x);
        QVERIFY(limits.ptMaxSize.y >= limits.ptMinTrackSize.y);

        frame.setFixedSize(220, 180);
        QVERIFY(!frame.nativeEvent("windows_generic_MSG", &message, &result));
        message.message = WM_NULL;
        QVERIFY(!frame.nativeEvent("windows_generic_MSG", &message, &result));
#else
        QSKIP("Requires native Windows monitor coordinates");
#endif
    }

    void maximizedFrameFillsWorkAreaAndRestores()
    {
#ifdef Q_OS_WIN
        auto main = window();
        main->show();
        QVERIFY(QTest::qWaitForWindowExposed(main.get()));
        QTest::qWait(50);
        const QRect normal = main->frameGeometry();
        auto *title = main->findChild<QWidget *>("roundedTitleBar");
        QVERIFY(title);
        QToolButton *maximize = nullptr;
        for (auto *button : title->findChildren<QToolButton *>()) {
            if (button->accessibleName() == QString("最大化")) maximize = button;
        }
        QVERIFY(maximize);
        const HWND hwnd = reinterpret_cast<HWND>(main->winId());
        auto fillsWorkArea = [&]() {
            RECT bounds = {};
            MONITORINFO info = {};
            info.cbSize = sizeof(info);
            const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            return monitor && GetMonitorInfo(monitor, &info) && GetWindowRect(hwnd, &bounds)
                && qAbs(bounds.left - info.rcWork.left) <= 1
                && qAbs(bounds.top - info.rcWork.top) <= 1
                && qAbs(bounds.right - info.rcWork.right) <= 1
                && qAbs(bounds.bottom - info.rcWork.bottom) <= 1;
        };
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(main->isMaximized());
        QTest::qWait(50);
        RECT observed = {};
        MONITORINFO observedMonitor = {};
        observedMonitor.cbSize = sizeof(observedMonitor);
        const HMONITOR observedScreen = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        QVERIFY(observedScreen && GetMonitorInfo(observedScreen, &observedMonitor) && GetWindowRect(hwnd, &observed));
        qInfo() << "Native maximized edges" << observed.left << observed.top << observed.right << observed.bottom
                << "work area" << observedMonitor.rcWork.left << observedMonitor.rcWork.top
                << observedMonitor.rcWork.right << observedMonitor.rcWork.bottom
                << "logical" << main->frameGeometry() << "DPR" << main->devicePixelRatioF();
        QTRY_VERIFY2(fillsWorkArea(), "Native maximized bounds do not fill the monitor work area");
        QWindow *handle = main->windowHandle();
        QVERIFY(handle);
        QScreen *screen = handle->screen();
        QVERIFY(screen);
        QCOMPARE(main->frameGeometry(), screen->availableGeometry());
        const QImage image = main->grab().toImage();
        RECT bounds = {};
        QVERIFY(GetWindowRect(hwnd, &bounds));
        QVERIFY(qAbs(image.width() - (bounds.right - bounds.left)) <= 1);
        QVERIFY(qAbs(image.height() - (bounds.bottom - bounds.top)) <= 1);
        qInfo() << "Maximized logical" << main->frameGeometry() << "DPR" << main->devicePixelRatioF()
                << "native" << QSize(bounds.right - bounds.left, bounds.bottom - bounds.top)
                << "rendered" << image.size();
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            QVERIFY(image.save(screenshots + "/maximized.png"));
        }
        QTest::mouseClick(maximize, Qt::LeftButton);
        QTRY_VERIFY(!main->isMaximized());
        QTRY_COMPARE(main->frameGeometry(), normal);
        QCOMPARE(main->grab().toImage().pixelColor(0, 0).alpha(), 0);

        const QPoint point(100, title->height() / 2);
        QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(title, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(main->isMaximized());
        QTRY_VERIFY(fillsWorkArea());
        QTest::mouseDClick(title, Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseRelease(title, Qt::LeftButton, Qt::NoModifier, point);
        QTRY_VERIFY(!main->isMaximized());
        QTRY_COMPARE(main->frameGeometry(), normal);
#else
        QSKIP("Requires native Windows window bounds");
#endif
    }

    void microsoftDebuggerDownload()
    {
        if (!qEnvironmentVariableIsSet("SYNC_QA_REMOTE_DOWNLOAD")) QSKIP("Requires live Microsoft network and Windows Downloads folder");
        RemoteDebugger dialog;
        const QString requested = qEnvironmentVariable("SYNC_QA_REMOTE_VERSION");
        if (!requested.isEmpty()) {
            auto *versions = dialog.findChild<QComboBox *>("debuggerVersion");
            const int index = versions->findText("Visual Studio " + requested);
            QVERIFY(index >= 0);
            versions->setCurrentIndex(index);
        }
        QSignalSpy finished(&dialog, &RemoteDebugger::downloadFinished);
        auto *bar = dialog.findChild<QProgressBar *>("downloadProgressBar");
        auto *progress = dialog.findChild<QLabel *>("downloadProgressLabel");
        auto *area = dialog.findChild<QWidget *>("downloadProgressArea");
        QVERIFY(bar && progress && area && area->isHidden());
        const QString screenshots = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        QTimer progressCapture;
        bool progressCaptured = false;
        if (!screenshots.isEmpty()) {
            QVERIFY(QDir().mkpath(screenshots));
            dialog.show();
            QVERIFY(QTest::qWaitForWindowExposed(&dialog));
            connect(&progressCapture, &QTimer::timeout, &dialog, [&]() {
                if (progress->text().contains(QString::fromUtf8("正在下载"))
                    && (progress->text().contains(" KB") || progress->text().contains(" MB"))) {
                    progressCaptured = dialog.grab().save(screenshots + "/debugger-download-progress.png");
                    progressCapture.stop();
                }
            });
            progressCapture.start(200);
        }
        dialog.findChild<QPushButton *>("downloadDebuggerButton")->click();
        QVERIFY(!area->isHidden());
        QCOMPARE(bar->maximum(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 300000);
        const auto result = finished.takeFirst();
        const QString path = result.at(0).toString();
        QVERIFY2(result.at(1).toString().isEmpty(), qPrintable(result.at(1).toString()));
        QVERIFY(QFileInfo(path).size() > 1000000);
        QCOMPARE(QFileInfo(path).absolutePath(), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        QCOMPARE(bar->maximum(), 100);
        QCOMPARE(bar->value(), 100);
        QVERIFY(progress->text().contains(QString::fromUtf8("下载完成")));
        QVERIFY(progress->text().contains(DownloadProgress::size(QFileInfo(path).size())));
        if (!screenshots.isEmpty()) {
            progressCapture.stop();
            QVERIFY(progressCaptured);
            QVERIFY(dialog.grab().save(screenshots + "/debugger-download-complete.png"));
        }
        const QString retained = qEnvironmentVariable("SYNC_QA_REMOTE_INSTALLER_COPY");
        if (!retained.isEmpty()) QVERIFY(QFile::copy(path, retained));
        QVERIFY(QFile::remove(path));
    }
};

int main(int argc, char *argv[])
{
    Appearance::prepareApplication();
    QApplication app(argc, argv);
    Appearance::apply(app);
    SyncTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "sync_tests.moc"
