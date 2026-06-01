#pragma once

#include "appconfig.h"

#include <QString>

class ConfigManager
{
public:
    static QString defaultConfigPath();
    static AppConfig load(const QString &filePath);
    static bool save(const QString &filePath, const AppConfig &config, QString *errorMessage = nullptr);
};
