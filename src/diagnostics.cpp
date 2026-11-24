#include "diagnostics.h"

#include <QCoreApplication>
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAction>
#include <QActionEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QFile>
#include <QFileDialog>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSpinBox>
#include <QStatusBar>
#include <QSysInfo>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWheelEvent>
#include <QWindowStateChangeEvent>

#ifdef Q_OS_WIN
#define SPDLOG_WCHAR_FILENAMES
#include <windows.h>
#endif

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <mutex>
#include <algorithm>

namespace
{
std::mutex logMutex;
std::mutex directoryMutex;
QString rootOverride;
std::unique_ptr<spdlog::logger> logger;
QString activeDay;
bool started = false;
QString session;
QString userAction;
QStringList secrets;
struct BackgroundRecord
{
    QByteArray context;
    QByteArray outcome;
    QJsonObject fields;
    QElapsedTimer summaryTimer;
    QDateTime since;
    QDateTime lastSeen;
    QString lastRun;
    qint64 repeats = 0;
    bool warning = false;
};
std::mutex backgroundMutex;
QHash<QString, BackgroundRecord> backgroundRecords;
QJsonObject backgroundSummary(const BackgroundRecord &entry, const QString &reason)
{
    QJsonObject result = entry.fields;
    result.insert("repeats", double(entry.repeats));
    result.insert("since_utc", entry.since.toString(Qt::ISODateWithMs));
    result.insert("last_seen_utc", entry.lastSeen.toString(Qt::ISODateWithMs));
    result.insert("last_run", entry.lastRun);
    result.insert("reason", reason);
    return result;
}
quint64 sequence = 0;
QtMessageHandler previousMessageHandler = nullptr;
QString redact(QString text);

QJsonValue safeFields(const QJsonValue &value)
{
    if (value.isObject()) {
        QJsonObject result;
        const auto object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it) {
            const QString key = it.key().toLower();
            const bool sensitive = key.contains("password") || key.contains("secret") || key.contains("token")
                || key == "authorization" || key == "stdin" || key == "payload" || key == "credentials";
            result.insert(it.key(), sensitive && (it.value().isString() || it.value().isArray() || it.value().isObject())
                ? QJsonValue("[REDACTED]") : safeFields(it.value()));
        }
        return result;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const auto &item : value.toArray()) result.append(safeFields(item));
        return result;
    }
    if (value.isString()) return redact(value.toString());
    return value;
}

