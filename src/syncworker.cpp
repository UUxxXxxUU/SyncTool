#pragma execution_character_set("utf-8")

#include "syncworker.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>
#include <QThread>
#include <algorithm>
#include <qdatetime.h>

#ifdef Q_OS_WIN
#include <windows.h>
#include <winnetwk.h>
#endif

namespace
{
struct RemoteFile
{
    QString absolutePath;
    qint64 size = 0;
    qint64 modifiedMs = 0;
};

struct PendingCopy
{
    QString sourcePath;
    QString destinationPath;
    QString relativePath;
    qint64 size = 0;
};

QString normalizeExtension(const QString &value)
{
    QString type = value.trimmed().toLower();
    while (type.startsWith('.')) {
        type.remove(0, 1);
    }
    return type;
}

QString normalizeQtPath(const QString &value)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(value.trimmed()));
}

QString nativePath(const QString &value)
{
    return QDir::toNativeSeparators(value);
}

QString uncShareRoot(const QString &path)
{
    const QString native = nativePath(path);
    if (!native.startsWith("\\\\")) {
        return native;
    }

    const QString tail = native.mid(2);
    const QStringList parts = tail.split('\\', QString::SkipEmptyParts);
    if (parts.size() < 2) {
        return native;
    }

    return QString("\\\\%1\\%2").arg(parts.at(0), parts.at(1));
}

QString hostDisplayName(const HostEntry &host)
{
    if (!host.name.trimmed().isEmpty()) {
        return host.name.trimmed();
    }

    const QString native = nativePath(host.sharePath);
    if (native.startsWith("\\\\")) {
        const QStringList parts = native.mid(2).split('\\', QString::SkipEmptyParts);
        if (!parts.isEmpty()) {
            return parts.first();
        }
    }

    return host.sharePath;
}

QHash<QString, SyncWorker::LocalFile> collectLocalFiles(const QString &rootPath, const QSet<QString> &types, const SyncWorker *worker)
{
    QHash<QString, SyncWorker::LocalFile> files;
    QDirIterator iterator(rootPath, QDir::Files, QDirIterator::Subdirectories);
    const QDir baseDir(rootPath);

    while (iterator.hasNext()) {
        if (worker != nullptr && worker->isCancelled()) {
            break;
        }

        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (!types.contains(info.suffix().toLower())) {
            continue;
        }

        SyncWorker::LocalFile file;
        file.absolutePath = info.absoluteFilePath();
        file.relativePath = QDir::cleanPath(baseDir.relativeFilePath(info.absoluteFilePath())).replace('\\', '/');
        file.size = info.size();
        file.modifiedMs = info.lastModified().toMSecsSinceEpoch();
        files.insert(file.relativePath, file);
    }

    return files;
}

QHash<QString, RemoteFile> collectRemoteFiles(const QString &rootPath, const QSet<QString> &types, const SyncWorker *worker)
{
    QHash<QString, RemoteFile> files;
    QDirIterator iterator(rootPath, QDir::Files, QDirIterator::Subdirectories);
    const QDir baseDir(rootPath);

    while (iterator.hasNext()) {
        if (worker != nullptr && worker->isCancelled()) {
            break;
        }

        iterator.next();
        const QFileInfo info = iterator.fileInfo();
        if (!types.contains(info.suffix().toLower())) {
            continue;
        }

        RemoteFile file;
        file.absolutePath = info.absoluteFilePath();
        file.size = info.size();
        file.modifiedMs = info.lastModified().toMSecsSinceEpoch();
        const QString relativePath = QDir::cleanPath(baseDir.relativeFilePath(info.absoluteFilePath())).replace('\\', '/');
        files.insert(relativePath, file);
    }

    return files;
}

bool sameFile(const SyncWorker::LocalFile &local, const RemoteFile &remote)
{
    return local.size == remote.size && qAbs(local.modifiedMs - remote.modifiedMs) <= 2000;
}

int mismatchPercent(const SyncWorker::ComparisonSummary &summary)
{
    const int mismatchUnits = summary.missingCount + summary.outdatedCount + summary.extraCount + summary.failedCount;
    const int base = qMax(1, summary.localCount + summary.extraCount);
    return qBound(0, qRound(static_cast<double>(mismatchUnits) * 100.0 / static_cast<double>(base)), 100);
}

bool hasSyncBlockingDifference(const SyncWorker::ComparisonSummary &summary)
{
    return summary.missingCount > 0 || summary.outdatedCount > 0 || summary.failedCount > 0;
}

