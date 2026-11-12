#pragma execution_character_set("utf-8")
#include "configmanager.h"
#include "mainwindow.h"
#include "syncplan.h"
#include "syncworker.h"

#include <QtTest>
#include <QAction>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QProgressBar>
#include <QProcess>
#include <QSettings>
#include <QScreen>
#include <QSignalSpy>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <memory>
#include <thread>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
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
        auto result = std::make_unique<MainWindow>(nullptr, configPath());
        stopTimers(*result);
        return result;
    }
private slots:
    void initTestCase()
    {
        QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
        QApplication::setFont(QFont("Microsoft YaHei", 10));
        QApplication::setWindowIcon(QIcon(":/icons/app_32.png"));
        QApplication::setApplicationVersion("1.0.8");
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
        if (m_hadStartup) settings.setValue("RemoteReleaseSync", m_originalStartup);
        else settings.remove("RemoteReleaseSync");
        settings.sync();
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
        QCOMPARE(tabs->count(), 2);
        tabs->setCurrentIndex(1);
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
        auto *button = main->findChild<QPushButton *>("usageTipsButton");
        QVERIFY(button);
        button->click();
        QDialog *tips = main->findChild<QDialog *>("UsageTipsDialog");
        QVERIFY(tips);
        button->click();
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
    void autoSyncIncludesSiblingRuleAndStopDisablesIt()
    {
        QVERIFY(writeFile(source() + "/../Lang/zh.qm", "initial"));
        auto main = window({{"../Lang", {"qm"}}}, true);
        // The startup singleShot runs the same public workflow as normal startup.
        QTRY_COMPARE_WITH_TIMEOUT(readFile(target() + "/../Lang/zh.qm"), QByteArray("initial"), 15000);
        QVERIFY(writeFile(source() + "/../Lang/zh.qm", "updated-language"));
        QTest::qWait(250);
        QTRY_VERIFY_WITH_TIMEOUT((QMetaObject::invokeMethod(main.get(), "pollForChanges"),
            readFile(target() + "/../Lang/zh.qm") == QByteArray("updated-language")), 15000);
        main->findChild<QPushButton *>("stopButton")->click();
        QVERIFY(!main->findChild<QCheckBox *>("autoSyncCheck")->isChecked());
        QVERIFY(!ConfigManager::load(configPath()).autoSync);
    }
};

QTEST_MAIN(SyncTests)
#include "sync_tests.moc"
