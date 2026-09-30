#pragma once
#include "input/Gamepad.h"

#include <climits>
#include <windows.h>

#include <chrono>
#include <condition_variable>
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
// because probing all 16 empty slots at poll rate is wasted work. While
// connected, the active slot is sampled every few ms and only state CHANGES
// are handed to the owning thread, in order, so a tap that starts and ends
// inside a GUI stall is delivered late rather than dropped.
//
// Every joy* call runs on ONE long-lived worker thread, one call at a time:
// the legacy WinMM/DirectInput stack is not safe to drive from several or
// short-lived threads, and touching it right after a joystick disappears
// corrupted the process heap (three field crashes, 2026-09-30, each on the
// first WinMM call ~0.5 s after an unplug). So after an unplug or a device
// topology change the worker also stays away from WinMM for a quiet period
// before it scans again. Only results are marshalled back to the owning
// thread; all state, timers and signals stay there.
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
    // A HID device arrived or left: hold off WinMM for the quiet period, then
    // scan. The stack is still tearing the old device down at this point.
    void rescanAfterTopologyChange();

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
    void workerLoop();   // the only code that calls into WinMM
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
    std::thread m_worker;              // started in start(), joined in the dtor
    // Worker requests, guarded by m_workerMutex.
    std::mutex m_workerMutex;
    std::condition_variable m_workerCv;
    bool m_quit = false;
    bool m_scanRequested = false;
    UINT m_sampleId = UINT_MAX;        // slot to sample, UINT_MAX = none
    bool m_sampleDs4 = false;
    quint64 m_sampleGen = 0;           // bumped on every start/stop request
    quint64 m_workerGen = 0;           // generation the worker has adopted
    std::chrono::steady_clock::time_point m_quietUntil{};
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
