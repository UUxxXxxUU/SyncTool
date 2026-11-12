#pragma execution_character_set("utf-8")
#include "syncplan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QRegularExpression>
#include <QDateTime>
#include <algorithm>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{
QString clean(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
}

bool isWithin(const QString &path, const QString &root)
{
    return path.compare(root, Qt::CaseInsensitive) == 0
        || path.startsWith(root.endsWith('/') ? root : root + '/', Qt::CaseInsensitive);
}

QString shareRoot(const QString &path)
{
    if (!path.startsWith("//")) {
        return QString();
    }
    const QStringList parts = path.mid(2).split('/', QString::SkipEmptyParts);
    return parts.size() >= 2 ? "//" + parts.at(0) + '/' + parts.at(1) : QString();
}

bool reparsePoint(const QString &path)
{
#ifdef Q_OS_WIN
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()));
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        return true;
    }
#endif
    return QFileInfo(path).isSymLink();
}

bool validateAncestors(QString path, QString *error)
{
    while (!path.isEmpty()) {
        if (reparsePoint(path)) {
            *error = QString("路径包含符号链接或目录联接，请改用实际目录：%1").arg(QDir::toNativeSeparators(path));
            return false;
        }
        const QString parent = clean(QFileInfo(path).absolutePath());
        if (parent == path || path == shareRoot(path)) {
            break;
        }
        path = parent;
    }
    return true;
}
}

bool SyncPlan::normalizeTypes(const QStringList &values, QStringList *types, QString *error)
{
    types->clear();
    for (QString value : values) {
        value = value.trimmed().toLower();
        if (value.startsWith("*.")) {
            value.remove(0, 2);
        }
        while (value.startsWith('.')) {
            value.remove(0, 1);
        }
        if (value.isEmpty() || value.contains(QRegularExpression("[\\s\\\\/:*?\"<>|.;,]"))) {
            if (error) *error = QString("无效的文件类型：%1；请填写 png、.qm 或 *.dll，多种类型用分号分隔。").arg(value);
            return false;
        }
        if (!types->contains(value)) types->append(value);
    }
    types->sort();
    return true;
}

bool SyncPlan::normalizeRelativePath(const QString &value, QString *path, QString *error)
{
    const QString input = QDir::fromNativeSeparators(value.trimmed());
    if (input.isEmpty() || input.startsWith('/') || input.contains(QRegularExpression("[:*?\"<>|\\x00-\\x1f]"))) {
        if (error) *error = "请填写相对 Release 的路径，例如 ./image 或 ../Lang。";
        return false;
    }
    for (const QString &part : input.split('/', QString::SkipEmptyParts)) {
        if (part == "." || part == "..") continue;
        if (part.endsWith('.') || part.endsWith(' ') || part.contains(QRegularExpression("^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\\.|$)", QRegularExpression::CaseInsensitiveOption))) {
            if (error) *error = QString("路径包含 Windows 不支持的名称：%1").arg(part);
            return false;
        }
    }
    *path = clean(input);
    return true;
}

QString SyncPlan::resolvePath(const QString &base, const QString &relativePath)
{
    if (base.trimmed().isEmpty()) return QString();
    return clean(QDir(clean(base)).absoluteFilePath(relativePath));
}

bool SyncPlan::validateRemotePath(const QString &base, const QString &relativePath, QString *error)
{
    if (base.trimmed().isEmpty()) return true;
    const QString remote = clean(base);
    const QString share = shareRoot(remote);
    if (!QDir::isAbsolutePath(remote) || (remote.startsWith("//") && share.isEmpty())) {
        *error = "请填写完整的远端路径，例如 \\\\SERVER\\share\\Release。";
        return false;
    }
    if (!share.isEmpty() && !isWithin(resolvePath(remote, relativePath), share)) {
        *error = QString("规则 %1 超出了共享目录 %2。").arg(relativePath, QDir::toNativeSeparators(share));
        return false;
    }
    return true;
}

