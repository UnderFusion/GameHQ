#pragma once
#include "input/Gamepad.h"

#include <climits>
#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

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
// While connected, the active slot is sampled on its own worker thread and
// only state CHANGES are handed to the owning thread, in order. joyGetPosEx
// reports "is it down right now", so sampling on the GUI thread lost every
// tap that started and ended inside a GUI stall (50-200 ms stalls are common
// while the overlay animates) — the user had to press twice. A change is now
// delivered late at worst, never dropped. On unplug the held buttons are
// released before the disconnect is reported, then arrival scanning resumes.
class WinMMDevice : public Gamepad
{
    Q_OBJECT
public:
    explicit WinMMDevice(QObject* parent = nullptr);
    ~WinMMDevice() override;

    bool start() override;
    ControlId::DeviceProfile profile() const override;

    // Pure DirectInput button-mask normalization, exposed so the canonical
    // control each physical button produces can be compared against the Sony
    // HID and XInput views of the same pad without a real device.
    static quint32 mapDigitalButtons(quint32 rawButtons, bool ds4Layout);

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
    // Worker thread; touches no members.
    static ScanResult scanSlots();
    void applyScanResult(const ScanResult& result);
    // One sampled reading; `result` is the joyGetPosEx code.
    struct Sample {
        UINT result = 0;
        quint32 state = 0;
    };
    void startSampler();
    void stopSampler();
    void pushSample(const Sample& sample);   // worker thread
    void drainSamples();                     // owning thread
    void applyState(quint32 state);
    void disconnectActive();
    void emitEdges(quint32 buttons);

    QTimer* m_rescanTimer = nullptr;   // slow safety net while disconnected
    std::thread m_scanThread;          // joined before reuse and in the dtor
    std::thread m_sampleThread;        // runs only while connected
    std::atomic<bool> m_sampleStop{false};
    std::mutex m_sampleMutex;          // guards the two members below
    std::vector<Sample> m_samples;
    bool m_drainPosted = false;
    bool m_scanInFlight = false;       // owning thread only
    quint32 m_prevButtons = 0;
    // The first reading after a connect is the device's resting state, not a
    // press: some pads (or their drivers) idle with an axis at 0, which would
    // otherwise decode as D-pad Up+Left held forever.
    bool m_baselinePending = false;
    bool m_connected = false;
    bool m_ds4Layout = false;   // Sony button order (Share=8, PS=12) vs Xbox
    UINT m_activeId = UINT_MAX;
    quint32 m_vendorId = 0;
    quint32 m_productId = 0;
};
