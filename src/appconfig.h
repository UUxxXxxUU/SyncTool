#pragma once

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
};

// 应用持久化配置。
struct AppConfig
{
    QString releaseDir;
    QStringList fileTypes;
    QStringList selectedFileTypes;
    bool autoSync = true;
    bool startWithSystem = false;
    int scanIntervalSeconds = 5;
    QVector<HostEntry> hosts;
    QStringList releaseDirHistory;
};
