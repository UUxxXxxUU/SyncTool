#pragma once

#include <memory>
#include <QFrame>
#include <QPointer>
#include <QQueue>
#include <QTimer>

namespace Ui { class SyncCompletionToast; }

// Owned by MainWindow, but kept as an independent top-level window so tray mode
// and minimizing the main window do not hide completion notifications.
class SyncCompletionToast : public QFrame
{
    Q_OBJECT

public:
    explicit SyncCompletionToast(QWidget *anchor);
    ~SyncCompletionToast() override;

    void enqueue(const QString &hostName);
    void dismiss();

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void showNext();

    std::unique_ptr<Ui::SyncCompletionToast> m_ui;
    QPointer<QWidget> m_anchor;
    QQueue<QString> m_pending;
    QTimer m_timer;
};
