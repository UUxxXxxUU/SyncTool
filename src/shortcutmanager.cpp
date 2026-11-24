#include "shortcutmanager.h"
#include <QScopedValueRollback>
#include "diagnostics.h"
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QFocusEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <algorithm>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {
class KeyField : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    void record()
    {
        recording = true;
        setFocus();
        selectAll();
        setStyleSheet("border: 2px solid palette(highlight);");
        setPlaceholderText(QStringLiteral("请按组合键"));
    }
protected:
    bool event(QEvent *event) override
    {
        if (recording && event->type() == QEvent::ShortcutOverride) {
            event->accept();
            return true;
        }
        if (recording && event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            const int code = key->key();
            if (key->isAutoRepeat() || recordedKey || code == Qt::Key_unknown ||
                (code >= Qt::Key_Shift && code <= Qt::Key_Alt) || code == Qt::Key_AltGr) return true;
            recordedKey = code;
            if (code != Qt::Key_Escape || key->modifiers())
                setText(QKeySequence(code | int(key->modifiers())).toString(QKeySequence::PortableText));
            return true;
        }
        if (recording && event->type() == QEvent::KeyRelease) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (!key->isAutoRepeat() && key->key() == recordedKey) finish();
            return true;
        }
        return QLineEdit::event(event);
    }
    void focusOutEvent(QFocusEvent *event) override
    {
        finish();
        QLineEdit::focusOutEvent(event);
    }
private:
    void finish()
    {
        recording = false;
        recordedKey = 0;
        setStyleSheet({});
        deselect();
        setPlaceholderText(QStringLiteral("未绑定"));
    }
    bool recording = false;
    int recordedKey = 0;
};

#ifdef Q_OS_WIN
struct NativeKey { UINT modifiers = 0, vk = 0; };
NativeKey nativeKey(const QKeySequence &sequence)
{
    const int chord = sequence[0];
    const int key = chord & ~int(Qt::KeyboardModifierMask);
    NativeKey result;
    if (chord & Qt::CTRL) result.modifiers |= MOD_CONTROL;
    if (chord & Qt::ALT) result.modifiers |= MOD_ALT;
    if (chord & Qt::SHIFT) result.modifiers |= MOD_SHIFT;
    if (chord & Qt::META) result.modifiers |= MOD_WIN;
    if (chord & Qt::KeypadModifier) {
        if (key >= '0' && key <= '9') result.vk = VK_NUMPAD0 + key - '0';
        else if (key == '+') result.vk = VK_ADD;
        else if (key == '-') result.vk = VK_SUBTRACT;
        else if (key == '*') result.vk = VK_MULTIPLY;
        else if (key == '/') result.vk = VK_DIVIDE;
        else if (key == '.') result.vk = VK_DECIMAL;
        return result;
    }
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) result.vk = key;
    else if (key >= Qt::Key_F1 && key <= Qt::Key_F24) result.vk = VK_F1 + key - Qt::Key_F1;
    else if (key >= 32 && key < 0xffff) {
        // 标点键的虚拟键码和隐含修饰键取决于当前键盘布局。
        const SHORT mapped = VkKeyScanW(wchar_t(key));
        if (mapped != -1) {
            result.vk = LOBYTE(mapped);
            if (HIBYTE(mapped) & 1) result.modifiers |= MOD_SHIFT;
            if (HIBYTE(mapped) & 2) result.modifiers |= MOD_CONTROL;
            if (HIBYTE(mapped) & 4) result.modifiers |= MOD_ALT;
        }
    } else {
        static const QHash<int, UINT> special{{Qt::Key_Tab,VK_TAB}, {Qt::Key_Escape,VK_ESCAPE},
            {Qt::Key_Backspace,VK_BACK}, {Qt::Key_Return,VK_RETURN}, {Qt::Key_Insert,VK_INSERT},
            {Qt::Key_Delete,VK_DELETE}, {Qt::Key_Home,VK_HOME}, {Qt::Key_End,VK_END},
            {Qt::Key_PageUp,VK_PRIOR}, {Qt::Key_PageDown,VK_NEXT}, {Qt::Key_Left,VK_LEFT},
            {Qt::Key_Right,VK_RIGHT}, {Qt::Key_Up,VK_UP}, {Qt::Key_Down,VK_DOWN}, {Qt::Key_Pause,VK_PAUSE}};
        result.vk = special.value(key);
    }
    return result;
}
QString registrationError(DWORD code)
{
    if (code == ERROR_HOTKEY_ALREADY_REGISTERED) return QStringLiteral("组合键已被系统或其他程序占用");
    return QStringLiteral("Windows 无法注册此组合键（错误码 %1）").arg(code);
}
#endif
}

ShortcutManager::ShortcutManager(QWidget *window) : m_window(window)
{
    qApp->installNativeEventFilter(this);
}

ShortcutManager::~ShortcutManager()
{
    suspend();
    qApp->removeNativeEventFilter(this);
}

void ShortcutManager::add(Command command)
{
    if (!command.id.isEmpty() && command.invoke) m_commands.append(std::move(command));
}

