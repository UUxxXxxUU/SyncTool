#pragma execution_character_set("utf-8")

#include "syncworker.h"

#include <QByteArray>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QMap>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
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
using RemoteFile = SyncPlan::File;

struct PendingCopy
{
    QString sourcePath;
    QString destinationPath;
    QString relativePath;
    qint64 size = 0;
};

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

        for (const QString &path : summary.extraFiles) {
            lines.append(nativePath(path));
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

    // 线程过多会占满到同一主机的 SMB 通道，导致资源管理器浏览该共享时长时间无响应。
    return qMin(pendingCount, qBound(8, threadCount, 32));
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
        return "远端文件正在被程序占用，无法覆盖。请关闭远端正在使用该文件的程序后重试。";
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
    static const QRegularExpression pattern("(?:ERROR|错误)\\s+(\\d+)", QRegularExpression::CaseInsensitiveOption);
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

// 本程序创建的共享连接（键为忽略大小写的共享根路径）。连接在程序运行期间保持，
// 不在每轮检查后断开：资源管理器与本程序共用同一登录会话的 SMB 连接，断开会让已打开的共享窗口失效。
QMutex &shareConnectionMutex()
{
    static QMutex mutex;
    return mutex;
}

QHash<QString, QString> &createdShareConnections()
{
    static QHash<QString, QString> connections;
    return connections;
}

bool isCreatedShareConnection(const QString &rootPath)
{
    QMutexLocker locker(&shareConnectionMutex());
    return createdShareConnections().contains(rootPath.toCaseFolded());
}

void rememberCreatedShareConnection(const QString &rootPath)
{
    QMutexLocker locker(&shareConnectionMutex());
    createdShareConnections().insert(rootPath.toCaseFolded(), rootPath);
}

void forgetCreatedShareConnection(const QString &rootPath)
{
    QMutexLocker locker(&shareConnectionMutex());
    createdShareConnections().remove(rootPath.toCaseFolded());
}

bool isShareAccessible(const QString &rootPath)
{
    const std::wstring path = (rootPath + "\\").toStdWString();
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool connectShare(const QString &fullPath, const QString &username, const QString &password, QString *error)
{
    const QString rootPath = uncShareRoot(fullPath);
    if (!rootPath.startsWith("\\\\")) {
        return true;
    }

    // 未填写凭据时使用 Windows 当前会话，避免向已可访问的共享提交空密码。
    if (username.isEmpty() && password.isEmpty() && isShareAccessible(rootPath)) {
        return true;
    }

    if (isCreatedShareConnection(rootPath) && isShareAccessible(rootPath)) {
        return true;
    }

    std::wstring remote = rootPath.toStdWString();
    NETRESOURCEW resource;
    ZeroMemory(&resource, sizeof(resource));
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = remote.data();

    const std::wstring userValue = username.toStdWString();
    const std::wstring passValue = password.toStdWString();

    auto tryConnect = [&]() -> DWORD {
        return WNetAddConnection2W(&resource,
                                   password.isEmpty() ? (username.isEmpty() ? nullptr : L"") : passValue.c_str(),
                                   username.isEmpty() ? nullptr : userValue.c_str(),
                                   0);
    };

    DWORD result = tryConnect();
    if (result == NO_ERROR) {
        rememberCreatedShareConnection(rootPath);
        return true;
    }
    if (result == ERROR_ALREADY_ASSIGNED || result == ERROR_DEVICE_ALREADY_REMEMBERED) {
        return true;
    }
    if (result == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        // 当前会话已用其他账号连接该主机（通常是用户在资源管理器中打开的），不能断开它，直接复用。
        if (isShareAccessible(rootPath)) {
            return true;
        }
        // 只重建本程序自己创建的连接（例如账号已修改），且不强制关闭已打开的文件。
        if (isCreatedShareConnection(rootPath)
            && WNetCancelConnection2W(remote.c_str(), 0, FALSE) == NO_ERROR) {
            forgetCreatedShareConnection(rootPath);
            result = tryConnect();
            if (result == NO_ERROR) {
                rememberCreatedShareConnection(rootPath);
                return true;
            }
        }
    }

    if (error != nullptr) {
        if (result == ERROR_SESSION_CREDENTIAL_CONFLICT) {
            *error = QString("共享连接失败：当前 Windows 登录已用其他账号连接该主机，且无权访问此共享。"
                             "请在资源管理器中改用相同账号，或断开现有连接后重试（错误码 %1）")
                         .arg(result);
        } else {
            const QString reason = describeWindowsError(result);
            *error = reason.isEmpty()
                ? QString("共享连接失败，错误码 %1").arg(result)
                : QString("共享连接失败：%1（错误码 %2）").arg(reason).arg(result);
        }
    }
    return false;
}

// Copy only the planned files. Per-directory batches also support ../ rules and
// overlapping filters without copying a file twice or widening the selected scope.
bool runRobocopy(SyncWorker *worker, const QString &localBase,
                 const QVector<PendingCopy> &pendingCopies, int threadCount, QString *error)
{
    struct Batch { QString sourceDir; QString destinationDir; QVector<PendingCopy> files; };
    QVector<Batch> batches;
    QMap<QString, QVector<PendingCopy>> groups;
    qint64 totalBytes = 0;
    QStringList pendingFiles;
    for (const PendingCopy &copy : pendingCopies) {
        const QString key = QFileInfo(copy.sourcePath).absolutePath().toCaseFolded() + '\n'
            + QFileInfo(copy.destinationPath).absolutePath().toCaseFolded();
        groups[key].append(copy);
        totalBytes += copy.size;
        pendingFiles.append(copy.relativePath);
    }
    for (auto group = groups.begin(); group != groups.end(); ++group) {
        auto &files = group.value();
        std::sort(files.begin(), files.end(), [](const PendingCopy &a, const PendingCopy &b) { return a.relativePath < b.relativePath; });
        Batch batch;
        batch.sourceDir = QFileInfo(files.first().sourcePath).absolutePath();
        batch.destinationDir = QFileInfo(files.first().destinationPath).absolutePath();
        int commandLength = batch.sourceDir.size() + batch.destinationDir.size() + 512;
        for (const PendingCopy &copy : files) {
            const int nameLength = QFileInfo(copy.sourcePath).fileName().size() + 3;
            if (!batch.files.isEmpty() && commandLength + nameLength > 24000) {
                batches.append(batch);
                batch.files.clear();
                commandLength = batch.sourceDir.size() + batch.destinationDir.size() + 512;
            }
            batch.files.append(copy);
            commandLength += nameLength;
        }
        if (!batch.files.isEmpty()) batches.append(batch);
    }
    pendingFiles.sort(Qt::CaseInsensitive);
    worker->emitRecentFiles(pendingFiles, QString("本轮待复制文件（共 %1 个）").arg(pendingFiles.size()));
    QStringList processed;
    qint64 completedBytes = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    for (int batchIndex = 0; batchIndex < batches.size(); ++batchIndex) {
        if (worker->isCancelled()) { *error = "同步已被用户取消"; return false; }
        const Batch &batch = batches.at(batchIndex);
        QStringList arguments{nativePath(batch.sourceDir), nativePath(batch.destinationDir)};
        qint64 batchBytes = 0;
        for (const PendingCopy &copy : batch.files) {
            arguments.append(QFileInfo(copy.sourcePath).fileName());
            batchBytes += copy.size;
        }
        // No /S: the explicit file list is scoped to this one directory.
        arguments << "/FFT" << "/R:0" << "/W:0" << QString("/MT:%1").arg(qMax(1, threadCount))
                  << "/COPY:DAT" << "/DCOPY:DA" << "/XX" << "/FP" << "/NC" << "/NS"
                  << "/NDL" << "/NJH" << "/NJS" << "/NP";
        if (!batch.files.isEmpty() && batchBytes / batch.files.size() >= 32 * 1024 * 1024) arguments << "/J";
        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start("robocopy", arguments);
        if (!process.waitForStarted(5000)) {
            *error = QString("无法启动 robocopy：%1").arg(process.errorString());
            return false;
        }
        HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process.processId()));
        struct HandleCloser { HANDLE handle; ~HandleCloser() { if (handle) CloseHandle(handle); } } closer{handle};
        QByteArray output;
        const QString scope = QDir(localBase).relativeFilePath(batch.sourceDir);
        auto progress = [&]() {
            IO_COUNTERS counters = {};
            const qint64 written = handle && GetProcessIoCounters(handle, &counters)
                ? qMin(static_cast<qint64>(counters.WriteTransferCount), batchBytes) : 0;
            const int percent = totalBytes > 0 ? static_cast<int>((completedBytes + written) * 99 / totalBytes)
                : batchIndex * 99 / qMax(1, batches.size());
            worker->emitProgress(qBound(0, percent, 99), QString("同步 %1（第 %2/%3 组），已复制 %4 / %5，已运行 %6 秒")
                .arg(nativePath(scope)).arg(batchIndex + 1).arg(batches.size())
                .arg(formatBytes(completedBytes + written), formatBytes(totalBytes)).arg(elapsed.elapsed() / 1000));
        };
        progress();
        while (process.state() != QProcess::NotRunning) {
            if (worker->isCancelled()) {
                process.kill();
                process.waitForFinished(3000);
                worker->emitRecentFiles(processed, QString("本轮处理记录（已取消，共 %1 个）").arg(processed.size()));
                *error = "同步已被用户取消";
                return false;
            }
            process.waitForFinished(200);
            output.append(process.readAllStandardOutput());
            if (output.size() > 65536) output = output.right(65536);
            progress();
        }
        output.append(process.readAllStandardOutput());
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() >= 8) {
            *error = QString("%1：%2").arg(nativePath(scope), process.exitStatus() == QProcess::NormalExit
                ? describeRobocopyFailure(process.exitCode(), QString::fromLocal8Bit(output))
                : QString("robocopy 异常退出：%1").arg(outputTail(QString::fromLocal8Bit(output))));
            worker->emitRecentFiles(processed, QString("本轮处理记录（同步失败，共 %1 个）").arg(processed.size()));
            return false;
        }
        for (const PendingCopy &copy : batch.files) processed.prepend(copy.relativePath);
        completedBytes += batchBytes;
        worker->emitRecentFiles(processed, QString("本轮处理记录（共 %1 个，最新在前）").arg(processed.size()));
    }
    return !worker->isCancelled();
}
#endif