QString syncDetailText(const SyncWorker::ComparisonSummary &summary)
{
    if (summary.extraCount > 0 && !hasSyncBlockingDifference(summary)) {
        return QString("远程有额外文件（%1 个）").arg(summary.extraCount);
    }

    if (summary.extraCount > 0) {
        return summary.detail + "；远端多余文件不会自动删除";
    }

    return summary.detail;
}

QString summaryText(const SyncWorker::ComparisonSummary &summary)
{
    return QString("本地 %1 个，匹配 %2 个，缺失 %3 个，需更新 %4 个，远端多余 %5 个")
        .arg(summary.localCount)
        .arg(summary.matchingCount)
        .arg(summary.missingCount)
        .arg(summary.outdatedCount)
        .arg(summary.extraCount);
}

QString detailTooltipText(const SyncWorker::ComparisonSummary &summary, bool includeExtraRetentionHint)
{
    QStringList lines;
    lines.append(summary.detail);

    if (!summary.extraFiles.isEmpty()) {
        lines.append(QString());
        lines.append(QString("远端多余文件 %1 个：").arg(summary.extraFiles.size()));

        const int visibleCount = qMin(summary.extraFiles.size(), 80);
        for (int index = 0; index < visibleCount; ++index) {
            lines.append(nativePath(summary.extraFiles.at(index)));
        }
        if (summary.extraFiles.size() > visibleCount) {
            lines.append(QString("...... 另外还有 %1 个").arg(summary.extraFiles.size() - visibleCount));
        }
        if (includeExtraRetentionHint) {
            lines.append(QString());
            lines.append("提示：远端多余文件不会自动删除");
        }
    }

    return lines.join("\n");
}

int recommendedRobocopyThreads(const QVector<PendingCopy> &pendingCopies)
{
    const int pendingCount = pendingCopies.size();
    if (pendingCount <= 1) {
        return pendingCount;
    }

    const int idealThreadCount = QThread::idealThreadCount();
    const int cpuCount = qMax(4, idealThreadCount > 0 ? idealThreadCount : 4);

    qint64 totalBytes = 0;
    int tinyFileCount = 0;
    int smallFileCount = 0;
    for (const PendingCopy &copy : pendingCopies) {
        totalBytes += copy.size;
        if (copy.size <= 256 * 1024) {
            tinyFileCount++;
        }
        if (copy.size <= 1024 * 1024) {
            smallFileCount++;
        }
    }

    const qint64 averageBytes = pendingCount > 0 ? totalBytes / pendingCount : 0;
    int threadCount = cpuCount * 4;
    if (pendingCount >= 4000 || averageBytes <= 256 * 1024 || tinyFileCount * 10 >= pendingCount * 7) {
        threadCount = cpuCount * 16;
    } else if (pendingCount >= 1200 || averageBytes <= 1024 * 1024 || smallFileCount * 10 >= pendingCount * 7) {
        threadCount = cpuCount * 12;
    } else if (pendingCount >= 300 || averageBytes <= 8 * 1024 * 1024) {
        threadCount = cpuCount * 8;
    }

    return qMin(pendingCount, qBound(8, threadCount, 128));
}

QStringList robocopyPatterns(const QSet<QString> &types)
{
    QStringList patterns;
    for (const QString &type : types) {
        patterns.append(QString("*.%1").arg(type));
    }
    patterns.removeDuplicates();
    std::sort(patterns.begin(), patterns.end());
    return patterns;
}

QString formatBytes(qint64 bytes)
{
    if (bytes >= 1024LL * 1024 * 1024) {
        return QString("%1 GB").arg(bytes / (1024.0 * 1024 * 1024), 0, 'f', 2);
    }
    if (bytes >= 1024LL * 1024) {
        return QString("%1 MB").arg(bytes / (1024.0 * 1024), 0, 'f', 1);
    }
    return QString("%1 KB").arg(qMax<qint64>(bytes, 0) / 1024);
}

QString outputTail(const QString &output, int maxLines = 6)
{
    QStringList lines;
    for (const QString &line : output.split(QRegularExpression("[\\r\\n]+"), QString::SkipEmptyParts)) {
        const QString trimmed = line.trimmed();
        if (!trimmed.isEmpty()) {
            lines.append(trimmed);
        }
    }

    if (lines.size() > maxLines) {
        lines = lines.mid(lines.size() - maxLines);
    }
    return lines.join(" | ");
}