QString ShortcutManager::normalized(const QString &text)
{
    const auto key = QKeySequence::fromString(text.trimmed(), QKeySequence::PortableText);
    if (key.count() != 1 || (key[0] & ~int(Qt::KeyboardModifierMask)) == Qt::Key_unknown) return {};
    return key.toString(QKeySequence::PortableText);
}

QString ShortcutManager::warning(const QVector<ShortcutBinding> &bindings, const ShortcutBinding &binding, bool probe) const
{
    if (binding.keys.isEmpty()) return {};
    const QString text = normalized(binding.keys);
    if (text.isEmpty()) return QStringLiteral("请输入一个组合键，例如 Ctrl+Alt+S。");
    const auto sequence = QKeySequence::fromString(text, QKeySequence::PortableText);
    for (auto *bar : m_window->findChildren<QMenuBar *>()) {
        if (!binding.global && bar->window() != m_window) continue;
        for (auto *menu : bar->actions()) {
            if (QKeySequence::mnemonic(menu->text()) == sequence)
                return QStringLiteral("与菜单「%1」的访问键冲突，菜单优先。").arg(menu->text().section('(', 0, 0));
        }
    }
    for (auto *action : m_window->findChildren<QAction *>()) {
        if (!action->shortcuts().contains(sequence)) continue;
        // 菜单自身是子窗口，菜单中的动作仍属于主窗口。
        QWidget *owner = qobject_cast<QWidget *>(action->parent());
        if (!binding.global && owner && owner->window() != m_window && !owner->inherits("QMenu")) continue;
        return QStringLiteral("与「%1」冲突，原快捷键优先。").arg(action->text().remove('&'));
    }
    for (const auto &other : bindings) {
        if (other.action == binding.action) break;
        if (normalized(other.keys) != text) continue;
        for (const auto &command : m_commands)
            if (command.id == other.action) return QStringLiteral("与「%1」冲突，原快捷键优先。").arg(command.title);
    }
    if (!binding.global && !probe) return {};
#ifdef Q_OS_WIN
    const auto key = nativeKey(sequence);
    if (binding.global && (!key.vk || key.vk == VK_F12))
        return QStringLiteral("该按键不支持注册为 Windows 全局快捷键。");
    if (probe && key.vk && key.vk != VK_F12) {
        const HWND window = reinterpret_cast<HWND>(m_window->winId());
        constexpr int probeId = 0x6fff;
        if (!RegisterHotKey(window, probeId, key.modifiers | MOD_NOREPEAT, key.vk)) return registrationError(GetLastError());
        UnregisterHotKey(window, probeId);
    }
#else
    Q_UNUSED(probe);
    if (binding.global) return QStringLiteral("当前系统不支持全局快捷键。");
#endif
    if (probe) {
        for (auto standard : {QKeySequence::Copy, QKeySequence::Cut, QKeySequence::Paste, QKeySequence::Undo,
                              QKeySequence::Redo, QKeySequence::SelectAll}) {
            if (QKeySequence::keyBindings(standard).contains(sequence))
                return binding.global ? QStringLiteral("此组合键也用于编辑文本，全局启用后会占用该按键。")
                                      : QStringLiteral("此组合键也用于编辑文本，输入框可能优先处理。");
        }
    }
    return {};
}

void ShortcutManager::suspend()
{
#ifdef Q_OS_WIN
    for (int id : m_global.keys()) UnregisterHotKey(reinterpret_cast<HWND>(m_handle), id);
#endif
    m_global.clear();
    m_globalChords.clear();
    qDeleteAll(m_local);
    m_local.clear();
}

QString ShortcutManager::apply(const QVector<ShortcutBinding> &bindings)
{
    suspend();
    QStringList failures;
    for (int i = 0; i < m_commands.size(); ++i) {
        const auto &command = m_commands.at(i);
        auto found = std::find_if(bindings.cbegin(), bindings.cend(), [&](const ShortcutBinding &b) { return b.action == command.id; });
        if (found == bindings.cend() || found->keys.isEmpty()) continue;
        QString reason = warning(bindings, *found, false);
        const auto keys = QKeySequence::fromString(normalized(found->keys), QKeySequence::PortableText);
        if (reason.isEmpty() && !found->global) {
            auto *shortcut = new QShortcut(keys, m_window);
            shortcut->setAutoRepeat(false);
            QObject::connect(shortcut, &QShortcut::activated, m_window, [this, i]() { activate(i); });
            m_local.append(shortcut);
        }
#ifdef Q_OS_WIN
        if (reason.isEmpty() && found->global) {
            const auto key = nativeKey(keys);
            const int id = 0x4000 + i;
            m_handle = quintptr(m_window->winId());
            if (RegisterHotKey(reinterpret_cast<HWND>(m_handle), id, key.modifiers | MOD_NOREPEAT, key.vk)) {
                m_global.insert(id, i);
                m_globalChords.insert(id, MAKELONG(key.modifiers, key.vk));
            } else {
                reason = registrationError(GetLastError());
            }
        }
#endif
        if (!reason.isEmpty()) failures.append(QStringLiteral("%1快捷键已保存，当前不可用：%2").arg(command.title, reason));
    }
    return failures.join('\n');
}

