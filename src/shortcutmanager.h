#pragma once

#include "appconfig.h"
#include <QAbstractNativeEventFilter>
#include <QHash>
#include <QWidget>
#include <functional>

class QShortcut;
class QLineEdit;
class QComboBox;
class QLabel;
class QStackedWidget;

class ShortcutManager : public QAbstractNativeEventFilter
{
public:
    struct Command { QString id, title; std::function<void()> invoke; QString group; };
    explicit ShortcutManager(QWidget *window);
    ~ShortcutManager() override;
    void add(Command command);
    const QVector<Command> &commands() const { return m_commands; }
    QString apply(const QVector<ShortcutBinding> &bindings);
    void suspend();
    bool isDispatching() const { return m_dispatching; }
    QString warning(const QVector<ShortcutBinding> &bindings, const ShortcutBinding &binding, bool probe) const;
    static QString normalized(const QString &text);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    bool nativeEventFilter(const QByteArray &, void *, qintptr *) override;
#else
    bool nativeEventFilter(const QByteArray &, void *, long *) override;
#endif
private:
    void activate(int command);
    QWidget *m_window;
    QVector<Command> m_commands;
    QVector<QShortcut *> m_local;
    QHash<int, int> m_global;
    QHash<int, quintptr> m_globalChords;
    quintptr m_handle = 0;
    bool m_dispatching = false;
};

class ShortcutPreferences : public QWidget
{
public:
    ShortcutPreferences(ShortcutManager &manager, const QVector<ShortcutBinding> &saved, QWidget *parent);
    const QVector<ShortcutBinding> &bindings() const { return m_draft; }
    bool validate();
private:
    struct Editor {
        QString action, title, group;
        QLineEdit *keys;
        QComboBox *scope;
        QLabel *warning;
    };
    void showCommands();
    void edited(const Editor &editor);
    ShortcutManager &m_manager;
    QVector<Editor> m_editors;
    QVector<ShortcutBinding> m_draft;
    QComboBox *m_group = nullptr;
    QComboBox *m_command = nullptr;
    QStackedWidget *m_pages = nullptr;
};