QString redact(QString text)
{
    // 同时覆盖普通文本与 JSON 转义后的凭据，避免错误输出中包含旧密码或特殊字符密码。
    for (const QString &secret : secrets) {
        text.replace(secret, "[REDACTED]");
        QByteArray escaped = QJsonDocument(QJsonArray{secret}).toJson(QJsonDocument::Compact);
        if (escaped.size() >= 4) text.replace(QString::fromUtf8(escaped.mid(2, escaped.size() - 4)), "[REDACTED]");
    }
    static const QRegularExpression urlCredentials(R"((https?://)[^/@\s]+@)");
    static const QRegularExpression queryToken(R"(([?&](?:access_token|token|password|api_key)=)[^&\s"\\]+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression authorization(R"((\bauthorization\s*[:=]\s*)(?:Basic|Bearer)\s+[^\s]+)", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression assignment(R"((\b(?:password|passwd|pwd|secret|token|access_token|refresh_token|api_key)\b\s*[:=]\s*)(?:"[^"]*"|'[^']*'|[^\s;,]+))", QRegularExpression::CaseInsensitiveOption);
    text.replace(urlCredentials, "\\1[REDACTED]@");
    text.replace(queryToken, "\\1[REDACTED]");
    text.replace(authorization, "\\1[REDACTED]");
    text.replace(assignment, "\\1[REDACTED]");
    return text;
}

bool sensitiveEdit(const QLineEdit *edit)
{
    return edit->echoMode() != QLineEdit::Normal || edit->objectName().contains("password", Qt::CaseInsensitive)
        || edit->property("diagnosticsSensitive").toBool();
}

QJsonObject describe(QObject *object)
{
    QJsonObject fields{{"control", object->objectName()}, {"class", object->metaObject()->className()}};
    if (auto *widget = qobject_cast<QWidget *>(object)) fields.insert("window", widget->window()->windowTitle());
    if (object->parent()) fields.insert("parent", object->parent()->objectName());
    if (auto *button = qobject_cast<QAbstractButton *>(object)) fields.insert("label", button->text());
    if (auto *action = qobject_cast<QAction *>(object)) fields.insert("label", action->text());
    return fields;
}

void actionEvent(const char *name, QObject *object, QJsonObject fields = {})
{
    QJsonObject context = describe(object);
    for (auto it = fields.begin(); it != fields.end(); ++it) context.insert(it.key(), it.value());
    {
        std::lock_guard<std::mutex> lock(logMutex);
        userAction = QUuid::createUuid().toString(QUuid::WithoutBraces);
        context.insert("action", userAction);
    }
    Diagnostics::event(name, context);
}

class InteractionObserver : public QObject
{
public:
    explicit InteractionObserver(QObject *parent) : QObject(parent) {}
    void observe(QObject *object)
    {
        if (!object || object->property("diagnosticsObserved").toBool()) return;
        object->setProperty("diagnosticsObserved", true);
        if (auto *button = qobject_cast<QAbstractButton *>(object)) {
            connect(button, &QAbstractButton::clicked, this, [button](bool checked) {
                actionEvent("ui.click", button, {{"checked", checked}});
            });
            if (button->objectName() == "showPasswordButton") {
                connect(button, &QAbstractButton::pressed, this, [button]() { actionEvent("ui.password_reveal", button, {{"visible", true}}); });
                connect(button, &QAbstractButton::released, this, [button]() { actionEvent("ui.password_reveal", button, {{"visible", false}}); });
            }
        }
        if (auto *action = qobject_cast<QAction *>(object)) {
            connect(action, &QAction::triggered, this, [action](bool checked) {
                actionEvent("ui.action", action, {{"checked", checked}});
            });
        }
        if (auto *edit = qobject_cast<QLineEdit *>(object)) {
            connect(edit, &QLineEdit::textEdited, this, [edit]() { edit->setProperty("diagnosticsEdited", true); });
            connect(edit, &QLineEdit::editingFinished, this, [edit]() {
                if (!edit->property("diagnosticsEdited").toBool()) return;
                edit->setProperty("diagnosticsEdited", false);
                const bool sensitive = sensitiveEdit(edit);
                if (sensitive) Diagnostics::rememberSecret(edit->text());
                actionEvent("ui.input_commit", edit, {{"value", sensitive ? QString("[REDACTED]") : edit->text()}, {"sensitive", sensitive}});
            });
        }
        if (auto *combo = qobject_cast<QComboBox *>(object)) {
            connect(combo, QOverload<int>::of(&QComboBox::activated), this, [combo](int index) {
                actionEvent("ui.combo_select", combo, {{"index", index}, {"value", combo->currentText()}});
            });
        }
        if (auto *spin = qobject_cast<QSpinBox *>(object)) {
            connect(spin, &QSpinBox::editingFinished, this, [spin]() {
                actionEvent("ui.number_commit", spin, {{"value", spin->value()}});
            });
        }
        if (auto *tabs = qobject_cast<QTabWidget *>(object)) {
            connect(tabs, &QTabWidget::currentChanged, this, [tabs](int index) {
                actionEvent("ui.tab_changed", tabs, {{"index", index}, {"title", tabs->tabText(index)}});
            });
        }
        if (auto *view = qobject_cast<QAbstractItemView *>(object)) {
            connect(view, &QAbstractItemView::clicked, this, [view](const QModelIndex &index) {
                actionEvent("ui.table_click", view, {{"row", index.row()}, {"column", index.column()}, {"value", index.data().toString()}});
            });
            connect(view, &QAbstractItemView::doubleClicked, this, [view](const QModelIndex &index) {
                actionEvent("ui.table_double_click", view, {{"row", index.row()}, {"column", index.column()}, {"value", index.data().toString()}});
            });
        }
        if (auto *dialog = qobject_cast<QDialog *>(object)) {
            connect(dialog, &QDialog::finished, this, [dialog](int result) {
                auto fields = describe(dialog);
                fields.insert("result", result);
                if (auto *box = qobject_cast<QMessageBox *>(dialog)) {
                    fields.insert("text", box->text());
                    fields.insert("information", box->informativeText());
                    fields.insert("details", box->detailedText());
                }
                Diagnostics::event("ui.dialog_finished", fields);
            });
        }
        if (auto *files = qobject_cast<QFileDialog *>(object)) {
            connect(files, &QFileDialog::filesSelected, this, [files](const QStringList &paths) {
                actionEvent("ui.files_selected", files, {{"paths", QJsonArray::fromStringList(paths)}});
            });
        }
        if (auto *tray = qobject_cast<QSystemTrayIcon *>(object)) {
            connect(tray, &QSystemTrayIcon::activated, this, [tray](QSystemTrayIcon::ActivationReason reason) {
                actionEvent("ui.tray_activate", tray, {{"reason", int(reason)}});
            });
        }
        if (auto *status = qobject_cast<QStatusBar *>(object)) {
            connect(status, &QStatusBar::messageChanged, this, [status](const QString &message) {
                if (message.isEmpty() || status->property("diagnosticsLastStatus").toString() == message) return;
                status->setProperty("diagnosticsLastStatus", message);
                Diagnostics::event("ui.status", {{"window", status->window()->windowTitle()}, {"message", message}});
            });
        }
    }
    void observeTree(QObject *root)
    {
        observe(root);
        for (QObject *child : root->findChildren<QObject *>()) observe(child);
    }
protected:
    bool eventFilter(QObject *object, QEvent *event) override
    {
        if (event->type() == QEvent::ActionAdded) observe(static_cast<QActionEvent *>(event)->action());
        auto *widget = qobject_cast<QWidget *>(object);
        if (!widget) return false;
        if (event->type() == QEvent::Polish || event->type() == QEvent::Show) {
            observeTree(widget);
            if (event->type() == QEvent::Show && widget->isWindow()) Diagnostics::event("ui.window_show", describe(widget));
        }
        if (event->type() == QEvent::MouseButtonPress || event->type() == QEvent::MouseButtonDblClick) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            actionEvent("ui.pointer", widget, {{"button", int(mouse->button())}, {"double_click", event->type() == QEvent::MouseButtonDblClick},
                {"x", mouse->pos().x()}, {"y", mouse->pos().y()}});
        }
        if (event->type() == QEvent::KeyPress) {
            const auto *key = static_cast<QKeyEvent *>(event);
            const bool controlKey = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Escape
                || key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab;
            const auto *edit = qobject_cast<QLineEdit *>(widget);
            const bool sensitive = edit && sensitiveEdit(edit);
            if (controlKey || (!sensitive && (key->modifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))))
                actionEvent("ui.key_control", widget, {{"key", key->key()}, {"modifiers", int(key->modifiers())}});
        }
        if (event->type() == QEvent::Wheel) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            if (now - widget->property("diagnosticsWheelAt").toLongLong() > 500) {
                widget->setProperty("diagnosticsWheelAt", now);
                const auto *wheel = static_cast<QWheelEvent *>(event);
                actionEvent("ui.scroll", widget, {{"dx", wheel->angleDelta().x()}, {"dy", wheel->angleDelta().y()}});
            }
        }
        if (event->type() == QEvent::FocusIn) {
            if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) edit->setProperty("diagnosticsPreviousText", edit->toPlainText());
        }
        if (event->type() == QEvent::FocusOut) {
            if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
                if (edit->property("diagnosticsPreviousText").isValid() && edit->property("diagnosticsPreviousText").toString() != edit->toPlainText())
                    actionEvent("ui.text_commit", edit, {{"value", edit->toPlainText()}});
                edit->setProperty("diagnosticsPreviousText", edit->toPlainText());
            }
        }
        if (widget->isWindow() && (event->type() == QEvent::Close || event->type() == QEvent::Hide || event->type() == QEvent::WindowStateChange)) {
            auto fields = describe(widget);
            fields.insert("event", int(event->type()));
            fields.insert("state", int(widget->windowState()));
            Diagnostics::event("ui.window_state", fields);
        }
        if (widget->isWindow() && (event->type() == QEvent::Move || event->type() == QEvent::Resize)) {
            auto *timer = widget->findChild<QTimer *>("diagnosticsGeometry", Qt::FindDirectChildrenOnly);
            if (!timer) {
                timer = new QTimer(widget);
                timer->setObjectName("diagnosticsGeometry");
                timer->setSingleShot(true);
                const QPointer<QWidget> guard(widget);
                connect(timer, &QTimer::timeout, this, [guard]() {
                    if (!guard) return;
                    auto fields = describe(guard);
                    fields.insert("x", guard->x()); fields.insert("y", guard->y());
                    fields.insert("width", guard->width()); fields.insert("height", guard->height());
                    fields.insert("dpr", guard->devicePixelRatioF());
                    fields.insert("state", int(guard->windowState()));
                    Diagnostics::event("ui.window_geometry", fields);
                });
            }
            timer->start(300);
        }
        return false;
    }
};
InteractionObserver *observer = nullptr;

