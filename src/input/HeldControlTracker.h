#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

// cpo-o06e: "which controls are held right now" — the one piece of state the
// neutral-state handoff consults before it lets the exclusive GameInput policy
// go.
//
// It is fed from the RAW edges the engine receives: the moment a backend
// publishes a press or a release, before any routing, capture, provider
// confirmation or gesture arbitration. A control the user is holding has to
// count even when the press never fired an action, because what the game would
// inherit on close is the physical state, not GameHQ's interpretation of it.
//
// Nothing here interprets a device. Threshold and deadzone decisions stay where
// they already are — the active backend's own normalisation (GameInput publishes
// a trigger button only past its own threshold and a thumbstick-direction flag
// only past its own deadzone; the legacy backends use StickNav's per-backend
// AxisConfig). The handoff therefore cannot disagree with the interpretation
// that routed the press in the first place.
//
// A device that goes away takes its held controls with it: a controller that no
// longer exists cannot carry a held state into the game either, and leaving the
// entry behind would make every later handoff run into its timeout.
//
// Keys are engine-internal device identities (a backend pointer, a logical
// controller id). They are never printed: the receipt only names canonical
// control ids ("gamepad.cross"), never device paths or serials.
class HeldControlTracker
{
public:
    // The device is attached — present with nothing held yet. Idempotent.
    void noteDevicePresent(const QString& deviceKey)
    {
        if (!deviceKey.isEmpty())
            m_devices[deviceKey];
    }

    // The device is gone. Whatever it was holding went with it — including the
    // count, or a handoff would wait forever for a release that cannot arrive.
    void noteDeviceGone(const QString& deviceKey)
    {
        auto device = m_devices.find(deviceKey);
        if (device == m_devices.end())
            return;
        m_heldCount -= device->size();
        m_devices.erase(device);
    }

    void notePressed(const QString& deviceKey, const QString& controlId)
    {
        if (deviceKey.isEmpty() || controlId.isEmpty())
            return;
        QSet<QString>& held = m_devices[deviceKey];
        if (!held.contains(controlId)) {
            held.insert(controlId);
            ++m_heldCount;
        }
    }

    void noteReleased(const QString& deviceKey, const QString& controlId)
    {
        auto device = m_devices.find(deviceKey);
        if (device == m_devices.end() || !device->remove(controlId))
            return;
        --m_heldCount;
    }

    void reset()
    {
        m_devices.clear();
        m_heldCount = 0;
    }

    // Devices the engine can still read, whether or not anything is held.
    int presentDevices() const { return m_devices.size(); }
    int heldCount() const { return m_heldCount; }
    bool anyHeld() const { return m_heldCount > 0; }

    // Deterministic (sorted) and bounded, for the receipt: `limit` ids followed
    // by a count of the rest. Control ids are canonical GameHQ names.
    QStringList heldControls(int limit = 0) const
    {
        QStringList ids;
        for (auto device = m_devices.cbegin(); device != m_devices.cend(); ++device) {
            for (const QString& id : *device)
                ids.append(id);
        }
        ids.sort();
        if (limit > 0 && ids.size() > limit) {
            const int extra = ids.size() - limit;
            ids = ids.mid(0, limit);
            ids.append(QStringLiteral("+%1 more").arg(extra));
        }
        return ids;
    }

private:
    QHash<QString, QSet<QString>> m_devices;
    int m_heldCount = 0;
};
