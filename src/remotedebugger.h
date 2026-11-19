#pragma once

#include <QDialog>
#include <QPointer>
#include <QUrl>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryFile;
class QTimer;
namespace Ui { class RemoteDebugger; }

class RemoteDebugger : public QDialog
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
    void copyPaths(bool directories);
    void updateArchitectures();
    void download();
    void request(const QUrl &url);
    void readDownload();
    void completeRequest();
    void finishDownload(const QString &error, const QString &temporaryPath = {});

    std::unique_ptr<Ui::RemoteDebugger> m_ui;
    QNetworkAccessManager *m_network = nullptr;
    QPointer<QNetworkReply> m_reply;
    std::unique_ptr<QTemporaryFile> m_download;
    QTimer *m_timeout = nullptr;
    QString m_downloadName;
    int m_redirects = 0;
};
