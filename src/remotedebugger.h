#pragma once

#include "roundedwindow.h"
#include "appconfig.h"
#include <QPointer>
#include <QElapsedTimer>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QCloseEvent;
class QProcess;
class QTemporaryDir;
class QTemporaryFile;
class QTimer;
namespace Ui { class RemoteDebugger; }

class RemoteDebugger : public RoundedDialog
{
    Q_OBJECT
public:
    explicit RemoteDebugger(QWidget *parent = nullptr);
    ~RemoteDebugger() override;
    void setHosts(const QVector<HostEntry> &hosts, int selectedRow);
    bool isDeploying() const;

protected:
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

signals:
    void downloadFinished(const QString &path, const QString &error);
    void deploymentFinished();

private:
    friend class SyncTests;
    void scan();
    void showPaths(const QByteArray &vswhereOutput);
    void openDebuggerFolder(int row);
    void copyPaths(bool directories);
    void updateArchitectures();
    void download();
    void request(const QUrl &url);
    void readDownload();
    void updateDownloadProgress(qint64 received, qint64 total);
    void completeRequest();
    void finishDownload(const QString &error, const QString &temporaryPath = {});
    void deploy(const QString &operation);
    void startDeploymentProcess(const QString &scriptPath, const QByteArray &payload);
    void readDeploymentOutput();
    void finishDeployment();
    void updateRemoteActions();
    void setRemoteStatus(const QString &message);

    std::unique_ptr<Ui::RemoteDebugger> m_ui;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QTemporaryFile> m_download;
    QTimer *m_timeout = nullptr;
    QString m_downloadName;
    qint64 m_downloadReceived = 0;
    qint64 m_downloadTotal = -1;
    int m_redirects = 0;
    QVector<HostEntry> m_hosts;
    QProcess *m_deployment = nullptr;
    std::unique_ptr<QTemporaryDir> m_deploymentDirectory;
    QByteArray m_deploymentOutput;
    QByteArray m_deploymentErrors;
    bool m_deploymentHasResult = false;
    bool m_remoteContactStarted = false;
    QString m_deploymentId;
    QString m_lastDeploymentProgress;
    QElapsedTimer m_deploymentElapsed;
    QString m_downloadId;
    QElapsedTimer m_downloadElapsed;
};
