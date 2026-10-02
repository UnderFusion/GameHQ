#include "input/RawInputApi.h"

#include "input/HidCloakMonitor.h"

#include <QByteArray>
#include <QHash>
#include <QVarLengthArray>
#include <QVector>

#include <windows.h>
// MinGW's HID headers ship without extern "C" guards; without this wrap the
// HidP_* imports mangle as C++ and never resolve against libhid.
extern "C" {
#include <hidsdi.h>
#include <hidpi.h>
}

#include <algorithm>

RawInputApi::~RawInputApi() = default;

namespace
{
// The real Win32 Raw Input surface. Nothing here decides anything about pads;
// it only translates Win32 into the platform-clean structs in RawInputApi.h.
class Win32RawInputApi final : public RawInputApi
{
public:
    bool readHeader(void* rawInputHandle, Header& out) override
    {
        // RID_HEADER copies a fixed 24-byte struct into our stack frame: no
        // size probe, no buffer, no allocation — the whole point of reading
        // the header before deciding whether the payload is worth fetching.
        RAWINPUTHEADER header{};
        UINT size = sizeof(header);
        if (GetRawInputData(static_cast<HRAWINPUT>(rawInputHandle), RID_HEADER,
                            &header, &size, sizeof(RAWINPUTHEADER))
            == static_cast<UINT>(-1))
            return false;

        out.device = header.hDevice;
        out.type = header.dwType == RIM_TYPEHID ? DeviceType::Hid : DeviceType::Other;
        return true;
    }

    bool readPayload(void* rawInputHandle, Payload& out) override
    {
        auto handle = static_cast<HRAWINPUT>(rawInputHandle);
        UINT size = 0;
        if (GetRawInputData(handle, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0
            || size == 0)
            return false;

        // One buffer for the life of the backend: it grows to the largest
        // report seen and is then reused, so a pad streaming at 250 Hz (or
        // 8000) does not allocate per event. QByteArray's storage comes from
        // malloc, so it is aligned well enough for RAWINPUT.
        if (m_buffer.size() < static_cast<qsizetype>(size))
            m_buffer.resize(static_cast<qsizetype>(size));
        if (GetRawInputData(handle, RID_INPUT, m_buffer.data(), &size, sizeof(RAWINPUTHEADER))
            != size)
            return false;

        auto* ri = reinterpret_cast<RAWINPUT*>(m_buffer.data());
        if (ri->header.dwType != RIM_TYPEHID)
            return false;

        out.reports     = ri->data.hid.bRawData;
        out.reportCount = static_cast<int>(ri->data.hid.dwCount);
        out.reportSize  = static_cast<int>(ri->data.hid.dwSizeHid);
        return true;
    }

    DeviceInfo describeDevice(void* deviceHandle) override
    {
        DeviceInfo out;
        RID_DEVICE_INFO info{};
        info.cbSize = sizeof(info);
        UINT infoSize = sizeof(info);
        if (GetRawInputDeviceInfoW(deviceHandle, RIDI_DEVICEINFO, &info, &infoSize)
            == static_cast<UINT>(-1))
            return out;   // queried stays false: transient, not a verdict

        out.queried = true;
        out.isHid   = info.dwType == RIM_TYPEHID;
        if (out.isHid) {
            out.vendorId  = info.hid.dwVendorId;
            out.productId = info.hid.dwProductId;
            out.usagePage = info.hid.usUsagePage;
            out.usage     = info.hid.usUsage;
        }
        return out;
    }

    DevicePath devicePath(void* deviceHandle) override
    {
        DevicePath out;
        UINT chars = 0;
        if (GetRawInputDeviceInfoW(deviceHandle, RIDI_DEVICENAME, nullptr, &chars) != 0
            || chars == 0)
            return out;

        QVarLengthArray<wchar_t, 256> buf(static_cast<int>(chars) + 1);
        if (GetRawInputDeviceInfoW(deviceHandle, RIDI_DEVICENAME, buf.data(), &chars)
            == static_cast<UINT>(-1))
            return out;

        buf[buf.size() - 1] = 0;
        out.queried = true;
        out.value   = QString::fromWCharArray(buf.data());
        return out;
    }

    QList<EnumeratedDevice> enumerateDevices() override
    {
        QList<EnumeratedDevice> devices;
        UINT count = 0;
        if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0 || count == 0)
            return devices;

        QByteArray bytes(static_cast<qsizetype>(count * sizeof(RAWINPUTDEVICELIST)),
                         Qt::Uninitialized);
        auto* list = reinterpret_cast<RAWINPUTDEVICELIST*>(bytes.data());
        if (GetRawInputDeviceList(list, &count, sizeof(RAWINPUTDEVICELIST))
            == static_cast<UINT>(-1))
            return devices;

        devices.reserve(static_cast<qsizetype>(count));
        for (UINT i = 0; i < count; ++i)
            devices.append({ list[i].hDevice,
                             list[i].dwType == RIM_TYPEHID ? DeviceType::Hid : DeviceType::Other });
        return devices;
    }

    CloakScan scanHiddenPads(const QSet<QString>& visibleRawPathsLower) override
    {
        const HidCloakMonitor::ScanResult result = HidCloakMonitor::scan(visibleRawPathsLower);
        return CloakScan{ result.hiddenPads, result.hidHidePresent };
    }

