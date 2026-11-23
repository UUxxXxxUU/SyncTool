#pragma once

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMessageBox>
#include <QPainter>
#include <QResizeEvent>
#include <QShowEvent>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVector>
#include <QtMath>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include "../third_party/qt-frameless-window/FramelessWindow.hpp"

template <typename T>
class RoundedWindow : public FramelessWindow<T>
{
    using Base = FramelessWindow<T>;

public:
    explicit RoundedWindow(QWidget *parent = nullptr)
        : Base(true, parent)
    {
        this->setProperty("roundedWindow", true);
        this->setAutoFillBackground(false);
        this->setMouseTracking(true);
        this->setUseSystemMove(false);
        this->setResizeRegionPadding(5);

        m_titleBar = new QWidget(this);
        m_titleBar->setObjectName("roundedTitleBar");
        m_titleBar->setMouseTracking(true);
        m_titleBar->installEventFilter(this);
        auto *layout = new QHBoxLayout(m_titleBar);
        layout->setContentsMargins(6, 0, 0, 0);
        layout->setSpacing(6);
        m_icon = new QLabel(m_titleBar);
        m_icon->setFixedSize(18, 18);
        m_icon->setAttribute(Qt::WA_TransparentForMouseEvents);
        m_title = new QLabel(m_titleBar);
        m_title->setTextFormat(Qt::PlainText);
        m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        m_title->setAttribute(Qt::WA_TransparentForMouseEvents);
        layout->addWidget(m_icon);
        layout->addWidget(m_title, 1);
        m_minimize = createTitleBarButton(layout, QStyle::SP_TitleBarMinButton, QString::fromUtf8("最小化"));
        m_maximize = createTitleBarButton(layout, QStyle::SP_TitleBarMaxButton, QString::fromUtf8("最大化"));
        m_close = createTitleBarButton(layout, QStyle::SP_TitleBarCloseButton, QString::fromUtf8("关闭"));
        m_close->setObjectName("roundedCloseButton");
        this->setTitlebar({m_titleBar});
        QObject::connect(m_minimize, &QToolButton::clicked, this, [this]() { this->showMinimized(); });
        QObject::connect(m_maximize, &QToolButton::clicked, this, [this]() {
            if (this->isMaximized()) this->showNormal();
            else this->showMaximized();
        });
        QObject::connect(m_close, &QToolButton::clicked, this, [this]() { this->close(); });
    }

protected:
#ifdef Q_OS_WIN
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override
#else
    bool nativeEvent(const QByteArray &eventType, void *message, long *result) override
#endif
    {
        if (!message || !result || !this->isWindow() || this->isFullScreen()
            || !this->windowFlags().testFlag(Qt::WindowMaximizeButtonHint)
            || this->minimumSize() == this->maximumSize()) {
            return Base::nativeEvent(eventType, message, result);
        }
        const auto *msg = static_cast<const MSG *>(message);
        if (msg->message != WM_GETMINMAXINFO || !msg->hwnd || !msg->lParam) {
            return Base::nativeEvent(eventType, message, result);
        }
        const HMONITOR monitor = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        const qreal scale = this->devicePixelRatioF();
        if (!monitor || !GetMonitorInfo(monitor, &info) || !qIsFinite(scale) || scale <= 0
            || info.rcWork.right <= info.rcWork.left || info.rcWork.bottom <= info.rcWork.top) {
            return Base::nativeEvent(eventType, message, result);
        }

        auto *limits = reinterpret_cast<MINMAXINFO *>(msg->lParam);
        const auto nativeLength = [scale](int logical) {
            return static_cast<LONG>(qCeil(qMin(qreal(logical) * scale, qreal(MAXLONG))));
        };
        // Handling this message bypasses Qt's size hints, so retain widget constraints here.
        limits->ptMinTrackSize.x = qMax(limits->ptMinTrackSize.x, nativeLength(this->minimumWidth()));
        limits->ptMinTrackSize.y = qMax(limits->ptMinTrackSize.y, nativeLength(this->minimumHeight()));
        limits->ptMaxTrackSize.x = this->maximumWidth() < QWIDGETSIZE_MAX
            ? qMax(limits->ptMinTrackSize.x, nativeLength(this->maximumWidth()))
            : qMax(limits->ptMinTrackSize.x,
                   qMax(limits->ptMaxTrackSize.x, info.rcWork.right - info.rcWork.left));
        limits->ptMaxTrackSize.y = this->maximumHeight() < QWIDGETSIZE_MAX
            ? qMax(limits->ptMinTrackSize.y, nativeLength(this->maximumHeight()))
            : qMax(limits->ptMinTrackSize.y,
                   qMax(limits->ptMaxTrackSize.y, info.rcWork.bottom - info.rcWork.top));
        // Win32 monitor rectangles already use native coordinates; do not scale them again.
        limits->ptMaxPosition.x = info.rcWork.left - info.rcMonitor.left;
        limits->ptMaxPosition.y = info.rcWork.top - info.rcMonitor.top;
        limits->ptMaxSize.x = qBound(limits->ptMinTrackSize.x,
                                    info.rcWork.right - info.rcWork.left, limits->ptMaxTrackSize.x);
        limits->ptMaxSize.y = qBound(limits->ptMinTrackSize.y,
                                    info.rcWork.bottom - info.rcWork.top, limits->ptMaxTrackSize.y);
        *result = 0;
        return true;
    }
#endif