void qtMessages(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Diagnostics::event("qt.message", {{"type", int(type)}, {"category", context.category ? context.category : ""},
        {"file", context.file ? context.file : ""}, {"line", context.line}, {"message", message}},
        type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg);
    if (previousMessageHandler) previousMessageHandler(type, context, message);
}

#ifdef Q_OS_WIN
using StopReporter = void (*)();
StopReporter stopReporter = nullptr;
#endif
}

QString Diagnostics::directory()
{
    QString custom;
    { std::lock_guard<std::mutex> lock(directoryMutex); custom = rootOverride; }
    if (!custom.isEmpty()) return custom;
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Diagnostics";
}

void Diagnostics::start(const QString &diagnosticRoot)
{
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (started) return;
        if (!diagnosticRoot.isEmpty() && !QDir::isAbsolutePath(diagnosticRoot)) return;
        { std::lock_guard<std::mutex> directoryLock(directoryMutex); rootOverride = diagnosticRoot; }
        started = true;
        session = QUuid::createUuid().toString(QUuid::WithoutBraces);
        sequence = 0;
    }
    { std::lock_guard<std::mutex> lock(backgroundMutex); backgroundRecords.clear(); }
    QDir().mkpath(directory() + "/Logs");
    const QString version = QCoreApplication::applicationVersion();
    event("app.start", {{"version", version}, {"exe", QCoreApplication::applicationFilePath()}, {"cwd", QDir::currentPath()},
        {"os", QSysInfo::prettyProductName()}, {"kernel", QSysInfo::kernelVersion()}, {"cpu", QSysInfo::currentCpuArchitecture()},
        {"qt_runtime", qVersion()}, {"qt_build", QT_VERSION_STR}, {"startup", QCoreApplication::arguments().contains("--startup")}});
    if (!observer && qobject_cast<QApplication *>(QCoreApplication::instance())) {
        observer = new InteractionObserver(QCoreApplication::instance());
        QCoreApplication::instance()->installEventFilter(observer);
    }
    previousMessageHandler = qInstallMessageHandler(qtMessages);

