#pragma once
#include <QAbstractNativeEventFilter>
#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QString>

// Global keyboard hotkeys via Win32 RegisterHotKey (docs/controller-input.md).
// Bindings are keyed by a stable ActionCatalog action id and registered
// through a generic, transactional core: applyBinding()/clearBinding() are
// runtime rebinding support (see docs/controller-input.md). The built-in
// defaults flow through the same path, so it is exercised on every startup.
class HotkeyManager : public QObject, public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit HotkeyManager(QObject* parent = nullptr);
    ~HotkeyManager() override;

    // Result of parseChord(): a validated modifier+key pair, or invalid with
    // a human-readable reason (shown by a future binding editor).
    struct ParsedChord
    {
        bool valid = false;
        quint32 modifiers = 0;   // MOD_CONTROL | MOD_SHIFT | MOD_ALT | MOD_NOREPEAT
        quint32 vk = 0;          // virtual-key code
        QString rejectionReason;
    };

    // Parses strings like "Ctrl+Shift+G". Requires at least one modifier
    // (rejects bare global keys) and rejects the Win modifier and the
    // Alt+F4 system combo (reserved combinations).
    static ParsedChord parseChord(const QString& text);

    // Registers a NEW chord for actionId before touching the old one: if
    // registration fails, the previous chord (if any) is left active and
    // this returns false. Only succeeds once the new chord is confirmed, at
    // which point the old registration (if different) is released.
    bool applyBinding(const QString& actionId, quint32 modifiers, quint32 vk);
    bool applyBindingSlot(const QString& actionId, int slot,
                          quint32 modifiers, quint32 vk);

    // Releases actionId's global hotkey, if any. The action becomes
    // keyboard-unbound; other actions' registrations are untouched.
    bool clearBinding(const QString& actionId);
    bool clearBindingSlot(const QString& actionId, int slot);

    bool nativeEventFilter(const QByteArray& eventType, void* message,
                           qintptr* result) override;

signals:
    void overlayTogglePressed();
    void screenshotPressed();
    void saveReplayPressed();

    // Generic counterpart to the four signals above, keyed by action id —
    // for binding-runtime callers that don't need a dedicated
    // signal per action.
    void hotkeyTriggered(const QString& actionId);

private:
    struct Binding
    {
        QString actionId;
        quint32 modifiers = 0;
        quint32 vk = 0;
        int hotkeyId = 0;   // currently-registered Win32 id; 0 = unregistered
        QElapsedTimer lastFire;
    };

    void dispatch(const QString& actionId);

    QHash<QString, Binding> m_bindings;   // keyed by action id
    QHash<int, QString> m_idToAction;     // Win32 hotkey id -> action id, for WM_HOTKEY lookup
    int m_nextId = 1;                     // never reused within the process lifetime
};
