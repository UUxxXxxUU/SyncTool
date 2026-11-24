#pragma once

#include <memory>
#include <QFrame>
#include <QPointer>
#include <QVariantAnimation>
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

    enum Kind { Information, Success, Warning };
    void present(const QString &title, const QString &detail, Kind kind);
    void dismiss();

signals:
    void presented(const QString &title, const QString &detail);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    std::unique_ptr<Ui::SyncCompletionToast> m_ui;
    QPointer<QWidget> m_anchor;
    QVariantAnimation m_glow;
    QTimer m_timer;
};