#ifdef Q_OS_WIN
    const QDir bin(QCoreApplication::applicationDirPath());
    const QString modulePath = QDir::toNativeSeparators(bin.filePath("SyncToolCrashReporter.dll"));
    const QString reportsPath = QDir::toNativeSeparators(directory() + "/CrashDumps");
    int reporterState = 0;
    if (QDir().mkpath(reportsPath)) {
        HMODULE module = LoadLibraryExW(modulePath.toStdWString().c_str(), nullptr,
                                       LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module) {
            using StartReporter = int (*)(const wchar_t *, const wchar_t *, const char *);
            const auto startReporter = reinterpret_cast<StartReporter>(GetProcAddress(module, "StartSyncToolCrashReporter"));
            stopReporter = reinterpret_cast<StopReporter>(GetProcAddress(module, "StopSyncToolCrashReporter"));
            if (startReporter && stopReporter) {
                reporterState = startReporter(QDir::toNativeSeparators(bin.absolutePath()).toStdWString().c_str(),
                                              reportsPath.toStdWString().c_str(), version.toLatin1().constData());
            }
        }
    }
    record("crashpad.start", QString("handler=%1 wer=%2").arg(reporterState >= 1).arg(reporterState == 2),
           reporterState != 2);
#endif
}

void Diagnostics::stop()
{
    { std::lock_guard<std::mutex> lock(logMutex); if (!started) return; }
    QVector<QJsonObject> summaries;
    {
        std::lock_guard<std::mutex> lock(backgroundMutex);
        for (const auto &entry : backgroundRecords)
            if (entry.warning && entry.repeats) summaries.append(backgroundSummary(entry, "shutdown"));
        backgroundRecords.clear();
    }
    for (const auto &summary : summaries) event("sync.background_repeat", summary, true);
    record("app.exit");
    qInstallMessageHandler(previousMessageHandler);
    previousMessageHandler = nullptr;
    if (observer) {
        QCoreApplication::instance()->removeEventFilter(observer);
        delete observer;
        observer = nullptr;
    }
#ifdef Q_OS_WIN
    if (stopReporter) stopReporter();
#endif
    std::lock_guard<std::mutex> lock(logMutex);
    started = false;
    logger.reset();
    secrets.clear();
    userAction.clear();
    { std::lock_guard<std::mutex> directoryLock(directoryMutex); rootOverride.clear(); }
}

