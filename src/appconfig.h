#pragma once

#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QVector>

// 远程主机配置项。
struct HostEntry
{
    QString id;
    QString name;
    QString sharePath;
    QString username;
    QString password;
    bool enabled = true;
    QDateTime lastSyncUtc;
};

// 路径同时相对于本地和远端 Release 目录；空类型列表表示全部文件。
struct SyncRule
{
    QString relativePath;
    QStringList fileTypes;
    bool enabled = true;

    bool operator==(const SyncRule &other) const
    {
        return relativePath == other.relativePath && fileTypes == other.fileTypes && enabled == other.enabled;
    }
    bool operator!=(const SyncRule &other) const { return !(*this == other); }
};

// 应用持久化配置。
struct AppConfig
{
    QString releaseDir;
    QStringList fileTypes;
    QStringList selectedFileTypes;
    QVector<SyncRule> syncRules;
    bool autoSync = true;
    bool startWithSystem = false;
    bool checkUpdatesOnStartup = false;
    bool silentUpdate = false;
    int scanIntervalSeconds = 5;
    QVector<HostEntry> hosts;
    QStringList releaseDirHistory;
};