void ShortcutManager::activate(int index)
{
    if (QApplication::activeModalWidget() || index < 0 || index >= m_commands.size()) return;
    const auto &command = m_commands.at(index);
    Diagnostics::event("shortcut.triggered", {{"action_id", command.id}});
    QScopedValueRollback<bool> dispatch(m_dispatching, true);
    command.invoke();
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
bool ShortcutManager::nativeEventFilter(const QByteArray &, void *message, qintptr *result)
#else
bool ShortcutManager::nativeEventFilter(const QByteArray &, void *message, long *result)
#endif
{
#ifdef Q_OS_WIN
    const auto *msg = static_cast<const MSG *>(message);
    // 重新绑定后，消息队列里仍可能留有旧组合键的通知。
    if (msg && msg->message == WM_HOTKEY && quintptr(msg->hwnd) == m_handle && m_global.contains(int(msg->wParam))
        && m_globalChords.value(int(msg->wParam)) == quintptr(msg->lParam)) {
        if (result) *result = 0;
        activate(m_global.value(int(msg->wParam)));
        return true;
    }
#else
    Q_UNUSED(message);
    Q_UNUSED(result);
#endif
    return false;
}

ShortcutPreferences::ShortcutPreferences(ShortcutManager &manager, const QVector<ShortcutBinding> &saved, QWidget *parent)
    : QWidget(parent), m_manager(manager), m_draft(saved)
{
    auto *layout = new QVBoxLayout(this);
    auto *hint = new QLabel(QStringLiteral("点击“录入”后按组合键，也可直接输入，例如 Ctrl+Alt+S。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto *grid = new QGridLayout;
    layout->addLayout(grid);
    grid->addWidget(new QLabel(QStringLiteral("操作")), 0, 0);
    grid->addWidget(new QLabel(QStringLiteral("快捷键")), 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("生效范围")), 0, 4);
    int line = 1;
    for (const auto &command : manager.commands()) {
        auto *keys = new KeyField(this);
        keys->setObjectName(command.id + "Keys");
        keys->setPlaceholderText(QStringLiteral("未绑定"));
        auto *scope = new QComboBox(this);
        scope->setObjectName(command.id + "Scope");
        scope->addItems({QStringLiteral("仅主窗口"), QStringLiteral("全局（含托盘）")});
        auto *record = new QPushButton(QStringLiteral("录入"), this);
        record->setObjectName(command.id + "Record");
        auto *clear = new QPushButton(QStringLiteral("清除"), this);
        clear->setObjectName(command.id + "Clear");
        auto *warning = new QLabel(this);
        warning->setObjectName(command.id + "Warning");
        warning->setWordWrap(true);
        warning->setTextFormat(Qt::PlainText);
        warning->setStyleSheet("color: #925800;");
        for (const auto &b : saved) if (b.action == command.id) { keys->setText(b.keys); scope->setCurrentIndex(b.global); break; }
        const Row row{command.id, keys, scope, warning};
        m_rows.append(row);
        grid->addWidget(new QLabel(command.title), line, 0);
        grid->addWidget(keys, line, 1);
        grid->addWidget(record, line, 2);
        grid->addWidget(clear, line, 3);
        grid->addWidget(scope, line, 4);
        grid->addWidget(warning, ++line, 1, 1, 4);
        ++line;
        connect(record, &QPushButton::clicked, keys, &KeyField::record);
        connect(clear, &QPushButton::clicked, keys, &QLineEdit::clear);
        connect(keys, &QLineEdit::textChanged, this, [this, row]() { edited(row); });
        connect(scope, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, row]() { edited(row); });
        warning->setText(manager.warning(saved, {command.id, keys->text(), scope->currentIndex() == 1}, true));
        warning->setVisible(!warning->text().isEmpty());
    }
    grid->setColumnStretch(1, 1);
    layout->addStretch();
}

void ShortcutPreferences::edited(const Row &row)
{
    m_draft.erase(std::remove_if(m_draft.begin(), m_draft.end(), [&](const ShortcutBinding &b) { return b.action == row.action; }), m_draft.end());
    const QString text = row.keys->text().trimmed();
    const QString canonical = ShortcutManager::normalized(text);
    if (!text.isEmpty()) m_draft.append({row.action, canonical.isEmpty() ? text : canonical, row.scope->currentIndex() == 1});
    for (const auto &item : m_rows) {
        item.warning->setText(m_manager.warning(m_draft, {item.action, item.keys->text(), item.scope->currentIndex() == 1}, true));
        item.warning->setVisible(!item.warning->text().isEmpty());
    }
}

bool ShortcutPreferences::valid() const
{
    for (const auto &row : m_rows)
        if (!row.keys->text().trimmed().isEmpty() && ShortcutManager::normalized(row.keys->text()).isEmpty()) return false;
    return true;
}