#ifdef Q_OS_WIN
QString formatSystemErrorMessage(DWORD errorCode)
{
    wchar_t *buffer = nullptr;
    const DWORD size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER
                                          | FORMAT_MESSAGE_FROM_SYSTEM
                                          | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr,
                                      errorCode,
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPWSTR>(&buffer),
                                      0,
                                      nullptr);
    if (size == 0 || buffer == nullptr) {
        return QString();
    }

    const QString message = QString::fromWCharArray(buffer, static_cast<int>(size)).trimmed();
    LocalFree(buffer);
    return message;
}

QString describeWindowsError(DWORD errorCode)
{
    switch (errorCode) {
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return "目标文件正在被其他程序占用，无法覆盖。请先关闭正在使用该 dll/exe 的程序";
    case ERROR_ACCESS_DENIED:
        return "访问被拒绝，可能是目标文件只读、权限不足，或文件正被程序占用";
    case ERROR_PATH_NOT_FOUND:
        return "目标路径不存在";
    case ERROR_FILE_NOT_FOUND:
        return "源文件不存在";
    case ERROR_NETWORK_ACCESS_DENIED:
        return "没有网络共享写入权限";
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_NETNAME_DELETED:
        return "网络共享路径不可用、连接已中断，或远程主机未开机";
    case ERROR_DISK_FULL:
        return "目标磁盘空间不足";
    default:
        return formatSystemErrorMessage(errorCode);
    }
}

int extractRobocopyErrorCode(const QString &output)
{
    static const QRegularExpression pattern("ERROR\\s+(\\d+)", QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatchIterator iterator = pattern.globalMatch(output);
    int errorCode = -1;
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        errorCode = match.captured(1).toInt();
    }
    return errorCode;
}

QString describeRobocopyFailure(int exitCode, const QString &output)
{
    const int errorCode = extractRobocopyErrorCode(output);
    if (errorCode >= 0) {
        const QString reason = describeWindowsError(static_cast<DWORD>(errorCode));
        if (!reason.isEmpty()) {
            return QString("robocopy 同步失败：%1（错误码 %2，退出码 %3）")
                .arg(reason)
                .arg(errorCode)
                .arg(exitCode);
        }
        return QString("robocopy 同步失败，错误码 %1，退出码 %2").arg(errorCode).arg(exitCode);
    }

    const QString tail = outputTail(output);
    if (!tail.isEmpty()) {
        return QString("robocopy 同步失败，退出码 %1：%2").arg(exitCode).arg(tail);
    }

    return QString("robocopy 同步失败，退出码 %1").arg(exitCode);
}

class ShareConnection
{
public:
    explicit ShareConnection(const QString &fullPath)
        : m_rootPath(uncShareRoot(fullPath))
    {
    }

    bool connect(const QString &username, const QString &password, QString *error)
    {
        if (!m_rootPath.startsWith("\\\\")) {
            return true;
        }

        std::wstring remote = m_rootPath.toStdWString();
        NETRESOURCEW resource;
        ZeroMemory(&resource, sizeof(resource));
        resource.dwType = RESOURCETYPE_DISK;
        resource.lpRemoteName = remote.data();

        std::wstring userValue = username.toStdWString();
        std::wstring passValue = password.toStdWString();

        auto tryConnect = [&](const wchar_t *userPtr, const wchar_t *passPtr) -> DWORD {
            return WNetAddConnection2W(&resource,
                                       password.isEmpty() ? L"" : passPtr,
                                       username.isEmpty() ? nullptr : userPtr,
                                       0);
        };

        DWORD result = tryConnect(userValue.c_str(), passValue.c_str());
        if (result == NO_ERROR) {
            m_created = true;
            return true;
        }
        if (result == ERROR_ALREADY_ASSIGNED || result == ERROR_DEVICE_ALREADY_REMEMBERED) {
            return true;
        }
        if (result == ERROR_SESSION_CREDENTIAL_CONFLICT) {
            WNetCancelConnection2W(remote.c_str(), 0, TRUE);
            result = tryConnect(userValue.c_str(), passValue.c_str());
            if (result == NO_ERROR) {
                m_created = true;
                return true;
            }
        }

        if (error != nullptr) {
            const QString reason = describeWindowsError(result);
            *error = reason.isEmpty()
                ? QString("共享连接失败，错误码 %1").arg(result)
                : QString("共享连接失败：%1（错误码 %2）").arg(reason).arg(result);
        }
        return false;
    }

