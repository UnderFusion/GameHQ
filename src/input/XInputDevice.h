#pragma once
#include "input/Gamepad.h"

class QTimer;

// Fallback for DSX Xbox mode and real Xbox pads. Xbox controllers do not
// expose a Share/Create button through standard XInput, so Back maps to
// Share; the Guide button (XInputGetStateEx, xinput1_4 ordinal 100) maps to
// PS when the loaded XInput DLL provides it.
//
// XInputGetState on an EMPTY slot can stall for milliseconds (documented
// XInput pitfall), so empty slots are never polled continuously: they are
// probed once at start, on rescan() — driven by the Raw Input backend's
// device-topology hint — and by a slow safety-net timer. Connected slots
// are polled fast; a slot that stops answering emits releases for its held
// buttons before reporting the disconnect.
class XInputDevice : public Gamepad
{
    Q_OBJECT
public:
    explicit XInputDevice(QObject* parent = nullptr);
    ~XInputDevice() override;

    bool start() override;
    ControlId::DeviceProfile profile() const override;
    ControlId::DeviceProfile profileForSlot(int slot) const;

    // Pure XInput button/trigger normalization, exposed so the canonical
    // control each physical button produces can be compared against the Sony
    // HID and WinMM views of the same pad without a real device.
    static quint32 mapDigitalButtons(quint16 buttons, quint8 leftTrigger,
                                     quint8 rightTrigger);

    // Stable hardware identity ("vvvv:pppp") for the connected pad when the
    // Raw Input topology correlation is unambiguous, empty otherwise.
    // XInput cannot know this itself — InputEngine injects it (see
    // ControllerIdentity). With no identity, profile() keeps the honest
    // legacy "xinput.slotN" fingerprint and says so in its display name.
    void setKnownDeviceIdentity(int slot, const QString& endpointId,
                                const QString& modelFingerprint = {});
    int connectedSlotCount() const { return connectedCount(); }
    int firstConnectedSlot() const;
    bool slotConnected(int slot) const
    {
        return slot >= 0 && slot < 4 && m_connected[slot];
    }

public slots:
    void rescan();   // probe empty slots for newly arrived pads

signals:
    void slotConnectionChanged(int slot, bool connected);

protected:
    // Protected test seam: tests exercise the production edge path without
    // loading or polling a system XInput DLL.
    void setSlotState(int slot, quint32 buttons, bool connected);

private:
    void poll();     // fast-poll connected slots only
    int connectedCount() const;

    QTimer* m_pollTimer = nullptr;     // runs only while a slot is connected
    QTimer* m_rescanTimer = nullptr;   // slow safety net for missed arrivals
    void* m_library = nullptr;
    using XInputGetStateFn = unsigned long(__stdcall*)(unsigned long, void*);
    XInputGetStateFn m_getState = nullptr;   // XInputGetStateEx when available
    quint32 m_prevButtons[4] = {};
    // Raw wButtons per slot for the diagnostics probe: the mapped mask above
    // drops bits GameHQ has no binding for, which are exactly the ones the
    // probe exists to reveal.
    quint16 m_prevRawButtons[4] = {};
    bool m_connected[4] = {};
    bool m_anyConnected = false;       // aggregate, as last emitted
    QString m_knownEndpoint[4];        // anonymized Raw Input endpoint when unambiguous
    QString m_modelFingerprint[4];     // VID/PID compatibility alias only
};