void Diagnostics::record(const char *event, const QString &fields, bool warning)
{
    static thread_local bool writing = false;
    if (writing || !event || !*event) return;
    // Qt 警告可能由文件系统或日志设施间接触发，递归写入会再次占用同一把锁。
    QScopedValueRollback<bool> reentry(writing, true);
    std::lock_guard<std::mutex> lock(logMutex);
    if (!started) return;
    try {
        const QString day = QDateTime::currentDateTimeUtc().date().toString(Qt::ISODate);
        if (!logger || activeDay != day) {
            const QString path = directory() + "/Logs/synctool-" + day + ".log";
            const auto fileName =
#ifdef Q_OS_WIN
                path.toStdWString();
#else
                QFile::encodeName(path).toStdString();
#endif
            auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(fileName, 5 * 1024 * 1024, 4);
            logger = std::make_unique<spdlog::logger>("synctool", std::move(sink));
            logger->set_pattern("%Y-%m-%dT%H:%M:%S.%eZ [%l] %v", spdlog::pattern_time_type::utc);
            logger->flush_on(spdlog::level::info);
            activeDay = day;
        }
        const QJsonDocument structured = QJsonDocument::fromJson(fields.toUtf8());
        QByteArray encoded = structured.isObject()
            ? QJsonDocument(safeFields(structured.object()).toObject()).toJson(QJsonDocument::Compact)
            : redact(fields).toUtf8();
        encoded.replace('\n', ' ').replace('\r', ' ');
        logger->log(warning ? spdlog::level::warn : spdlog::level::info,
            "{} session={} seq={} pid={} thread={} {}", event, session.toUtf8().constData(), ++sequence,
            QCoreApplication::applicationPid(), reinterpret_cast<quintptr>(QThread::currentThreadId()), encoded.constData());
    } catch (const std::exception &) {
        // Diagnostics must never interrupt synchronization or startup.
    }
}