SyncWorker::ComparisonSummary compareFiles(const QHash<QString, SyncWorker::LocalFile> &localFiles,
                                           const QHash<QString, RemoteFile> &remoteFiles,
                                           QVector<PendingCopy> *pendingCopies,
                                           const QString &remoteRoot)
{
    SyncWorker::ComparisonSummary summary;
    summary.localCount = localFiles.size();

    // Windows 共享路径忽略大小写匹配，原始路径仍用于复制和显示。
    QHash<QString, const RemoteFile *> remotePaths;
    remotePaths.reserve(remoteFiles.size());
    for (auto it = remoteFiles.constBegin(); it != remoteFiles.constEnd(); ++it) {
        remotePaths.insert(it.key().toCaseFolded(), &it.value());
    }
    QSet<QString> localPaths;
    localPaths.reserve(localFiles.size());

    for (auto it = localFiles.constBegin(); it != localFiles.constEnd(); ++it) {
        const QString relativePath = it.value().relativePath;
        const QString pathKey = relativePath.toCaseFolded();
        localPaths.insert(pathKey);
        const SyncWorker::LocalFile &local = it.value();
        const auto remoteIt = remotePaths.constFind(pathKey);
        if (remoteIt == remotePaths.constEnd()) {
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

        const RemoteFile &remote = *remoteIt.value();
        if (sameFile(local, remote)) {
            summary.matchingCount++;
        } else {
            summary.outdatedCount++;
            summary.totalCopyBytes += local.size;
            if (pendingCopies != nullptr) {
                PendingCopy copy;
                copy.sourcePath = local.absolutePath;
                copy.destinationPath = remote.absolutePath;
                copy.relativePath = relativePath;
                copy.size = local.size;
                pendingCopies->push_back(copy);
            }
        }
    }

    for (auto it = remoteFiles.constBegin(); it != remoteFiles.constEnd(); ++it) {
        if (!localPaths.contains(it.key().toCaseFolded())) {
            summary.extraCount++;
            summary.extraFiles.append(it.value().relativePath);
        }
    }

    std::sort(summary.extraFiles.begin(), summary.extraFiles.end());
    summary.detail = summaryText(summary);
    return summary;
}
}

