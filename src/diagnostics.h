#pragma once

#include <QString>
#include <QByteArray>
#include <QJsonObject>

class QObject;

namespace Diagnostics
{
QString directory();
void start(const QString &diagnosticRoot = {});
void stop();
void record(const char *event, const QString &fields = {}, bool warning = false);
void event(const char *name, const QJsonObject &fields = {}, bool warning = false);
bool backgroundState(const QString &key, const QByteArray &context, const QJsonObject &outcome,
                     const QJsonObject &fields, bool warning = false);
bool backgroundFailure(const QString &key, const QByteArray &context);
void rememberSecret(const QString &value);
QString lastUserAction();
void watch(QObject *root);
void maintainLogs(const QString &logDirectory, int retentionDays);
void maintain(int retentionDays);
}
