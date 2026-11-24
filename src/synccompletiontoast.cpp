#pragma execution_character_set("utf-8")
#include "synccompletiontoast.h"
#include "ui_synccompletiontoast.h"

#include <QGuiApplication>
#include <QCursor>
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
    m_timer.setInterval(3000);
    connect(&m_timer, &QTimer::timeout, this, &SyncCompletionToast::dismiss);
    m_glow.setDuration(1500);
    m_glow.setStartValue(0.0);
    m_glow.setKeyValueAt(0.5, 1.0);
    m_glow.setEndValue(0.0);
    m_glow.setEasingCurve(QEasingCurve::InOutSine);
    connect(&m_glow, &QVariantAnimation::valueChanged, this, [this]() { update(); });
}

SyncCompletionToast::~SyncCompletionToast() = default;

void SyncCompletionToast::dismiss()
{
    m_timer.stop();
    m_glow.stop();
    hide();
}

void SyncCompletionToast::present(const QString &title, const QString &detail, Kind kind)
{
    QScreen *screen = nullptr;
    if (m_anchor && m_anchor->isActiveWindow() && m_anchor->windowHandle())
        screen = m_anchor->windowHandle()->screen();
#ifdef Q_OS_WIN
    if (!screen) {
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTONULL);
        if (monitor && GetMonitorInfoW(monitor, &info)) {
            for (auto *candidate : QGuiApplication::screens())
                if (candidate->name() == QString::fromWCharArray(info.szDevice)) { screen = candidate; break; }
        }
    }
#endif
    if (!screen) screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen == nullptr) {
        return;
    }

    const QRect area = screen->availableGeometry();
    ensurePolished();
    m_ui->messageLabel->setVisible(!detail.isEmpty());
    m_ui->titleLabel->setText(title);
    m_ui->successIcon->setText(kind == Success ? QStringLiteral("✓") : kind == Warning ? QStringLiteral("!") : QStringLiteral("•"));
    m_ui->successIcon->setStyleSheet(kind == Success ? "color: #80f2af" : kind == Warning ? "color: #ffd07a" : "color: #91caff");
    setFixedSize(qMin(480, qMax(1, area.width() - 32)), qMin(area.height(), qMax(78, layout()->sizeHint().height())));
    layout()->activate();
    m_ui->messageLabel->setText(m_ui->messageLabel->fontMetrics().elidedText(
        detail.simplified(), Qt::ElideRight, m_ui->messageLabel->contentsRect().width()));
    move(area.left() + (area.width() - width()) / 2,
         area.top() + qMin(qRound(area.height() * 0.12), area.height() - height()));
    bool animate = kind == Success;
#ifdef Q_OS_WIN
    if (QGuiApplication::platformName() == "windows") {
        // Qt 5.12's no-focus flag alone does not set WS_EX_NOACTIVATE.
        const HWND handle = reinterpret_cast<HWND>(winId());
        const LONG_PTR style = GetWindowLongPtrW(handle, GWL_EXSTYLE);
        SetWindowLongPtrW(handle, GWL_EXSTYLE, style | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT);
        BOOL effects = FALSE;
        animate = animate && SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &effects, 0) && effects;
    }
#endif
    show();
    m_glow.stop();
    if (animate) m_glow.start();
    update();
    m_timer.start();
    emit presented(title, detail);
}

void SyncCompletionToast::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(28, 35, 46, 224));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12, 12);
    if (m_glow.state() == QAbstractAnimation::Running) {
        QRadialGradient halo(m_ui->successIcon->geometry().center(), 23);
        halo.setColorAt(0, QColor(91, 230, 155, qRound(95 * m_glow.currentValue().toReal())));
        halo.setColorAt(1, Qt::transparent);
        painter.setBrush(halo);
        painter.drawEllipse(QPointF(m_ui->successIcon->geometry().center()), 23, 23);
    }
}