    ~ShareConnection()
    {
        if (!m_created) {
            return;
        }

        const std::wstring remote = m_rootPath.toStdWString();
        WNetCancelConnection2W(remote.c_str(), 0, TRUE);
    }

private:
    QString m_rootPath;
    bool m_created = false;
};

bool runRobocopy(SyncWorker *worker,
                 const QString &localRoot,
                 const QString &remoteRoot,
                 const QVector<PendingCopy> &pendingCopies,
                 const QSet<QString> &types,
                 int threadCount,
                 QString *error)
{
    if (worker == nullptr || worker->isCancelled()) {
        if (error != nullptr) {
            *error = QString("同步未启动或已被用户停止");
        }
        return false;
    }

    if (pendingCopies.isEmpty()) {
        return true;
    }

    QStringList arguments;
    arguments << nativePath(localRoot) << nativePath(remoteRoot);
    arguments.append(robocopyPatterns(types));
    arguments << "/S"
              << "/FFT"
              << "/R:0"
              << "/W:0"
              << QString("/MT:%1").arg(threadCount)
              << "/COPY:DAT"
              << "/DCOPY:DA"
              << "/XX"
              << "/NFL"
              << "/NDL"
              << "/NJH"
              << "/NJS"
              << "/NP";

    qint64 totalBytes = 0;
    for (const PendingCopy &copy : pendingCopies) {
        totalBytes += copy.size;
    }
    if (pendingCopies.size() > 0 && totalBytes / pendingCopies.size() >= 32 * 1024 * 1024) {
        arguments << "/J";
    }

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start("robocopy", arguments);
    if (!process.waitForStarted(5000)) {
        if (error != nullptr) {
            *error = QString("无法启动 robocopy：%1").arg(process.errorString());
        }
        return false;
    }

    // /MT 多线程下各文件并行复制、几乎同时完成，按文件个数统计会从 0 直接跳到 100；
    // 目标文件又会被预分配到完整大小，也无法按目标文件大小统计。
    // 因此读取 robocopy 进程累计写入字节数（GetProcessIoCounters）作为真实复制进度。
    HANDLE processHandle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process.processId()));
    struct HandleCloser
    {
        HANDLE handle;
        ~HandleCloser()
        {
            if (handle != nullptr) {
                CloseHandle(handle);
            }
        }
    } handleCloser{processHandle};

    qint64 writtenBytes = 0;
    int elapsedSeconds = 0;
    int elapsedTicks = 0;
    auto emitCopyProgress = [&]() {
        IO_COUNTERS counters = {};
        if (processHandle != nullptr && GetProcessIoCounters(processHandle, &counters)) {
            writtenBytes = qMin(static_cast<qint64>(counters.WriteTransferCount), totalBytes);
        }
        const int percent = totalBytes > 0 ? static_cast<int>(writtenBytes * 99 / totalBytes) : 0;
        worker->emitProgress(qMax(1, percent),
                             QString("robocopy 同步中，%1 线程，%2 个文件，已复制 %3 / %4，已运行 %5 秒")
                                 .arg(threadCount)
                                 .arg(pendingCopies.size())
                                 .arg(formatBytes(writtenBytes))
                                 .arg(formatBytes(totalBytes))
                                 .arg(elapsedSeconds));
    };

    emitCopyProgress();

    // 每 500ms 刷新一次进度并检查取消；进程已结束时 waitForFinished 也返回 false，所以用 state() 判断
    while (process.state() != QProcess::NotRunning) {
        if (worker->isCancelled()) {
            process.kill();
            process.waitForFinished(3000);
            if (error != nullptr) {
                *error = QString("同步已被用户取消");
            }
            return false;
        }
        if (process.waitForFinished(500)) {
            break;
        }
        elapsedTicks++;
        elapsedSeconds = elapsedTicks / 2;
        emitCopyProgress();
    }

    const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
    if (process.exitStatus() != QProcess::NormalExit) {
        if (error != nullptr) {
            *error = QString("robocopy 异常退出：%1").arg(outputTail(output));
        }
        return false;
    }

    const int exitCode = process.exitCode();
    if (exitCode >= 8) {
        if (error != nullptr) {
            *error = describeRobocopyFailure(exitCode, output);
        }
        return false;
    }

    return true;
}
#endif