SyncWorker::SyncWorker(HostEntry host, QString releaseDir, QStringList fileTypes, bool performCopy, QVector<SyncRule> rules, QObject *parent)
    : QObject(parent)
    , m_host(host)
    , m_releaseDir(releaseDir)
    , m_fileTypes(fileTypes)
    , m_rules(std::move(rules))
    , m_performCopy(performCopy)
    , m_cancelled(std::make_shared<std::atomic<bool>>(false))
{
}

void SyncWorker::releaseShareConnections()
{
#ifdef Q_OS_WIN
    QHash<QString, QString> connections;
    {
        QMutexLocker locker(&shareConnectionMutex());
        connections.swap(createdShareConnections());
    }
    for (const QString &rootPath : connections) {
        const std::wstring remote = rootPath.toStdWString();
        WNetCancelConnection2W(remote.c_str(), 0, FALSE);
    }
#endif
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

void SyncWorker::emitRecentFiles(const QStringList &files, const QString &title)
{
    emit recentFilesChanged(m_host.id, files, title);
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
    return m_releaseDir.trimmed().isEmpty() ? QString() : normalizeQtPath(m_releaseDir);
}

QString SyncWorker::normalizedRemoteDir() const
{
    return m_host.sharePath.trimmed().isEmpty() ? QString() : normalizeQtPath(m_host.sharePath);
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

    emitState(Checking,
              0,
              "检查中",
              QString("%1：正在连接共享目录").arg(displayName));

#ifdef Q_OS_WIN
    QString connectionError;
    if (!connectShare(remoteRoot, m_host.username, m_host.password, &connectionError)) {
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

    QVector<SyncPlan::Scope> scopes;
    QString planError;
    if (!SyncPlan::build(localRoot, remoteRoot, m_fileTypes, m_rules, &scopes, &planError)) {
        finishWithError(Error, "同步规则无效", planError);
        return;
    }
    const auto localFiles = SyncPlan::collect(localRoot, scopes, true, m_cancelled.get(), &planError);
    if (finishIfCancelled()) return;
    if (!planError.isEmpty()) { finishWithError(Error, "本地扫描失败", planError); return; }
    const auto remoteFiles = SyncPlan::collect(remoteRoot, scopes, false, m_cancelled.get(), &planError);
    if (finishIfCancelled()) return;
    if (!planError.isEmpty()) { finishWithError(Error, "远端扫描失败", planError); return; }
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
    if (!runRobocopy(this, localRoot, pendingCopies, threadCount, &copyError)) {
        summary.failedCount++;
        finishWithError(Error, "同步失败", copyError);
        return;
    }

    if (finishIfCancelled()) {
        return;
    }

    // Re-read the destination instead of assuming that a successful process copied every file.
    const auto finalRemoteFiles = SyncPlan::collect(remoteRoot, scopes, false, m_cancelled.get(), &planError);
    if (finishIfCancelled()) return;
    if (!planError.isEmpty()) { finishWithError(Error, "同步后校验失败", planError); return; }
    ComparisonSummary finalSummary = compareFiles(localFiles, finalRemoteFiles, nullptr, remoteRoot);
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
