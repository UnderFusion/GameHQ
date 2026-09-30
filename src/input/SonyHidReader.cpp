#include "input/SonyHidReader.h"

#include <QElapsedTimer>
#include <QVector>

#include <thread>

#include <windows.h>
// MinGW's HID headers ship without extern "C" guards (see RawInputApi.cpp).
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

namespace
{
constexpr USHORT kUsagePageGeneric = 0x01;
constexpr USHORT kUsageJoystick    = 0x04;
constexpr USHORT kUsageGamepad     = 0x05;
constexpr USHORT kUsageMultiAxis   = 0x08;

QString win32Error(const char* what, DWORD code)
{
    return QStringLiteral("%1 failed (error %2)").arg(QLatin1String(what)).arg(code);
}
} // namespace

struct SonyHidReader::Impl {
    HANDLE file = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE readEvent = nullptr;
    int reportLength = 0;
    SonyReportLayout::Family family = SonyReportLayout::Family::DualSense;
    Callbacks callbacks;
    std::thread worker;

    ~Impl()
    {
        if (readEvent)
            CloseHandle(readEvent);
        if (stopEvent)
            CloseHandle(stopEvent);
        if (file != INVALID_HANDLE_VALUE)
            CloseHandle(file);
    }

    void run()
    {
        QVector<unsigned char> buffer(reportLength);
        OVERLAPPED ov{};
        ov.hEvent = readEvent;
        QElapsedTimer clock;
        clock.start();
        quint64 lastSignature = 0;
        qint64 lastForwardMs = -kHeartbeatMs;
        QString failure;

        for (;;) {
            ResetEvent(readEvent);
            DWORD got = 0;
            if (!ReadFile(file, buffer.data(), DWORD(reportLength), nullptr, &ov)
                && GetLastError() != ERROR_IO_PENDING) {
                failure = win32Error("ReadFile", GetLastError());
                break;
            }
            const HANDLE waits[2] = {stopEvent, readEvent};
            const DWORD woke = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (woke == WAIT_OBJECT_0) {
                // Requested stop: cancel exactly our read and wait for the
                // kernel to finish with the buffer before it goes away.
                CancelIoEx(file, &ov);
                GetOverlappedResult(file, &ov, &got, TRUE);
                return;
            }
            if (woke != WAIT_OBJECT_0 + 1) {
                failure = win32Error("WaitForMultipleObjects", GetLastError());
                CancelIoEx(file, &ov);
                GetOverlappedResult(file, &ov, &got, TRUE);
                break;
            }
            if (!GetOverlappedResult(file, &ov, &got, FALSE)) {
                // ERROR_DEVICE_NOT_CONNECTED on unplug / Bluetooth drop.
                failure = win32Error("ReadFile completion", GetLastError());
                break;
            }
            if (got == 0)
                continue;

            const quint64 sig = SonyReportLayout::controlSignature(
                buffer.constData(), int(got), family);
            if (sig == 0)
                continue;   // not a state report we decode (feature/aux ids)
            const qint64 now = clock.elapsed();
            if (sig == lastSignature && now - lastForwardMs < kHeartbeatMs)
                continue;
            lastSignature = sig;
            lastForwardMs = now;
            callbacks.report(QByteArray(reinterpret_cast<const char*>(buffer.constData()),
                                        int(got)));
        }
        if (callbacks.failed)
            callbacks.failed(failure);
    }
};

std::unique_ptr<SonyHidReader> SonyHidReader::open(const QString& devicePath,
                                                   SonyReportLayout::Family family,
                                                   Callbacks callbacks, QString* error)
{
    auto fail = [error](const QString& why) {
        if (error)
            *error = why;
        return std::unique_ptr<SonyHidReader>();
    };
    if (devicePath.isEmpty() || !callbacks.report)
        return fail(QStringLiteral("no device path"));

    auto impl = std::make_unique<Impl>();
    impl->family = family;
    impl->callbacks = std::move(callbacks);
    // GENERIC_READ only: no write access means no output/feature reports can
    // ever be sent through this handle. Share both ways so the game, Steam or
    // a remapper keep their own handles.
    impl->file = CreateFileW(reinterpret_cast<LPCWSTR>(devicePath.utf16()), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED, nullptr);
    if (impl->file == INVALID_HANDLE_VALUE)
        return fail(win32Error("CreateFile", GetLastError()));

    PHIDP_PREPARSED_DATA preparsed = nullptr;
    if (!HidD_GetPreparsedData(impl->file, &preparsed))
        return fail(win32Error("HidD_GetPreparsedData", GetLastError()));
    HIDP_CAPS caps{};
    const NTSTATUS status = HidP_GetCaps(preparsed, &caps);
    HidD_FreePreparsedData(preparsed);
    if (status != HIDP_STATUS_SUCCESS)
        return fail(QStringLiteral("HidP_GetCaps failed"));
    // Same collection filter as the Raw Input classifier: gamepad-class only.
    if (caps.UsagePage != kUsagePageGeneric
        || (caps.Usage != kUsageGamepad && caps.Usage != kUsageJoystick
            && caps.Usage != kUsageMultiAxis))
        return fail(QStringLiteral("not a gamepad collection"));
    if (caps.InputReportByteLength < 2)
        return fail(QStringLiteral("no input reports"));
    impl->reportLength = caps.InputReportByteLength;

    impl->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    impl->readEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl->stopEvent || !impl->readEvent)
        return fail(win32Error("CreateEvent", GetLastError()));

    Impl* raw = impl.get();
    impl->worker = std::thread([raw] { raw->run(); });
    return std::unique_ptr<SonyHidReader>(new SonyHidReader(std::move(impl)));
}

SonyHidReader::SonyHidReader(std::unique_ptr<Impl> impl)
    : d(std::move(impl))
{
}

SonyHidReader::~SonyHidReader()
{
    SetEvent(d->stopEvent);
    if (d->worker.joinable())
        d->worker.join();
}