SyncWorker::ComparisonSummary compareFiles(const QHash<QString, SyncWorker::LocalFile> &localFiles,
                                           const QHash<QString, RemoteFile> &remoteFiles,
                                           QVector<PendingCopy> *pendingCopies,
                                           const QString &remoteRoot)
{
    SyncWorker::ComparisonSummary summary;
    summary.localCount = localFiles.size();

    for (auto it = localFiles.constBegin(); it != localFiles.constEnd(); ++it) {
        const QString relativePath = it.key();
        const SyncWorker::LocalFile &local = it.value();
        const auto remoteIt = remoteFiles.constFind(relativePath);
        if (remoteIt == remoteFiles.constEnd()) {
            summary.missingCount++;
            summary.totalCopyBytes += local.size;
            if (pendingCopies != nullptr) {
                PendingCopy copy;
                copy.sourcePath = local.absolutePath;
                copy.destinationPath = QDir(remoteRoot).filePath(relativePath);
                copy.relativePath = relativePath;
                copy.size = local.size;
                pendingCopies->push_back(copy);
            }
            continue;
        }

        if (sameFile(local, remoteIt.value())) {
            summary.matchingCount++;
        } else {
            summary.outdatedCount++;
            summary.totalCopyBytes += local.size;
            if (pendingCopies != nullptr) {
                PendingCopy copy;
                copy.sourcePath = local.absolutePath;
                copy.destinationPath = remoteIt.value().absolutePath;
                copy.relativePath = relativePath;
                copy.size = local.size;
                pendingCopies->push_back(copy);
            }
        }
    }

    for (auto it = remoteFiles.constBegin(); it != remoteFiles.constEnd(); ++it) {
        if (!localFiles.contains(it.key())) {
            summary.extraCount++;
            summary.extraFiles.append(it.key());
        }
    }

    std::sort(summary.extraFiles.begin(), summary.extraFiles.end());
    summary.detail = summaryText(summary);
    return summary;
}
}

SyncWorker::SyncWorker(HostEntry host, QString releaseDir, QStringList fileTypes, bool performCopy, QObject *parent)
    : QObject(parent)
    , m_host(host)
    , m_releaseDir(releaseDir)
    , m_fileTypes(fileTypes)
    , m_performCopy(performCopy)
    , m_cancelled(std::make_shared<std::atomic<bool>>(false))
{
}

void SyncWorker::cancel()
{
    m_cancelled->store(true);
}

bool SyncWorker::isCancelled() const
{
    return m_cancelled->load();
}

std::shared_ptr<std::atomic<bool>> SyncWorker::cancellationFlag() const
{
    return m_cancelled;
}

void SyncWorker::emitProgress(int percent, const QString &detail)
{
    emit progressChanged(m_host.id, percent, detail);
}

void SyncWorker::emitState(int state,
                           int mismatchPercentValue,
                           const QString &statusText,
                           const QString &detail,
                           const QString &detailTooltip)
{
    emit stateChanged(m_host.id, state, mismatchPercentValue, statusText, detail, detailTooltip);
}

void SyncWorker::emitFinished()
{
    emit finished(m_host.id);
}

void SyncWorker::finishWithError(int state, const QString &statusText, const QString &detail)
{
    if (finishIfCancelled()) {
        return;
    }

    emitState(state, 100, statusText, detail, detail);
    emitFinished();
}

bool SyncWorker::finishIfCancelled()
{
    if (!isCancelled()) {
        return false;
    }

    const QString detail = QString("%1：操作已被用户停止").arg(hostDisplayName(m_host));
    emitState(Stopped, 0, "已停止", detail, detail);
    emitFinished();
    return true;
}

QString SyncWorker::normalizedReleaseDir() const
{
    return normalizeQtPath(m_releaseDir);
}

QString SyncWorker::normalizedRemoteDir() const
{
    return normalizeQtPath(m_host.sharePath);
}

