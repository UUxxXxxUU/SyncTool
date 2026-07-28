#pragma execution_character_set("utf-8")

#include "configmanager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

namespace
{
QString ensureHostId(const QString &value)
{
    return value.trimmed().isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : value.trimmed();
}

QStringList defaultTypes()
{
    return QStringList() << "dll" << "exe" << "pdb";
}
}

QString ConfigManager::defaultConfigPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath("sync_config.json");
}

AppConfig ConfigManager::load(const QString &filePath)
{
    AppConfig config;
    config.fileTypes = defaultTypes();
    config.selectedFileTypes = config.fileTypes;

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly)) {
        return config;
    }

    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) {
        return config;
    }

    const QJsonObject root = document.object();
    config.releaseDir = root.value("releaseDir").toString().trimmed();
    config.autoSync = root.value("autoSync").toBool(true);
    config.startWithSystem = root.value("startWithSystem").toBool(false);
    config.scanIntervalSeconds = qMax(1, root.value("scanIntervalSeconds").toInt(5));

    const QJsonArray historyArray = root.value("releaseDirHistory").toArray();
    for (const QJsonValue &value : historyArray) {
        const QString path = value.toString().trimmed();
        if (!path.isEmpty() && !config.releaseDirHistory.contains(path)) {
            config.releaseDirHistory.append(path);
        }
    }

    const QJsonArray typesArray = root.value("fileTypes").toArray();
    if (!typesArray.isEmpty()) {
        config.fileTypes.clear();
        for (const QJsonValue &value : typesArray) {
            const QString type = value.toString().trimmed().toLower();
            if (!type.isEmpty() && !config.fileTypes.contains(type)) {
                config.fileTypes.append(type);
            }
        }
    }
    if (config.fileTypes.isEmpty()) {
        config.fileTypes = defaultTypes();
    }

    const QJsonArray selectedTypesArray = root.value("selectedFileTypes").toArray();
    if (!selectedTypesArray.isEmpty()) {
        config.selectedFileTypes.clear();
        for (const QJsonValue &value : selectedTypesArray) {
            const QString type = value.toString().trimmed().toLower();
            if (!type.isEmpty() && !config.selectedFileTypes.contains(type)) {
                config.selectedFileTypes.append(type);
            }
        }
    } else {
        config.selectedFileTypes = config.fileTypes;
    }

    for (const QString &type : config.selectedFileTypes) {
        if (!config.fileTypes.contains(type)) {
            config.fileTypes.append(type);
        }
    }

    const QJsonArray hostsArray = root.value("hosts").toArray();
    for (const QJsonValue &value : hostsArray) {
        if (!value.isObject()) {
            continue;
        }

        const QJsonObject item = value.toObject();
        HostEntry host;
        host.id = ensureHostId(item.value("id").toString());
        host.name = item.value("name").toString().trimmed();
        host.sharePath = item.value("sharePath").toString().trimmed();
        host.username = item.value("username").toString().trimmed();
        host.password = item.value("password").toString();
        host.enabled = item.value("enabled").toBool(true);

        if (!host.sharePath.isEmpty()) {
            config.hosts.push_back(host);
        }
    }

    return config;
}

bool ConfigManager::save(const QString &filePath, const AppConfig &config, QString *errorMessage)
{
    QJsonObject root;
    root.insert("releaseDir", config.releaseDir);
    root.insert("autoSync", config.autoSync);
    root.insert("startWithSystem", config.startWithSystem);
    root.insert("scanIntervalSeconds", qMax(1, config.scanIntervalSeconds));

    QJsonArray historyArray;
    for (const QString &path : config.releaseDirHistory) {
        const QString normalized = path.trimmed();
        if (!normalized.isEmpty()) {
            historyArray.append(normalized);
        }
    }
    root.insert("releaseDirHistory", historyArray);

    QJsonArray typesArray;
    for (const QString &type : config.fileTypes) {
        const QString normalized = type.trimmed().toLower();
        if (!normalized.isEmpty()) {
            typesArray.append(normalized);
        }
    }
    root.insert("fileTypes", typesArray);

    QJsonArray selectedTypesArray;
    for (const QString &type : config.selectedFileTypes) {
        const QString normalized = type.trimmed().toLower();
        if (!normalized.isEmpty()) {
            selectedTypesArray.append(normalized);
        }
    }
    root.insert("selectedFileTypes", selectedTypesArray);

    QJsonArray hostsArray;
    for (const HostEntry &host : config.hosts) {
        if (host.sharePath.trimmed().isEmpty()) {
            continue;
        }

        QJsonObject item;
        item.insert("id", ensureHostId(host.id));
        item.insert("name", host.name.trimmed());
        item.insert("sharePath", host.sharePath.trimmed());
        item.insert("username", host.username.trimmed());
        item.insert("password", host.password);
        item.insert("enabled", host.enabled);
        hostsArray.append(item);
    }
    root.insert("hosts", hostsArray);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}