void Diagnostics::event(const char *name, const QJsonObject &fields, bool warning)
{
    record(name, QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact)), warning);
}

bool Diagnostics::backgroundState(const QString &key, const QByteArray &context, const QJsonObject &outcome,
                                  const QJsonObject &fields, bool warning)
{
    const auto signature = QCryptographicHash::hash(QJsonDocument(outcome).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256);
    QJsonObject summary, changed;
    bool report = false;
    {
        std::lock_guard<std::mutex> lock(backgroundMutex);
        auto found = backgroundRecords.find(key);
        const bool fresh = found == backgroundRecords.end() || found->context != context || found->outcome != signature;
        const QDateTime now = QDateTime::currentDateTimeUtc();
        if (fresh) {
            if (found != backgroundRecords.end() && found->warning && found->repeats) summary = backgroundSummary(*found, "state_changed");
            changed = fields;
            changed.insert("result", outcome);
            if (found != backgroundRecords.end() && found->warning && !warning && !fields.value("cancelled").toBool()) changed.insert("recovered", true);
            BackgroundRecord next;
            next.context = context; next.outcome = signature; next.fields = changed;
            next.warning = warning; next.since = next.lastSeen = now; next.summaryTimer.start();
            backgroundRecords.insert(key, next);
            report = true;
        } else {
            ++found->repeats;
            found->lastSeen = now;
            found->lastRun = fields.value("run").toString();
            // 健康的空闲轮询不写心跳；持续异常每五分钟汇总，状态变化时也保留尚未汇总的次数。
            if (found->warning && found->summaryTimer.elapsed() >= 5 * 60 * 1000) {
                summary = backgroundSummary(*found, "interval");
                found->repeats = 0;
                found->since = now;
                found->summaryTimer.restart();
            }
        }
    }
    // 日志写入会获取 logMutex，不能在持有后台状态锁时执行。
    if (!summary.isEmpty()) event("sync.background_repeat", summary, true);
    if (report) event("sync.background_state", changed, warning);
    return report;
}

bool Diagnostics::backgroundFailure(const QString &key, const QByteArray &context)
{
    std::lock_guard<std::mutex> lock(backgroundMutex);
    const auto found = backgroundRecords.constFind(key);
    return found != backgroundRecords.constEnd() && found->warning && found->context == context;
}

void Diagnostics::rememberSecret(const QString &value)
{
    if (value.isEmpty()) return;
    std::lock_guard<std::mutex> lock(logMutex);
    if (!secrets.contains(value)) secrets.append(value);
    std::sort(secrets.begin(), secrets.end(), [](const QString &left, const QString &right) { return left.size() > right.size(); });
}

QString Diagnostics::lastUserAction()
{
    std::lock_guard<std::mutex> lock(logMutex);
    return userAction;
}

void Diagnostics::watch(QObject *root)
{
    if (observer && root) observer->observeTree(root);
}

void Diagnostics::maintainLogs(const QString &logDirectory, int retentionDays)
{
    if (retentionDays <= 0) return;
    const QDir dir(logDirectory);
    const QDate cutoff = QDateTime::currentDateTimeUtc().date().addDays(1 - retentionDays);
    const QRegularExpression managedName(R"(^synctool-(\d{4}-\d{2}-\d{2})(?:\.[1-4])?\.log$)");
    for (const auto &file : dir.entryInfoList(QDir::Files | QDir::NoSymLinks)) {
        const auto match = managedName.match(file.fileName());
        if (!match.hasMatch()) continue;
        const QDate date = QDate::fromString(match.captured(1), Qt::ISODate);
        if (date.isValid() && date < cutoff && !QFile::remove(file.absoluteFilePath())) {
            record("log.retention_failed", {}, true);
        }
    }
}

void Diagnostics::maintain(int retentionDays)
{
    if (!started) return;
    maintainLogs(directory() + "/Logs", retentionDays);
}
