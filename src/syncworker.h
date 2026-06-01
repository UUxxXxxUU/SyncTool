#pragma once

#include "appconfig.h"

#include <QObject>
#include <QStringList>

class SyncWorker : public QObject
{
    Q_OBJECT

public:
    enum VisualState
    {
        Checking = 0,
        Syncing = 1,
        InSync = 2,
        Mismatch = 3,
        Disconnected = 4,
        Error = 5
    };
    Q_ENUM(VisualState)

    struct LocalFile
    {
        QString absolutePath;
        QString relativePath;
        qint64 size = 0;
        qint64 modifiedMs = 0;
    };

    struct ComparisonSummary
    {
        int localCount = 0;
        int matchingCount = 0;
        int missingCount = 0;
        int outdatedCount = 0;
        int extraCount = 0;
        int failedCount = 0;
        qint64 totalCopyBytes = 0;
        QString detail;
        QStringList extraFiles;
    };

    SyncWorker(HostEntry host, QString releaseDir, QStringList fileTypes, bool performCopy, QObject *parent = nullptr);
    void emitProgress(int percent, const QString &detail);

public slots:
    void process();

signals:
    void progressChanged(const QString &hostId, int percent, const QString &detail);
    void stateChanged(const QString &hostId,
                      int state,
                      int mismatchPercent,
                      const QString &statusText,
                      const QString &detail,
                      const QString &detailTooltip);
    void finished(const QString &hostId);

private:
    void emitState(int state,
                   int mismatchPercent,
                   const QString &statusText,
                   const QString &detail,
                   const QString &detailTooltip = QString());
    void emitFinished();
    void finishWithError(int state, const QString &statusText, const QString &detail);

    QString normalizedReleaseDir() const;
    QString normalizedRemoteDir() const;

    HostEntry m_host;
    QString m_releaseDir;
    QStringList m_fileTypes;
    bool m_performCopy = false;
};
