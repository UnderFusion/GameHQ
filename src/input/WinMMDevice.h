#pragma once
#include "input/Gamepad.h"

#include <climits>
#include <windows.h>

#include <thread>

class QTimer;

// Last-resort fallback using the legacy Windows multimedia joystick API
// (joyGetPosEx). Some virtual controllers (DSX, ViGEm, etc.) that do not
// appear through Raw Input or XInput still enumerate as a standard Windows
// joystick.
//
// While disconnected, slots are scanned only on rescan() — driven by the
// Raw Input backend's device-topology hint — plus a slow safety-net timer,
// because probing all 16 empty slots at poll rate is wasted work. The
// discovery sweep itself runs on a short-lived worker thread: asking the
// driver stack about up to 16 mostly absent devices was measured at 161 ms
// on a real machine, far too slow for the GUI thread (and repeating every
// 2 s on machines with no joystick at all). Only the result is marshalled
// back to the owning thread; all state, timers and signals stay there.
// While connected, the active slot is polled fast; on unplug the held
// buttons are released before the disconnect is reported, then arrival
// scanning resumes.
class WinMMDevice : public Gamepad
{
    Q_OBJECT
public:
    explicit WinMMDevice(QObject* parent = nullptr);
    ~WinMMDevice() override;

    bool start() override;
    ControlId::DeviceProfile profile() const override;

public slots:
    void rescan();   // kick a background scan for a newly arrived joystick

private:
    // What the worker thread found; everything Qt-visible happens in
    // applyScanResult() on the owning thread.
    struct ScanResult {
        bool found = false;
        UINT id = 0;
        quint32 mid = 0;
        quint32 pid = 0;
        qint64 elapsedUs = 0;
    };
    static ScanResult scanSlots();   // worker thread; touches no members
    void applyScanResult(const ScanResult& result);
    void poll();
    void disconnectActive();
    void emitEdges(quint32 buttons);

    QTimer* m_pollTimer = nullptr;     // runs only while connected
    QTimer* m_rescanTimer = nullptr;   // slow safety net while disconnected
    std::thread m_scanThread;          // joined before reuse and in the dtor
    bool m_scanInFlight = false;       // owning thread only
    quint32 m_prevButtons = 0;
    bool m_connected = false;
    bool m_ds4Layout = false;   // Sony button order (Share=8, PS=12) vs Xbox
    UINT m_activeId = UINT_MAX;
    quint32 m_vendorId = 0;
    quint32 m_productId = 0;
};
