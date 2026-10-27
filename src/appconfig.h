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

// 应用持久化配置。
struct AppConfig
{
    QString releaseDir;
    QStringList fileTypes;
    QStringList selectedFileTypes;
    bool autoSync = true;
    bool startWithSystem = false;
    bool checkUpdatesOnStartup = false;
    int scanIntervalSeconds = 5;
    QVector<HostEntry> hosts;
    QStringList releaseDirHistory;
};
