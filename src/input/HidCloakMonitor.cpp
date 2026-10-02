#include "input/HidCloakMonitor.h"

#include <QVarLengthArray>

#include <string>
#include <vector>

#include <windows.h>
#include <cfgmgr32.h>
#include <shellapi.h>

namespace
{
struct KnownPad { quint32 vid; quint32 pid; const char* name; };

// Mirrors supportedReportLayout() in DualSenseDevice.cpp. 054C:0ECC is
// deliberately absent: on PlayStation Link hardware that PID exposes only
// vendor-defined collections which Raw Input legitimately lists, so a
// PnP-vs-Raw-Input comparison on it would never be meaningful.
constexpr KnownPad kKnownPads[] = {
    { 0x054C, 0x0CE6, "DualSense" },
    { 0x054C, 0x0DF2, "DualSense Edge" },
    { 0x054C, 0x05C4, "DualShock 4" },
    { 0x054C, 0x09CC, "DualShock 4 v2" },
    { 0x11FF, 0x0847, "DSX virtual DS4" },
    { 0x3670, 0x0902, "DSX virtual DS4" },
};

QString vidPidToken(quint32 vid, quint32 pid)
{
    return QStringLiteral("vid_%1&pid_%2")
        .arg(vid, 4, 16, QLatin1Char('0'))
        .arg(pid, 4, 16, QLatin1Char('0'));
}

// Bluetooth HID devnodes and Raw Input paths spell the IDs differently:
// "{00001124-...}_vid&0002054c_pid&0ce6" (0002 = USB-IF vendor source,
// 0001 = Bluetooth SIG). Without these a Bluetooth pad never matched.
QStringList vidPidTokens(quint32 vid, quint32 pid)
{
    const QString v = QStringLiteral("%1").arg(vid, 4, 16, QLatin1Char('0'));
    const QString p = QStringLiteral("%1").arg(pid, 4, 16, QLatin1Char('0'));
    return { vidPidToken(vid, pid),
             QStringLiteral("vid&0002%1_pid&%2").arg(v, p),
             QStringLiteral("vid&0001%1_pid&%2").arg(v, p) };
}

bool containsAny(const QString& haystack, const QStringList& tokens)
{
    for (const QString& t : tokens) {
        if (haystack.contains(t))
            return true;
    }
    return false;
}

// HidHide control-device interface (github.com/nefarius/HidHide, HidHideApi.h).
// CTL_CODE(32769, function, METHOD_BUFFERED, FILE_READ_DATA) — the driver uses
// FILE_READ_DATA for both getters and setters.
constexpr DWORD kHidHideDeviceType = 32769;
constexpr DWORD kFuncGetWhitelist  = 2048;
constexpr DWORD kFuncSetWhitelist  = 2049;

DWORD hidHideCtlCode(DWORD function)
{
    return (kHidHideDeviceType << 16) | (FILE_READ_DATA << 14)
         | (function << 2) | METHOD_BUFFERED;
}

// HidHide whitelist entries are full image paths in NT namespace form
// ("\Device\HarddiskVolumeN\...\GameHQ.exe"). Resolve our own.
QString ownImageNtPath()
{
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return {};
    HANDLE f = CreateFileW(exePath, 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return {};
    wchar_t ntPath[1024]{};
    const DWORD n = GetFinalPathNameByHandleW(f, ntPath, 1024,
                                              FILE_NAME_NORMALIZED | VOLUME_NAME_NT);
    CloseHandle(f);
    if (n == 0 || n >= 1024)
        return {};
    return QString::fromWCharArray(ntPath);
}
} // namespace

bool HidCloakMonitor::hidHideInstalled()
{
    // HidHide registers as an upper filter on the HID device class GUID.
    wchar_t data[2048]{};
    DWORD bytes = sizeof(data);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
                     L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
                     L"{745a17a0-74d3-11d0-b6fe-00a0c90f57da}",
                     L"UpperFilters", RRF_RT_REG_MULTI_SZ, nullptr,
                     data, &bytes) != ERROR_SUCCESS)
        return false;
    for (const wchar_t* p = data; *p; p += wcslen(p) + 1) {
        if (_wcsicmp(p, L"HidHide") == 0)
            return true;
    }
    return false;
}

HidCloakMonitor::ScanResult HidCloakMonitor::scan(const QSet<QString>& visibleRawPathsLower)
{
    ScanResult r;
    r.hidHidePresent = hidHideInstalled();

    // Present devnodes under the HID enumerator. This list comes from the PnP
    // manager, which still shows devices a HID filter cloaks from Raw Input /
    // DirectInput / joy.cpl — exactly the asymmetry that identifies a hidden pad.
    constexpr ULONG kFlags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG chars = 0;
    if (CM_Get_Device_ID_List_SizeW(&chars, L"HID", kFlags) != CR_SUCCESS || chars == 0)
        return r;
    QVarLengthArray<wchar_t, 4096> ids(static_cast<int>(chars));
    if (CM_Get_Device_ID_ListW(L"HID", ids.data(), chars, kFlags) != CR_SUCCESS)
        return r;

    QString pnpJoined;
    for (const wchar_t* p = ids.constData(); *p; p += wcslen(p) + 1) {
        pnpJoined += QString::fromWCharArray(p).toLower();
        pnpJoined += QLatin1Char('\n');
    }
    QString rawJoined;
    for (const QString& path : visibleRawPathsLower) {
        rawJoined += path;
        rawJoined += QLatin1Char('\n');
    }

    for (const auto& pad : kKnownPads) {
        const QString token = vidPidToken(pad.vid, pad.pid);
        const QStringList tokens = vidPidTokens(pad.vid, pad.pid);
        if (containsAny(pnpJoined, tokens) && !containsAny(rawJoined, tokens))
            r.hiddenPads << QStringLiteral("%1 (%2)")
                                .arg(QLatin1String(pad.name), token.toUpper());
    }
    return r;
}