    void showEvent(QShowEvent *event) override
    {
        updateFrame();
        T::showEvent(event);
#ifdef Q_OS_WIN
        scheduleWindowBounds();
#endif
    }

    void resizeEvent(QResizeEvent *event) override
    {
        updateFrame();
        T::resizeEvent(event);
#ifdef Q_OS_WIN
        scheduleWindowBounds();
#endif
    }

    void changeEvent(QEvent *event) override
    {
        T::changeEvent(event);
        if (event->type() == QEvent::WindowStateChange) this->m_Direction = Base::Direction::NONE;
        if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowTitleChange
            || event->type() == QEvent::WindowIconChange || event->type() == QEvent::FontChange) {
            updateFrame();
        }
#ifdef Q_OS_WIN
        if (event->type() == QEvent::WindowStateChange) scheduleWindowBounds();
#endif
    }

    void paintEvent(QPaintEvent *event) override
    {
        T::paintEvent(event);
        QPainter painter(this);
        // Clear the corners as well as the body when restoring from maximized state.
        painter.setCompositionMode(QPainter::CompositionMode_Source);
        painter.fillRect(this->rect(), Qt::transparent);
        painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QColor("#c4d3e5"));
        painter.setBrush(QColor("#f1f5fa"));
        const qreal radius = this->isMaximized() || this->isFullScreen() ? 0.0 : 8.0;
        painter.drawRoundedRect(QRectF(this->rect()).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    }

    bool eventFilter(QObject *target, QEvent *event) override
    {
        const bool mouseEvent = event->type() == QEvent::MouseMove || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::MouseButtonDblClick;
        if (target == m_titleBar && mouseEvent) {
            if (event->type() == QEvent::MouseMove && this->isMaximized() && this->m_bLeftPressed) {
                const auto *mouse = static_cast<QMouseEvent *>(event);
                const qreal fraction = qBound(0.0, qreal(mouse->pos().x()) / qMax(1, m_titleBar->width()), 1.0);
                this->showNormal();
                this->m_DragPos = QPoint(qRound(this->width() * fraction), m_titleBar->height() / 2);
            }
            Base::eventFilter(target, event);
            // Consume title-bar events here so they are not processed again by the parent.
            return true;
        }
        if (target == this && event->type() == QEvent::MouseMove && this->isMaximized()
            && !this->m_bLeftPressed) {
            this->m_Direction = Base::Direction::NONE;
            this->unsetCursor();
            return T::eventFilter(target, event);
        }
        return Base::eventFilter(target, event);
    }

