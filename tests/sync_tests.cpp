#pragma execution_character_set("utf-8")
#include "configmanager.h"
#include "diagnostics.h"
#include "mainwindow.h"
#include "remotedebugger.h"
#include "syncplan.h"
#include "syncworker.h"
#include "synccompletiontoast.h"

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QProgressBar>
#include <QProcess>
#include <QSettings>
#include <QScreen>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QToolButton>
#include <memory>
#include <thread>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace
{
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
        toast->installEventFilter(this);
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::Show) {
            auto *toast = qobject_cast<SyncCompletionToast *>(object);
            messages.append(toast->findChild<QLabel *>("messageLabel")->text());
            shownAt.append(clock.elapsed());
        }
        return QObject::eventFilter(object, event);
    }
};

SyncCompletionToast *completionToast()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (auto *toast = qobject_cast<SyncCompletionToast *>(widget)) return toast;
    }
    return nullptr;
}

bool workersAreIdle(MainWindow &window)
{
    for (QThread *thread : window.findChildren<QThread *>()) {
        if (thread->isRunning()) return false;
    }
    return true;
}
}

class SyncTests : public QObject
{
    Q_OBJECT
private:
    std::unique_ptr<QTemporaryDir> m_temp;
    QString root() const { return m_temp->path(); }
    QString source() const { return root() + "/本地 source/a/Release"; }
    QString target() const { return root() + "/远端 target/b/Release"; }
    QString configPath() const { return root() + "/qa-config.json"; }
    QString m_originalStartup;
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
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        QApplication::setFont(QFont("Microsoft YaHei", 10));
        QApplication::setWindowIcon(QIcon(":/icons/app_32.png"));
        QApplication::setApplicationVersion("1.0.9");
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
    }
    void cleanup()
    {
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
        QVERIFY(main->findChild<QLabel *>("rulePreview")->text().contains("image"));
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
    void completionToastQueuesAndExpiresWithoutTakingFocus()
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
        toast.enqueue("SERVER-A");
        toast.enqueue("SERVER-B");
        QVERIFY(toast.isVisible());
        QCOMPARE(label->text(), QString("SERVER-A 同步完成"));
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
        QTest::qWait(800);
        QCOMPARE(label->text(), QString("SERVER-A 同步完成"));
        QTRY_COMPARE_WITH_TIMEOUT(events.messages.size(), 2, 2000);
        QCOMPARE(events.messages, QStringList({"SERVER-A 同步完成", "SERVER-B 同步完成"}));
        QVERIFY(events.shownAt.at(1) - events.shownAt.at(0) >= 950);
        QVERIFY(toast.isVisible());
        QTest::qWait(800);
        QVERIFY(toast.isVisible());
        QTRY_VERIFY_WITH_TIMEOUT(!toast.isVisible(), 2000);
        QVERIFY(events.clock.elapsed() - events.shownAt.at(1) >= 950);
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
        toast.enqueue(QString("很长的主机名称 <SERVER> ").repeated(20) + "\nEND");
        auto *label = toast.findChild<QLabel *>("messageLabel");
        QVERIFY(label);
        QCOMPARE(label->textFormat(), Qt::PlainText);
        QVERIFY(!label->text().contains('\n'));
        QVERIFY(label->text().endsWith(" 同步完成"));
        QVERIFY(label->text().contains(QChar(0x2026)));
        QVERIFY(label->fontMetrics().horizontalAdvance(label->text()) <= label->contentsRect().width());
        QScreen *screen = QGuiApplication::screenAt(toast.frameGeometry().center());
        QVERIFY(screen);
        QVERIFY(screen->availableGeometry().contains(toast.frameGeometry()));
        const QString screenshot = qEnvironmentVariable("SYNC_QA_SCREENSHOTS");
        if (!screenshot.isEmpty()) {
            QDir().mkpath(screenshot);
            QVERIFY(toast.grab().save(screenshot + "/completion-toast-long.png"));
        }
        toast.enqueue("QUEUED");
        toast.dismiss();
        QVERIFY(!toast.isVisible());
        QTest::qWait(1100);
        QCOMPARE(events.messages.size(), 1);
        QVERIFY(!toast.isVisible());
        toast.enqueue("NEW");
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
        QTRY_COMPARE_WITH_TIMEOUT(events.messages.size(), 2, 15000);
        QVERIFY(events.messages.contains("测试主机 同步完成"));
        QVERIFY(events.messages.contains("第二台主机 同步完成"));
        QVERIFY(events.shownAt.at(1) - events.shownAt.at(0) >= 950);
        QVERIFY(toast->isVisible());
        QVERIFY(!main->isVisible());
        QCOMPARE(readFile(target() + "/app.exe"), QByteArray("toast-copy"));
        QCOMPARE(readFile(target() + "-second/app.exe"), QByteArray("toast-copy"));
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(!toast->isVisible(), 3000);
        // A subsequent no-op sync and an explicit check must produce no toast.
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        main->findChild<QPushButton *>("refreshButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QTest::qWait(100);
        QCOMPARE(events.messages.size(), 2);
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
        QCOMPARE(events.messages.first(), QString("测试主机 同步完成"));
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
        reveal->click();
        QCOMPARE(password->echoMode(), QLineEdit::Normal);
        QCOMPARE(password->displayText(), QString("qa-only-credential"));
        QEvent inactive(QEvent::WindowDeactivate);
        QCoreApplication::sendEvent(main.get(), &inactive);
        QCOMPARE(password->echoMode(), QLineEdit::Password);
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
        QVERIFY(writeFile(source() + "/assets/example.lib"));
        QVERIFY(writeFile(source() + "/assets/example.pdb"));
        QVERIFY(writeFile(source() + "/assets/keep.lib"));
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
        edit->setPlainText("skip.dll; example.lib\npdb");
        apply->click();
        QCOMPARE(ConfigManager::load(configPath()).excludedFilePatterns,
                 QStringList({"example.lib", "pdb", "skip.dll"}));
        main->findChild<QPushButton *>("invertTypeButton")->click();
        QCOMPARE(ConfigManager::load(configPath()).selectedFileTypes, QStringList({"dll", "pdb"}));
        main->findChild<QPushButton *>("syncButton")->click();
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(target() + "/assets/keep.lib"), 15000);
        QTRY_VERIFY_WITH_TIMEOUT(workersAreIdle(*main), 15000);
        QVERIFY(QFileInfo::exists(target() + "/good.dll"));
        QVERIFY(!QFileInfo::exists(target() + "/app.exe"));
        QVERIFY(!QFileInfo::exists(target() + "/SKIP.dll"));
        QVERIFY(!QFileInfo::exists(target() + "/assets/example.lib"));
        QCOMPARE(readFile(target() + "/assets/example.pdb"), QByteArray("existing symbol stays"));
        const auto patterns = ConfigManager::load(configPath()).excludedFilePatterns;
        const auto fingerprint = SyncPlan::fingerprint(source(), {"dll", "pdb"}, config.syncRules, patterns);
        QVERIFY(writeFile(source() + "/SKIP.dll", "excluded source changed"));
        QCOMPARE(SyncPlan::fingerprint(source(), {"dll", "pdb"}, config.syncRules, patterns), fingerprint);
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

    void microsoftDebuggerDownload()
    {
        if (!qEnvironmentVariableIsSet("SYNC_QA_REMOTE_DOWNLOAD")) QSKIP("Requires live Microsoft network and Windows Downloads folder");
        RemoteDebugger dialog;
        QSignalSpy finished(&dialog, &RemoteDebugger::downloadFinished);
        dialog.findChild<QPushButton *>("downloadDebuggerButton")->click();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 120000);
        const auto result = finished.takeFirst();
        const QString path = result.at(0).toString();
        QVERIFY2(result.at(1).toString().isEmpty(), qPrintable(result.at(1).toString()));
        QVERIFY(QFileInfo(path).size() > 1000000);
        QCOMPARE(QFileInfo(path).absolutePath(), QStandardPaths::writableLocation(QStandardPaths::DownloadLocation));
        QVERIFY(QFile::remove(path));
    }
};

QTEST_MAIN(SyncTests)
#include "sync_tests.moc"
