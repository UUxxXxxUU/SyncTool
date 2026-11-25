#pragma once

#include <QDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QUrl>
#include <functional>

class QCheckBox;
class QComboBox;
class QDragEnterEvent;
class QDropEvent;
class QImage;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTreeWidget;

class FeedbackDialog : public QDialog
{
    Q_OBJECT
public:
    explicit FeedbackDialog(QWidget *parent = nullptr);

signals:
    void unreadChanged(int total);

protected:
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    using Response = std::function<void(const QJsonObject &)>;
    void request(const QString &route, const QJsonObject *payload, Response handler, bool authenticated = true);
    void activate();
    void refresh();
    void showReport(const QString &report);
    void submit();
    void saveDraft() const;
    void restoreDraft();
    void updateAttachments();
    void addAttachment(const QString &name, const QByteArray &bytes);
    void attachFile(const QString &path);
    void openAttachment(const QString &attachment);
    void preview(const QImage &image);
    void markResolution(bool resolved);
    void setBusy(bool busy);
    QString credentialKey() const;

    QNetworkAccessManager m_network;
    QUrl m_endpoint;
    QByteArray m_token;
    QByteArray m_activationToken;
    QString m_report;
    QJsonObject m_pending;
    QJsonArray m_files;
    bool m_busy = false;
    QLineEdit *m_service;
    QLineEdit *m_invite;
    QLineEdit *m_title;
    QComboBox *m_type;
    QCheckBox *m_diagnostics;
    QPlainTextEdit *m_body;
    QPlainTextEdit *m_thread;
    QListWidget *m_attachments;
    QListWidget *m_received;
    QTreeWidget *m_reports;
    QPushButton *m_activate;
    QPushButton *m_refresh;
    QPushButton *m_submit;
    QPushButton *m_resolved;
    QPushButton *m_reopen;
    QLabel *m_status;
};