private:
#ifdef Q_OS_WIN
    void scheduleWindowBounds()
    {
        if (m_boundsCorrectionPending || this->isMinimized() || this->isFullScreen()
            || (!this->isMaximized() && !m_normalGeometryBeforeCorrection.isValid())) return;
        m_boundsCorrectionPending = true;
        // Layered frameless windows can change state without adopting the maximized size.
        QTimer::singleShot(0, this, [this]() {
            if (this->isVisible() && !this->isMinimized() && !this->isFullScreen()) {
                if (this->isMaximized()) {
                    applyMaximizedBounds();
                } else if (m_normalGeometryBeforeCorrection.isValid()) {
                    const QRect normal = m_normalGeometryBeforeCorrection;
                    m_normalGeometryBeforeCorrection = QRect();
                    this->setGeometry(normal);
                }
            }
            m_boundsCorrectionPending = false;
        });
    }

    void applyMaximizedBounds()
    {
        const HWND hwnd = reinterpret_cast<HWND>(this->effectiveWinId());
        if (!hwnd || !IsWindow(hwnd) || !IsZoomed(hwnd)) return;
        RECT bounds = {};
        MONITORINFO info = {};
        info.cbSize = sizeof(info);
        const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!monitor || !GetMonitorInfo(monitor, &info) || !GetWindowRect(hwnd, &bounds)) return;
        MINMAXINFO limits = {};
        MSG message = {};
        message.hwnd = hwnd;
        message.message = WM_GETMINMAXINFO;
        message.lParam = reinterpret_cast<LPARAM>(&limits);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        qintptr result = 0;
#else
        long result = 0;
#endif
        if (!nativeEvent("windows_generic_MSG", &message, &result)
            || limits.ptMaxSize.x <= 0 || limits.ptMaxSize.y <= 0) return;
        const LONG left = info.rcMonitor.left + limits.ptMaxPosition.x;
        const LONG top = info.rcMonitor.top + limits.ptMaxPosition.y;
        const RECT wanted = {left, top, left + limits.ptMaxSize.x, top + limits.ptMaxSize.y};
        if (EqualRect(&bounds, &wanted)) return;
        QWindow *handle = this->windowHandle();
        if (!handle || !handle->screen()) return;
        const qreal scale = this->devicePixelRatioF();
        const QPoint offset(qRound(limits.ptMaxPosition.x / scale), qRound(limits.ptMaxPosition.y / scale));
        const QSize size(qRound(limits.ptMaxSize.x / scale), qRound(limits.ptMaxSize.y / scale));
        // QWindow geometry changes can overwrite Qt's restore geometry while maximized.
        if (!m_normalGeometryBeforeCorrection.isValid()) {
            m_normalGeometryBeforeCorrection = this->normalGeometry();
            if (!m_normalGeometryBeforeCorrection.isValid()) m_normalGeometryBeforeCorrection = this->geometry();
        }
        // QWindow updates the layered backing store without clearing the widget's maximized state.
        handle->setGeometry(QRect(handle->screen()->geometry().topLeft() + offset, size));
    }
