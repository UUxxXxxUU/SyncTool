#include "diagnostics.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStandardPaths>

#ifdef Q_OS_WIN
#define SPDLOG_WCHAR_FILENAMES
#include <windows.h>
#endif

#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <memory>
#include <mutex>

namespace
{
std::mutex logMutex;
std::unique_ptr<spdlog::logger> logger;
QString activeDay;
bool started = false;

#ifdef Q_OS_WIN
using StopReporter = void (*)();
StopReporter stopReporter = nullptr;
#endif
}

QString Diagnostics::directory()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) + "/Diagnostics";
}

void Diagnostics::start()
{
    QDir().mkpath(directory() + "/Logs");
    {
        std::lock_guard<std::mutex> lock(logMutex);
        started = true;
    }
    const QString version = QCoreApplication::applicationVersion();
    record("app.start", "version=" + version);

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
    record("app.exit");
#ifdef Q_OS_WIN
    if (stopReporter) stopReporter();
#endif
    std::lock_guard<std::mutex> lock(logMutex);
    started = false;
    logger.reset();
}

void Diagnostics::record(const char *event, const QString &fields, bool warning)
{
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
        QByteArray encoded = fields.toUtf8();
        encoded.replace('\n', ' ').replace('\r', ' ');
        logger->log(warning ? spdlog::level::warn : spdlog::level::info, "{} {}", event, encoded.constData());
    } catch (const std::exception &) {
        // Diagnostics must never interrupt synchronization or startup.
    }
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
