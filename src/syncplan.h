#pragma once

#include "appconfig.h"
#include <QHash>
#include <atomic>

namespace SyncPlan
{
struct Scope
{
    QString relativePath;
    QString localPath;
    QString remotePath;
    QStringList fileTypes;
};

struct File
{
    QString absolutePath;
    QString relativePath;
    qint64 size = 0;
    qint64 modifiedMs = 0;
};

// 文件集合始终使用相对 Release 的路径，允许 ../，按 Windows 大小写规则去重。
using Files = QHash<QString, File>;
bool normalizeTypes(const QStringList &values, QStringList *types, QString *error = nullptr);
bool normalizeRelativePath(const QString &value, QString *path, QString *error = nullptr);
QString resolvePath(const QString &base, const QString &relativePath);
bool validateRemotePath(const QString &base, const QString &relativePath, QString *error);
bool build(const QString &localBase, const QString &remoteBase, const QStringList &baseTypes,
           const QVector<SyncRule> &rules, QVector<Scope> *scopes, QString *error);
Files collect(const QString &base, const QVector<Scope> &scopes, bool local,
              const std::atomic<bool> *cancelled, QString *error);
QString fingerprint(const QString &localBase, const QStringList &baseTypes, const QVector<SyncRule> &rules);
}