#endif

    QToolButton *createTitleBarButton(QHBoxLayout *layout, QStyle::StandardPixmap icon, const QString &text)
    {
        auto *button = new QToolButton(m_titleBar);
        button->setAutoRaise(true);
        button->setFixedSize(32, 28);
        button->setIcon(this->style()->standardIcon(icon));
        button->setToolTip(text);
        button->setAccessibleName(text);
        button->setFocusPolicy(Qt::NoFocus);
        layout->addWidget(button);
        return button;
    }

    void updateFrame()
    {
        if (!m_close || m_updatingFrame) return;
        m_updatingFrame = true;
        const bool fullScreen = this->isFullScreen();
        const bool maximized = this->isMaximized();
        const int border = maximized || fullScreen ? 0 : 6;
        const int titleHeight = fullScreen ? 0 : qMax(32, this->fontMetrics().height() + 12);
        const QMargins margins(border, border + titleHeight, border, border);
        if (this->contentsMargins() != margins) this->setContentsMargins(margins);
        const bool resizable = this->minimumSize() != this->maximumSize();
        // The upstream hit test starts at contentsRect; expand it to include our frame.
        this->setResizeable(resizable && !fullScreen,
                           QMargins(-margins.left(), -margins.top(), -margins.right(), -margins.bottom()));
        m_titleBar->setVisible(!fullScreen);
        m_titleBar->setGeometry(border, border, qMax(0, this->width() - border * 2), titleHeight);
        m_titleBar->raise();
        const Qt::WindowFlags flags = this->windowFlags();
        m_minimize->setVisible(flags.testFlag(Qt::WindowMinimizeButtonHint));
        m_maximize->setVisible(resizable && flags.testFlag(Qt::WindowMaximizeButtonHint));
        m_close->setVisible(flags.testFlag(Qt::WindowCloseButtonHint));
        m_maximize->setIcon(this->style()->standardIcon(maximized ? QStyle::SP_TitleBarNormalButton : QStyle::SP_TitleBarMaxButton));
        const QString maximizeText = maximized ? QString::fromUtf8("还原") : QString::fromUtf8("最大化");
        m_maximize->setToolTip(maximizeText);
        m_maximize->setAccessibleName(maximizeText);
        m_title->setText(this->windowTitle());
        m_title->setToolTip(this->windowTitle());
        m_icon->setPixmap(this->windowIcon().pixmap(m_icon->size()));
        m_updatingFrame = false;
        this->update();
    }

    QWidget *m_titleBar = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QToolButton *m_minimize = nullptr;
    QToolButton *m_maximize = nullptr;
    QToolButton *m_close = nullptr;
    bool m_updatingFrame = false;
#ifdef Q_OS_WIN
    bool m_boundsCorrectionPending = false;
    QRect m_normalGeometryBeforeCorrection;
#endif
};

using RoundedMainWindow = RoundedWindow<QMainWindow>;
using RoundedDialog = RoundedWindow<QDialog>;

class RoundedMessageBox : public RoundedWindow<QMessageBox>
{
public:
    explicit RoundedMessageBox(QWidget *parent = nullptr) : RoundedWindow<QMessageBox>(parent) {}

    RoundedMessageBox(Icon icon, const QString &title, const QString &text, StandardButtons buttons, QWidget *parent)
        : RoundedMessageBox(parent)
    {
        setIcon(icon);
        setWindowTitle(title);
        setText(text);
        setStandardButtons(buttons);
    }

    static StandardButton information(QWidget *parent, const QString &title, const QString &text,
                                      StandardButtons buttons = Ok, StandardButton defaultButton = NoButton)
    {
        return run(Information, parent, title, text, buttons, defaultButton);
    }

    static StandardButton warning(QWidget *parent, const QString &title, const QString &text,
                                  StandardButtons buttons = Ok, StandardButton defaultButton = NoButton)
    {
        return run(Warning, parent, title, text, buttons, defaultButton);
    }

    static StandardButton question(QWidget *parent, const QString &title, const QString &text,
                                   StandardButtons buttons = StandardButtons(Yes | No), StandardButton defaultButton = NoButton)
    {
        return run(Question, parent, title, text, buttons, defaultButton);
    }

private:
    static StandardButton run(Icon icon, QWidget *parent, const QString &title, const QString &text,
                              StandardButtons buttons, StandardButton defaultButton)
    {
        RoundedMessageBox box(icon, title, text, buttons, parent);
        if (defaultButton != NoButton && buttons.testFlag(defaultButton)) box.setDefaultButton(defaultButton);
        return static_cast<StandardButton>(box.exec());
    }
};
