#pragma once

#include "appconfig.h"
#include "syncplan.h"

#include <atomic>
#include <memory>
#include <QByteArray>
#include <QObject>
#include <QElapsedTimer>
#include <QSet>
#include <QJsonObject>
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
        Error = 5,
        Stopping = 6,
        Stopped = 7
    };
    Q_ENUM(VisualState)

    using LocalFile = SyncPlan::File;

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
        QByteArray diagnosticSignature;
    };

    SyncWorker(HostEntry host, QString releaseDir, QStringList fileTypes, bool performCopy,
               QVector<SyncRule> rules = {}, QObject *parent = nullptr, QStringList exclusions = {});
    void emitProgress(int percent, const QString &detail);
    void emitRecentFiles(const QStringList &files, const QString &title);
    void recordDiagnostic(const char *name, const QJsonObject &fields, bool warning = false);
    void flushDiagnostics(bool force = true);
    bool isCancelled() const;
    std::shared_ptr<std::atomic<bool>> cancellationFlag() const;
    static void releaseShareConnections();

public slots:
    void process();
    void cancel();

signals:
    void copySucceeded(const QString &hostId, const QDateTime &completedUtc);
    void symbolsVerified(const QString &hostId, const QStringList &pendingPdbs, bool verified);
    void progressChanged(const QString &hostId, int percent, const QString &detail);
    void recentFilesChanged(const QString &hostId, const QStringList &files, const QString &title);
    void stateChanged(const QString &hostId,
                      int state,
                      int mismatchPercent,
                      const QString &statusText,
                      const QString &detail,
                      const QString &detailTooltip,
                      const QByteArray &displaySignature);
    void finished(const QString &hostId);

private:
    void emitState(int state,
                   int mismatchPercent,
                   const QString &statusText,
                   const QString &detail,
                   const QString &detailTooltip = QString());
    void emitFinished();
    void finishWithError(int state, const QString &statusText, const QString &detail);
    bool finishIfCancelled();

    QString normalizedReleaseDir() const;
    QString normalizedRemoteDir() const;
    QByteArray diagnosticContext() const;

    HostEntry m_host;
    QString m_releaseDir;
    QStringList m_fileTypes;
    QVector<SyncRule> m_rules;
    QStringList m_exclusions;
    QString m_operationId;
    QString m_parentAction;
    QElapsedTimer m_elapsed;
    int m_loggedProgress = -1;
    QSet<QString> m_loggedFiles;
    struct DeferredDiagnostic { QByteArray name; QJsonObject fields; bool warning; };
    QVector<DeferredDiagnostic> m_deferredDiagnostics;
    int m_deferredBytes = 0;
    bool m_backgroundLogging = false;
    bool m_detailedLogging = true;
    bool m_copyAttempted = false;
    QString m_diagnosticKey;
    QByteArray m_diagnosticConfiguration;
    QByteArray m_comparisonSignature;
    bool m_performCopy = false;
    std::shared_ptr<std::atomic<bool>> m_cancelled;
};
