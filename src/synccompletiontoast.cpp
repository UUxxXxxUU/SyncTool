#pragma execution_character_set("utf-8")
#include "synccompletiontoast.h"
#include "ui_synccompletiontoast.h"

#include <QGuiApplication>
#include <QLayout>
#include <QPainter>
#include <QScreen>
#include <QWindow>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

SyncCompletionToast::SyncCompletionToast(QWidget *anchor)
    : QFrame(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint
                         | Qt::WindowDoesNotAcceptFocus | Qt::WindowTransparentForInput)
    , m_ui(new Ui::SyncCompletionToast)
    , m_anchor(anchor)
{
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_QuitOnClose, false);
    m_ui->setupUi(this);
    m_timer.setSingleShot(true);
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &SyncCompletionToast::showNext);
}

SyncCompletionToast::~SyncCompletionToast() = default;

void SyncCompletionToast::enqueue(const QString &hostName)
{
    const QString name = hostName.simplified();
    m_pending.enqueue(name.isEmpty() ? QString("主机") : name);
    if (!m_timer.isActive()) {
        showNext();
    }
}

void SyncCompletionToast::dismiss()
{
    m_timer.stop();
    m_pending.clear();
    hide();
}

void SyncCompletionToast::showNext()
{
    hide();
    if (m_pending.isEmpty()) {
        return;
    }

    QScreen *screen = nullptr;
    if (m_anchor != nullptr) {
        if (m_anchor->windowHandle() != nullptr) {
            screen = m_anchor->windowHandle()->screen();
        }
        if (!m_anchor->isMinimized()) {
            if (auto *atAnchor = QGuiApplication::screenAt(m_anchor->frameGeometry().center())) {
                screen = atAnchor;
            }
        }
    }
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        m_pending.clear();
        return;
    }

    const QRect area = screen->availableGeometry();
    setFixedSize(qMin(480, qMax(1, area.width() - 48)), qMin(60, area.height()));
    ensurePolished();
    layout()->activate();
    const QString suffix = " 同步完成";
    const QFontMetrics metrics = m_ui->messageLabel->fontMetrics();
    const int nameWidth = qMax(0, m_ui->messageLabel->contentsRect().width() - metrics.horizontalAdvance(suffix));
    m_ui->messageLabel->setText(metrics.elidedText(m_pending.dequeue(), Qt::ElideRight, nameWidth) + suffix);
    move(area.left() + (area.width() - width()) / 2,
         qMax(area.top(), area.bottom() - height() - 23));
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "windows") {
        // Qt 5.12's no-focus flag alone does not set WS_EX_NOACTIVATE.
        const HWND handle = reinterpret_cast<HWND>(winId());
        const LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
        SetWindowLongPtrW(handle, GWL_EXSTYLE, style | WS_EX_NOACTIVATE);
    }
#endif
    show();
    m_timer.start();
}

void SyncCompletionToast::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(28, 35, 46, 210));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
}
