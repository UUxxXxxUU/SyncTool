#pragma execution_character_set("utf-8")

#include "configmanager.h"
#include "diagnostics.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
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
        Diagnostics::event("config.read_unavailable", {{"path", filePath}, {"exists", file.exists()}, {"error", file.errorString()}}, file.exists());
        return config;
    }

    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (!document.isObject()) {
        Diagnostics::event("config.read_invalid", {{"path", filePath}, {"error", parseError.errorString()}, {"offset", double(parseError.offset)}}, true);
        return config;
    }

    const QJsonObject root = document.object();
    config.releaseDir = root.value("releaseDir").toString().trimmed();
    config.autoSync = root.value("autoSync").toBool(true);
    config.startWithSystem = root.value("startWithSystem").toBool(false);
    config.checkUpdatesOnStartup = root.value("checkUpdatesOnStartup").toBool(false);
    config.silentUpdate = root.value("silentUpdate").toBool(false);
    config.scanIntervalSeconds = qMax(1, root.value("scanIntervalSeconds").toInt(5));
    config.logRetentionDays = qBound(0, root.value("logRetentionDays").toInt(30), 365);
    for (const QJsonValue &entry : root.value("excludedFilePatterns").toArray()) {
        if (entry.isString()) config.excludedFilePatterns.append(entry.toString());
    }

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
    if (root.contains("selectedFileTypes")) {
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

    for (const QJsonValue &value : root.value("syncRules").toArray()) {
        if (!value.isObject()) continue;
        const QJsonObject item = value.toObject();
        SyncRule rule;
        rule.relativePath = item.value("relativePath").toString().trimmed();
        rule.enabled = item.value("enabled").toBool(true);
        for (const QJsonValue &type : item.value("fileTypes").toArray()) {
            rule.fileTypes.append(type.toString());
        }
        config.syncRules.append(rule);
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
        Diagnostics::rememberSecret(host.password);
        host.enabled = item.value("enabled").toBool(true);
        host.lastSyncUtc = QDateTime::fromString(item.value("lastSyncUtc").toString(), Qt::ISODateWithMs).toUTC();

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
    root.insert("checkUpdatesOnStartup", config.checkUpdatesOnStartup);
    root.insert("silentUpdate", config.silentUpdate);
    root.insert("scanIntervalSeconds", qMax(1, config.scanIntervalSeconds));
    root.insert("logRetentionDays", qBound(0, config.logRetentionDays, 365));
    root.insert("excludedFilePatterns", QJsonArray::fromStringList(config.excludedFilePatterns));

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

    QJsonArray rulesArray;
    for (const SyncRule &rule : config.syncRules) {
        QJsonObject item;
        item.insert("relativePath", rule.relativePath);
        item.insert("enabled", rule.enabled);
        QJsonArray types;
        for (const QString &type : rule.fileTypes) types.append(type);
        item.insert("fileTypes", types);
        rulesArray.append(item);
    }
    root.insert("syncRules", rulesArray);

    QJsonArray hostsArray;
    for (const HostEntry &host : config.hosts) {
        Diagnostics::rememberSecret(host.password);
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
        if (host.lastSyncUtc.isValid()) {
            item.insert("lastSyncUtc", host.lastSyncUtc.toUTC().toString(Qt::ISODateWithMs));
        }
        hostsArray.append(item);
    }
    root.insert("hosts", hostsArray);

    Diagnostics::event("config.write_requested", {{"path", filePath}, {"configuration", root}});
    QSaveFile file(filePath);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        Diagnostics::event("config.write_failed", {{"path", filePath}, {"error", file.errorString()}}, true);
        if (errorMessage != nullptr) {
            *errorMessage = file.errorString();
        }
        return false;
    }

    Diagnostics::event("config.write_done", {{"path", filePath}, {"bytes", data.size()}});
    return true;
}
