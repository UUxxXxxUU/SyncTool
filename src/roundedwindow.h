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
#include <QToolButton>
#include <QVector>

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
    void showEvent(QShowEvent *event) override
    {
        updateFrame();
        T::showEvent(event);
    }

    void resizeEvent(QResizeEvent *event) override
    {
        updateFrame();
        T::resizeEvent(event);
    }

    void changeEvent(QEvent *event) override
    {
        T::changeEvent(event);
        if (event->type() == QEvent::WindowStateChange) this->m_Direction = Base::Direction::NONE;
        if (event->type() == QEvent::WindowStateChange || event->type() == QEvent::WindowTitleChange
            || event->type() == QEvent::WindowIconChange || event->type() == QEvent::FontChange) {
            updateFrame();
        }
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
