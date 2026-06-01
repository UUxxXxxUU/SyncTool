#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

struct HostEntry
{
    QString id;
    QString name;
    QString sharePath;
    QString username;
    QString password;
    bool enabled = true;
};

struct AppConfig
{
    QString releaseDir;
    QStringList fileTypes;
    QStringList selectedFileTypes;
    bool autoSync = true;
    int scanIntervalSeconds = 5;
    QVector<HostEntry> hosts;
};
