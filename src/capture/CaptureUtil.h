#pragma once

// Shared helpers for the capture/replay subsystem. These existed as separate
// per-file copies that had already drifted (the AudioCapture QPC conversion
// could overflow); keep the single source of truth here.

#include <windows.h>

#include <QDateTime>
#include <QDebug>
#include <QFile>
#include <QString>

namespace CaptureUtil {

// Stale replay-cache age shared by the SegmentRecorder ring restore and the
// worker-start sweep in FramePumpService. Must stay MUCH wider than the
// configured replay length: the buffer disarms on every focus loss and the
// ring's file mtimes are finalise times, so a tight threshold would wipe a
// healthy ring on almost every re-arm (see the note in SegmentRecorder::begin).
constexpr qint64 kStaleSegmentMaxAgeSecs = 600;

// Log an HRESULT failure with a subsystem prefix; returns true on success.
inline bool ok(const char* subsystem, const char* what, HRESULT hr)
{
    if (FAILED(hr))
        qWarning().nospace() << subsystem << ": " << what
                             << " failed hr=0x" << Qt::hex << quint32(hr);
    return SUCCEEDED(hr);
}

// QPC → 100 ns units. Whole seconds and the remainder are converted
// separately: a naive counter*10^7 multiply overflows int64 after roughly a
// day of uptime at a 10 MHz QPC frequency. Both A/V sync clocks use this.
inline long long qpcNow100ns()
{
    LARGE_INTEGER qpc{}, freq{};
    QueryPerformanceCounter(&qpc);
    QueryPerformanceFrequency(&freq);
    return (qpc.QuadPart / freq.QuadPart) * 10000000LL
         + (qpc.QuadPart % freq.QuadPart) * 10000000LL / freq.QuadPart;
}

// dir/<stamp><suffix>, appending _2, _3, ... before the suffix when the
// timestamped name already exists (same-second/same-millisecond clobber).
inline QString uniqueTimestampedPath(const QString& dir, const QString& stampFormat,
                                     const QString& suffix)
{
    const QString stamp = QDateTime::currentDateTime().toString(stampFormat);
    QString path = dir + QLatin1Char('/') + stamp + suffix;
    for (int i = 2; QFile::exists(path); ++i)
        path = dir + QLatin1Char('/') + stamp + QStringLiteral("_%1").arg(i) + suffix;
    return path;
}

// Real OS build, not the marketing version: GetVersionEx() lies to unmanifested
// processes, RtlGetVersion() does not. 0 means "could not read it".
inline unsigned long windowsBuildNumber()
{
    static const unsigned long build = []() -> unsigned long {
        using PfnRtlGetVersion = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
        const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        const auto pfn = ntdll ? reinterpret_cast<PfnRtlGetVersion>(
                                     reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")))
                               : nullptr;
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        if (!pfn || pfn(&info) != 0)
            return 0;
        return info.dwBuildNumber;
    }();
    return build;
}

} // namespace CaptureUtil