bool SyncPlan::build(const QString &localBase, const QString &remoteBase, const QStringList &baseTypes,
                     const QVector<SyncRule> &rules, QVector<Scope> *scopes, QString *error)
{
    error->clear();
    scopes->clear();
    const QString local = clean(localBase);
    const QString remote = remoteBase.trimmed().isEmpty() ? QString() : clean(remoteBase);
    if (localBase.trimmed().isEmpty() || !QFileInfo(local).isDir()) {
        *error = "请先配置有效的本地 Release 目录。";
        return false;
    }
    if (!validateRemotePath(remoteBase, ".", error)) return false;
    if (!baseTypes.isEmpty()) {
        Scope scope;
        scope.relativePath = ".";
        scope.localPath = local;
        scope.remotePath = remote;
        if (!normalizeTypes(baseTypes, &scope.fileTypes, error)) return false;
        scopes->append(scope);
    }
    for (const SyncRule &rule : rules) {
        if (!rule.enabled) continue;
        Scope scope;
        if (!normalizeRelativePath(rule.relativePath, &scope.relativePath, error)
            || !normalizeTypes(rule.fileTypes, &scope.fileTypes, error)) return false;
        scope.localPath = resolvePath(local, scope.relativePath);
        scope.remotePath = resolvePath(remote, scope.relativePath);
        if (!validateRemotePath(remoteBase, scope.relativePath, error)) return false;
        if (!QFileInfo(scope.localPath).exists()) {
            *error = QString("规则 %1 的本地路径不存在：%2").arg(rule.relativePath, QDir::toNativeSeparators(scope.localPath));
            return false;
        }
        scopes->append(scope);
    }
    if (scopes->isEmpty()) {
        *error = "请勾选基本同步类型，或启用至少一条附加同步规则。";
        return false;
    }
    for (const Scope &scope : *scopes) {
        if (!validateAncestors(scope.localPath, error)) return false;
        if (!remote.isEmpty() && !validateAncestors(scope.remotePath, error)) return false;
        if (remote.isEmpty()) continue;
        for (const Scope &source : *scopes) {
            if (isWithin(scope.remotePath, source.localPath) || isWithin(source.localPath, scope.remotePath)) {
                *error = "本地路径和远端路径不能相同或互相包含。";
                return false;
            }
        }
    }
    return true;
}

SyncPlan::Files SyncPlan::collect(const QString &base, const QVector<Scope> &scopes, bool local,
                                 const std::atomic<bool> *cancelled, QString *error)
{
    error->clear();
    Files files;
    const QDir baseDir(clean(base));
    for (const Scope &scope : scopes) {
        if (cancelled && cancelled->load()) return files;
        const QString path = local ? scope.localPath : scope.remotePath;
        if (!validateAncestors(path, error)) return {};
        const QFileInfo root(path);
        if (!root.exists()) {
            if (!local) continue;
            *error = QString("规则 %1 的本地路径不存在：%2").arg(scope.relativePath, QDir::toNativeSeparators(path));
            return {};
        }
        if (!root.isReadable()) {
            *error = QString("无法读取：%1").arg(QDir::toNativeSeparators(path));
            return {};
        }
        const bool expectsFile = QFileInfo(scope.localPath).isFile();
        if (root.isFile() != expectsFile) {
            *error = QString("文件与目录冲突：%1").arg(QDir::toNativeSeparators(path));
            return {};
        }
        auto addFile = [&](const QFileInfo &info) {
            if (!scope.fileTypes.isEmpty() && !scope.fileTypes.contains(info.suffix().toLower())) return;
            File file;
            file.absolutePath = info.absoluteFilePath();
            file.relativePath = clean(baseDir.relativeFilePath(file.absolutePath));
            file.size = info.size();
            file.modifiedMs = info.lastModified().toMSecsSinceEpoch();
            files.insert(file.relativePath.toCaseFolded(), file);
        };
        if (root.isFile()) {
            addFile(root);
            continue;
        }
        QDirIterator iterator(path, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                              QDirIterator::Subdirectories);
        while (iterator.hasNext()) {
            if (cancelled && cancelled->load()) return files;
            iterator.next();
            const QFileInfo info = iterator.fileInfo();
            if (reparsePoint(info.absoluteFilePath())) {
                *error = QString("规则 %1 包含符号链接或目录联接：%2").arg(scope.relativePath, QDir::toNativeSeparators(info.absoluteFilePath()));
                return {};
            }
            if (!info.isReadable()) {
                *error = QString("无法读取：%1").arg(QDir::toNativeSeparators(info.absoluteFilePath()));
                return {};
            }
            if (info.isFile()) addFile(info);
        }
    }
    return files;
}

QString SyncPlan::fingerprint(const QString &localBase, const QStringList &baseTypes, const QVector<SyncRule> &rules)
{
    QVector<Scope> scopes;
    QString error;
    if (!build(localBase, QString(), baseTypes, rules, &scopes, &error)) return "error:" + error;
    const Files files = collect(localBase, scopes, true, nullptr, &error);
    if (!error.isEmpty()) return "error:" + error;
    QStringList records;
    for (const File &file : files) {
        records.append(QString("%1|%2|%3").arg(file.relativePath.toCaseFolded()).arg(file.size).arg(file.modifiedMs));
    }
    records.sort();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(clean(localBase).toUtf8());
    for (const Scope &scope : scopes) hash.addData(("\nrule:" + scope.relativePath + ':' + scope.fileTypes.join(';')).toUtf8());
    for (const QString &record : records) hash.addData(('\n' + record).toUtf8());
    return QString::fromLatin1(hash.result().toHex());
}
