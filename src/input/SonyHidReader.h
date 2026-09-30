#pragma once
#include "input/SonyReportLayout.h"

#include <QByteArray>
#include <QString>

#include <functional>
#include <memory>

// Opt-in spike (bt-ds02): a focus-independent reader for ONE Sony/DS4 gamepad
// HID collection. Raw Input stopped delivering a Bluetooth DualSense while a
// particular game was foreground, and WinMM never re-enumerated the Bluetooth
// instance after a USB -> Bluetooth switch; a plain HID handle does not depend
// on focus or on a per-process joystick cache.
//
// Strictly read-only: GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
// overlapped ReadFile on a worker thread. It never sends output or feature
// reports, so it cannot switch a Bluetooth pad between simple and full mode
// underneath the game. Decoding stays in DualSenseDevice::parseReport; this
// class only forwards raw state reports whose control signature changed, plus
// a heartbeat so the owner can tell a healthy idle pad from a dead reader.
//
// Owned per tracked endpoint by DualSenseDevice, which is the only thing that
// knows the endpoint's lifetime. The destructor cancels the pending read and
// joins the worker, so no callback runs after it returns.
class SonyHidReaderHandle
{
public:
    virtual ~SonyHidReaderHandle() = default;
};

class SonyHidReader final : public SonyHidReaderHandle
{
public:
    struct Callbacks {
        // Worker thread. One state report (report id at byte 0).
        std::function<void(const QByteArray& report)> report;
        // Worker thread, at most once, only for failures — never for a
        // requested stop (destruction).
        std::function<void(const QString& reason)> failed;
    };

    // Forward at least one report this often while the pad streams, even when
    // no control changed. The owner's failover grace is built on top of it.
    static constexpr int kHeartbeatMs = 100;

    // Null (with `error` set) if the collection cannot be opened read-only —
    // exclusive access by another tool, HidHide cloaking, a non-gamepad
    // collection or a device that vanished mid-call. Never throws.
    static std::unique_ptr<SonyHidReader> open(const QString& devicePath,
                                               SonyReportLayout::Family family,
                                               Callbacks callbacks, QString* error);
    ~SonyHidReader() override;

    SonyHidReader(const SonyHidReader&) = delete;
    SonyHidReader& operator=(const SonyHidReader&) = delete;

private:
    struct Impl;
    explicit SonyHidReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};
