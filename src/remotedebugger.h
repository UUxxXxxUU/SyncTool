#pragma once

#include "roundedwindow.h"
#include <QPointer>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryFile;
class QTimer;
namespace Ui { class RemoteDebugger; }

class RemoteDebugger : public RoundedDialog
{
    Q_OBJECT
public:
    explicit RemoteDebugger(QWidget *parent = nullptr);
    ~RemoteDebugger() override;

signals:
    void downloadFinished(const QString &path, const QString &error);

private:
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

    std::unique_ptr<Ui::RemoteDebugger> m_ui;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QTemporaryFile> m_download;
    QTimer *m_timeout = nullptr;
    QString m_downloadName;
    qint64 m_downloadReceived = 0;
    qint64 m_downloadTotal = -1;
    int m_redirects = 0;
};
