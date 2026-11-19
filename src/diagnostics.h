#pragma once

#include <QString>

namespace Diagnostics
{
QString directory();
void start();
void stop();
void record(const char *event, const QString &fields = {}, bool warning = false);
void maintainLogs(const QString &logDirectory, int retentionDays);
void maintain(int retentionDays);
}