    QList<HiddenInterface> hiddenPadInterfaces(const QSet<QString>& visibleRawPathsLower) override
    {
        QList<HiddenInterface> out;
        for (const auto& i : HidCloakMonitor::hiddenPadInterfaces(visibleRawPathsLower))
            out.append({ i.path, i.vendorId, i.productId });
        return out;
    }

    bool visitButtonUsageReports(void* deviceHandle, const Payload& payload, void* context,
                                 const ButtonUsageVisitor& visitor) override
    {
        if (!payload.reports || payload.reportSize <= 0 || payload.reportCount <= 0
            || !visitor)
            return false;

        auto cacheIt = m_parsers.find(deviceHandle);
        if (cacheIt == m_parsers.end()) {
            ParserCache cache;
            if (!initializeParser(deviceHandle, cache))
                return false;
            cacheIt = m_parsers.insert(deviceHandle, std::move(cache));
        }
        ParserCache& cache = cacheIt.value();
        auto* preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(cache.preparsed.data());
        if (cache.reportCopy.size() < payload.reportSize)
            cache.reportCopy.resize(payload.reportSize);

        for (int reportIndex = 0; reportIndex < payload.reportCount; ++reportIndex) {
            const unsigned char* report = payload.reports
                + static_cast<size_t>(reportIndex) * payload.reportSize;
            memcpy(cache.reportCopy.data(), report, static_cast<size_t>(payload.reportSize));
            cache.pressedUsages.clear();

            for (PagePlan& page : cache.pages) {
                ULONG usageCount = static_cast<ULONG>(page.usages.size());
                if (HidP_GetUsages(HidP_Input, page.usagePage, 0, page.usages.data(),
                                   &usageCount, preparsed, cache.reportCopy.data(),
                                   static_cast<ULONG>(payload.reportSize))
                    != HIDP_STATUS_SUCCESS)
                    continue;
                for (ULONG usageIndex = 0; usageIndex < usageCount; ++usageIndex) {
                    cache.pressedUsages.push_back(
                        (quint32(page.usagePage) << 16) | page.usages.at(usageIndex));
                }
            }
            visitor(context, cache.pressedUsages);
        }
        return true;
    }

    void forgetDevice(void* deviceHandle) override
    {
        m_parsers.remove(deviceHandle);
    }

private:
    struct PagePlan {
        USAGE usagePage = 0;
        QVector<USAGE> usages;
    };

    struct ParserCache {
        QByteArray preparsed;
        HIDP_CAPS caps{};
        QVector<HIDP_BUTTON_CAPS> buttonCaps;
        QVector<PagePlan> pages;
        QByteArray reportCopy;
        QList<quint32> pressedUsages;
    };

    static bool initializeParser(void* deviceHandle, ParserCache& cache)
    {
        UINT bytes = 0;
        if (GetRawInputDeviceInfoW(deviceHandle, RIDI_PREPARSEDDATA, nullptr, &bytes) != 0
            || bytes == 0)
            return false;
        cache.preparsed.resize(static_cast<qsizetype>(bytes));
        if (GetRawInputDeviceInfoW(deviceHandle, RIDI_PREPARSEDDATA,
                                   cache.preparsed.data(), &bytes) == static_cast<UINT>(-1))
            return false;
        auto* preparsed = reinterpret_cast<PHIDP_PREPARSED_DATA>(cache.preparsed.data());

        if (HidP_GetCaps(preparsed, &cache.caps) != HIDP_STATUS_SUCCESS
            || cache.caps.NumberInputButtonCaps == 0)
            return false;

        cache.buttonCaps.resize(cache.caps.NumberInputButtonCaps);
        USHORT capsCount = cache.caps.NumberInputButtonCaps;
        if (HidP_GetButtonCaps(HidP_Input, cache.buttonCaps.data(), &capsCount, preparsed)
            != HIDP_STATUS_SUCCESS)
            return false;
        cache.buttonCaps.resize(capsCount);

        qsizetype maximumPressedUsages = 0;
        for (USHORT i = 0; i < capsCount; ++i) {
            const USAGE usagePage = cache.buttonCaps.at(i).UsagePage;
            if (std::any_of(cache.pages.cbegin(), cache.pages.cend(),
                            [usagePage](const PagePlan& page) {
                                return page.usagePage == usagePage;
                            }))
                continue;
            const ULONG usageCount = HidP_MaxUsageListLength(
                HidP_Input, usagePage, preparsed);
            if (usageCount == 0)
                continue;
            PagePlan page;
            page.usagePage = usagePage;
            page.usages.resize(static_cast<qsizetype>(usageCount));
            maximumPressedUsages += static_cast<qsizetype>(usageCount);
            cache.pages.push_back(std::move(page));
        }
        if (cache.pages.isEmpty())
            return false;
        cache.pressedUsages.reserve(maximumPressedUsages);
        return true;
    }

    QByteArray m_buffer;
    QHash<void*, ParserCache> m_parsers;
};
} // namespace

RawInputApi* RawInputApi::createSystem()
{
    return new Win32RawInputApi;
}