void SyncWorker::process()
{
    if (finishIfCancelled()) {
        return;
    }

    const QString localRoot = normalizedReleaseDir();
    const QString remoteRoot = normalizedRemoteDir();
    const QString displayName = hostDisplayName(m_host);

    if (localRoot.isEmpty() || !QFileInfo::exists(localRoot)) {
        finishWithError(Error, "本地目录无效", "请先配置有效的本地 release 目录");
        return;
    }
    if (remoteRoot.isEmpty()) {
        finishWithError(Error, "共享目录为空", "请为主机配置共享目录");
        return;
    }

    QSet<QString> typeSet;
    for (const QString &type : m_fileTypes) {
        const QString normalized = normalizeExtension(type);
        if (!normalized.isEmpty()) {
            typeSet.insert(normalized);
        }
    }
    if (typeSet.isEmpty()) {
        finishWithError(Error, "未选择类型", "请至少勾选一种文件类型");
        return;
    }

    emitState(Checking,
              0,
              "检查中",
              QString("%1：正在连接共享目录").arg(displayName));

#ifdef Q_OS_WIN
    ShareConnection connection(remoteRoot);
    QString connectionError;
    if (!connection.connect(m_host.username, m_host.password, &connectionError)) {
        finishWithError(Disconnected, "断线", QString("%1：%2").arg(displayName, connectionError));
        return;
    }
#else
    finishWithError(Error, "平台不支持", "当前版本仅支持 Windows 下的 robocopy 同步");
    return;
#endif

    if (finishIfCancelled()) {
        return;
    }

    if (!QFileInfo::exists(remoteRoot)) {
        const QDir dir;
        if (!dir.mkpath(remoteRoot)) {
            finishWithError(Disconnected, "断线", QString("%1：共享路径不可达").arg(displayName));
            return;
        }
    }

    if (finishIfCancelled()) {
        return;
    }

    const QHash<QString, LocalFile> localFiles = collectLocalFiles(localRoot, typeSet, this);
    if (finishIfCancelled()) {
        return;
    }

    const QHash<QString, RemoteFile> remoteFiles = collectRemoteFiles(remoteRoot, typeSet, this);
    if (finishIfCancelled()) {
        return;
    }

    QVector<PendingCopy> pendingCopies;
    ComparisonSummary summary = compareFiles(localFiles, remoteFiles, m_performCopy ? &pendingCopies : nullptr, remoteRoot);
    const int initialMismatch = mismatchPercent(summary);

    if (finishIfCancelled()) {
        return;
    }

    if (!m_performCopy) {
        emitState(initialMismatch == 0 ? InSync : Mismatch,
                  initialMismatch,
                  initialMismatch == 0 ? "完全一致" : "不一致",
                  summary.detail,
                  detailTooltipText(summary, false));
        emitFinished();
        return;
    }

    if (pendingCopies.isEmpty()) {
        const bool hasBlockingDifference = hasSyncBlockingDifference(summary);
        emitProgress(100, "无需复制");
        emitState(hasBlockingDifference ? Mismatch : InSync,
                  hasBlockingDifference ? initialMismatch : 0,
                  hasBlockingDifference ? "部分不一致" : "已同步",
                  syncDetailText(summary),
                  detailTooltipText(summary, summary.extraCount > 0));
        emitFinished();
        return;
    }

    if (finishIfCancelled()) {
        return;
    }

    const int threadCount = recommendedRobocopyThreads(pendingCopies);
    emitProgress(0, QString("待同步 %1 个文件，robocopy %2 线程").arg(pendingCopies.size()).arg(threadCount));
    emitState(Syncing,
              initialMismatch,
              "同步中",
              QString("%1：robocopy 准备同步 %2 个文件，%3 线程")
                  .arg(displayName)
                  .arg(pendingCopies.size())
                  .arg(threadCount));

    QString copyError;
    if (!runRobocopy(this, localRoot, remoteRoot, pendingCopies, typeSet, threadCount, &copyError)) {
        summary.failedCount++;
        finishWithError(Error, "同步失败", copyError);
        return;
    }

    if (finishIfCancelled()) {
        return;
    }

    ComparisonSummary finalSummary = summary;
    finalSummary.matchingCount = finalSummary.localCount;
    finalSummary.missingCount = 0;
    finalSummary.outdatedCount = 0;
    finalSummary.failedCount = 0;
    finalSummary.detail = summaryText(finalSummary);
    const int finalMismatch = mismatchPercent(finalSummary);
    const bool hasBlockingDifference = hasSyncBlockingDifference(finalSummary);

    emitProgress(100, hasBlockingDifference ? "同步完成但仍有差异" : "同步完成");
    emitState(hasBlockingDifference ? Mismatch : InSync,
              hasBlockingDifference ? finalMismatch : 0,
              hasBlockingDifference ? "仍有差异" : "已同步",
              syncDetailText(finalSummary),
              detailTooltipText(finalSummary, finalSummary.extraCount > 0));
    if (!hasBlockingDifference && !isCancelled()) {
        emit copySucceeded(m_host.id, QDateTime::currentDateTimeUtc());
    }
    emitFinished();
}