QList<HidCloakMonitor::HiddenInterface>
HidCloakMonitor::hiddenPadInterfaces(const QSet<QString>& visibleRawPathsLower)
{
    // GUID_DEVINTERFACE_HID, spelled out to avoid an initguid.h dependency.
    static const GUID kHidInterface =
        { 0x4D1E55B2, 0xF16F, 0x11CF, { 0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30 } };
    QList<HiddenInterface> out;
    ULONG chars = 0;
    GUID guid = kHidInterface;
    if (CM_Get_Device_Interface_List_SizeW(&chars, &guid, nullptr,
                                           CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS
        || chars <= 1)
        return out;
    QVarLengthArray<wchar_t, 4096> buf(static_cast<int>(chars));
    if (CM_Get_Device_Interface_ListW(&guid, nullptr, buf.data(), chars,
                                      CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return out;
    for (const wchar_t* p = buf.constData(); *p; p += wcslen(p) + 1) {
        const QString path = QString::fromWCharArray(p);
        const QString lower = path.toLower();
        if (lower.contains(QLatin1String("ig_")) || visibleRawPathsLower.contains(lower))
            continue;
        for (const auto& pad : kKnownPads) {
            if (pad.vid != 0x054C)
                continue;   // physical Sony hardware only
            if (containsAny(lower, vidPidTokens(pad.vid, pad.pid))) {
                out.append({ path, pad.vid, pad.pid });
                break;
            }
        }
    }
    return out;
}

int HidCloakMonitor::applyWhitelistSelfElevated()
{
    const QString mine = ownImageNtPath();
    if (mine.isEmpty())
        return 4;

    HANDLE dev = CreateFileW(L"\\\\.\\HidHide", GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dev == INVALID_HANDLE_VALUE)
        return 2;   // driver absent, or still no admin rights

    int result = 0;
    // Read-modify-write: SET replaces the whole multi-string list, so fetch
    // the existing entries first and append ours if it is not present yet.
    DWORD requiredBytes = 0;
    DeviceIoControl(dev, hidHideCtlCode(kFuncGetWhitelist), nullptr, 0,
                    nullptr, 0, &requiredBytes, nullptr);
    if (requiredBytes < 2 * sizeof(wchar_t)) {
        CloseHandle(dev);
        return 5;   // never replace the list if the current entries cannot be read
    }

    QVarLengthArray<wchar_t, 512> buf(
        static_cast<int>((requiredBytes + sizeof(wchar_t) - 1) / sizeof(wchar_t)));
    DWORD bytes = 0;
    QStringList entries;
    if (!DeviceIoControl(dev, hidHideCtlCode(kFuncGetWhitelist), nullptr, 0,
                         buf.data(), static_cast<DWORD>(buf.size() * sizeof(wchar_t)),
                         &bytes, nullptr)
        || bytes < 2 * sizeof(wchar_t)) {
        CloseHandle(dev);
        return 5;
    }
    for (const wchar_t* p = buf.constData(); *p; p += wcslen(p) + 1)
        entries << QString::fromWCharArray(p);

    bool present = false;
    for (const QString& e : entries) {
        if (e.compare(mine, Qt::CaseInsensitive) == 0) {
            present = true;
            break;
        }
    }

    if (!present) {
        entries << mine;
        std::vector<wchar_t> out;
        for (const QString& e : entries) {
            const std::wstring w = e.toStdWString();
            out.insert(out.end(), w.c_str(), w.c_str() + w.size() + 1);   // includes NUL
        }
        out.push_back(L'\0');   // multi-sz double-NUL terminator
        if (!DeviceIoControl(dev, hidHideCtlCode(kFuncSetWhitelist),
                             out.data(), static_cast<DWORD>(out.size() * sizeof(wchar_t)),
                             nullptr, 0, &bytes, nullptr))
            result = 3;
    }

    CloseHandle(dev);
    return result;
}

void* HidCloakMonitor::launchElevatedWhitelistHelper()
{
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH))
        return nullptr;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize       = sizeof(sei);
    sei.fMask        = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb       = L"runas";
    sei.lpFile       = exePath;
    sei.lpParameters = L"--hidhide-allow-self";
    sei.nShow        = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        return nullptr;   // UAC declined or launch failure
    return sei.hProcess;
}
