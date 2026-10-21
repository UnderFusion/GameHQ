#include "input/HotkeyManager.h"

#include <QCoreApplication>
#include <QDebug>
#include <QStringList>

#include <windows.h>

HotkeyManager::HotkeyManager(QObject* parent)
    : QObject(parent)
{
    QCoreApplication::instance()->installNativeEventFilter(this);
}

HotkeyManager::~HotkeyManager()
{
    for (auto it = m_bindings.constBegin(); it != m_bindings.constEnd(); ++it) {
        if (it->hotkeyId != 0)
            UnregisterHotKey(nullptr, it->hotkeyId);
    }
    if (auto* app = QCoreApplication::instance())
        app->removeNativeEventFilter(this);
}

HotkeyManager::ParsedChord HotkeyManager::parseChord(const QString& text)
{
    ParsedChord result;
    const QStringList parts = text.split(QLatin1Char('+'), Qt::SkipEmptyParts);
    if (parts.size() < 2) {
        result.rejectionReason = QStringLiteral("A global shortcut needs at least one modifier plus a key.");
        return result;
    }

    quint32 modifiers = MOD_NOREPEAT;
    bool sawWin = false;
    for (int i = 0; i < parts.size() - 1; ++i) {
        const QString mod = parts[i].trimmed().toLower();
        if (mod == QLatin1String("ctrl") || mod == QLatin1String("control"))
            modifiers |= MOD_CONTROL;
        else if (mod == QLatin1String("shift"))
            modifiers |= MOD_SHIFT;
        else if (mod == QLatin1String("alt"))
            modifiers |= MOD_ALT;
        else if (mod == QLatin1String("win") || mod == QLatin1String("windows") || mod == QLatin1String("meta"))
            sawWin = true;
        else {
            result.rejectionReason = QStringLiteral("Unknown modifier \"%1\".").arg(parts[i]);
            return result;
        }
    }
    if (sawWin) {
        result.rejectionReason = QStringLiteral("The Windows key is reserved for OS shortcuts.");
        return result;
    }
    if ((modifiers & ~static_cast<quint32>(MOD_NOREPEAT)) == 0) {
        result.rejectionReason = QStringLiteral("A global shortcut needs at least one modifier plus a key.");
        return result;
    }

    const QString keyPart = parts.last().trimmed().toUpper();
    quint32 vk = 0;
    if (keyPart.size() == 1) {
        const QChar c = keyPart.at(0);
        if (c.isLetterOrNumber())
            vk = c.toLatin1();
    } else if (keyPart.size() >= 2 && keyPart.size() <= 3 && keyPart.at(0) == QLatin1Char('F')) {
        bool ok = false;
        const int n = keyPart.mid(1).toInt(&ok);
        if (ok && n >= 1 && n <= 24)
            vk = VK_F1 + (n - 1);
    }
    if (vk == 0) {
        result.rejectionReason = QStringLiteral("Unsupported key \"%1\".").arg(parts.last());
        return result;
    }

    // Alt+F4 (with no other modifier) is a reserved system combo — RegisterHotKey
    // would only ever silently lose the race with the shell for it.
    if (modifiers == (MOD_ALT | MOD_NOREPEAT) && vk == VK_F4) {
        result.rejectionReason = QStringLiteral("Alt+F4 is reserved by Windows.");
        return result;
    }

    result.valid = true;
    result.modifiers = modifiers;
    result.vk = vk;
    return result;
}

bool HotkeyManager::applyBinding(const QString& actionId, quint32 modifiers, quint32 vk)
{
    return applyBindingSlot(actionId, 1, modifiers, vk);
}

bool HotkeyManager::applyBindingSlot(const QString& actionId, int slot,
                                     quint32 modifiers, quint32 vk)
{
    const QString bindingKey = actionId + QLatin1Char('#') + QString::number(slot);
    const auto existing = m_bindings.constFind(bindingKey);
    if (existing != m_bindings.cend() && existing->hotkeyId != 0
        && existing->modifiers == modifiers && existing->vk == vk)
        return true;

    const int newId = m_nextId++;
    if (!RegisterHotKey(nullptr, newId, modifiers, vk)) {
        qWarning().noquote() << QStringLiteral("Hotkey: rebind failed for action \"%1\" (chord in use by another app?)").arg(actionId);
        return false;
    }

    // New chord confirmed working — only now release the previous one, so a
    // failed attempt above never disturbs the binding that was already live.
    auto it = m_bindings.find(bindingKey);
    if (it != m_bindings.end() && it->hotkeyId != 0) {
        UnregisterHotKey(nullptr, it->hotkeyId);
        m_idToAction.remove(it->hotkeyId);
    }

    Binding& b = m_bindings[bindingKey];
    b.actionId = actionId;
    b.modifiers = modifiers;
    b.vk = vk;
    b.hotkeyId = newId;
    b.lastFire.invalidate();
    m_idToAction[newId] = bindingKey;
    qInfo().noquote() << QStringLiteral("Hotkey: rebound action \"%1\"").arg(actionId);
    return true;
}

bool HotkeyManager::clearBinding(const QString& actionId)
{
    bool ok = true;
    for (int slot = 1; slot <= 2; ++slot)
        ok = clearBindingSlot(actionId, slot) && ok;
    // Compatibility with whole-action keys (the retired registerDefault() path).
    auto it = m_bindings.find(actionId);
    if (it == m_bindings.end() || it->hotkeyId == 0)
        return ok;
    UnregisterHotKey(nullptr, it->hotkeyId);
    m_idToAction.remove(it->hotkeyId);
    m_bindings.erase(it);
    qInfo().noquote() << QStringLiteral("Hotkey: cleared action \"%1\"").arg(actionId);
    return ok;
}

bool HotkeyManager::clearBindingSlot(const QString& actionId, int slot)
{
    const QString bindingKey = actionId + QLatin1Char('#') + QString::number(slot);
    auto it = m_bindings.find(bindingKey);
    if (it == m_bindings.end() || it->hotkeyId == 0)
        return true;
    UnregisterHotKey(nullptr, it->hotkeyId);
    m_idToAction.remove(it->hotkeyId);
    m_bindings.erase(it);
    return true;
}

bool HotkeyManager::nativeEventFilter(const QByteArray& eventType, void* message,
                                      qintptr* result)
{
    Q_UNUSED(result);
    if (eventType != "windows_generic_MSG")
        return false;
    const MSG* msg = static_cast<MSG*>(message);
    if (msg->message != WM_HOTKEY)
        return false;

    const QString bindingKey = m_idToAction.value(static_cast<int>(msg->wParam));
    if (bindingKey.isEmpty())
        return false;

    dispatch(bindingKey);
    return true;
}

void HotkeyManager::dispatch(const QString& bindingKey)
{
    auto it = m_bindings.find(bindingKey);
    if (it == m_bindings.end())
        return;
    if (it->lastFire.isValid() && it->lastFire.elapsed() < 250)
        return;   // duplicate delivery (e.g. injected input) — swallow
    it->lastFire.restart();

    // Copy, not reference: a synchronous slot on the signals below may rebind
    // or clear this very binding, invalidating `it` and anything it points at.
    const QString actionId = it->actionId;
    if (actionId == QLatin1String("global.toggle_overlay"))
        emit overlayTogglePressed();
    else if (actionId == QLatin1String("global.screenshot"))
        emit screenshotPressed();
    else if (actionId == QLatin1String("global.save_replay"))
        emit saveReplayPressed();
    emit hotkeyTriggered(actionId);
}
