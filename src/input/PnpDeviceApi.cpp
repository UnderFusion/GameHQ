#include "input/PnpDeviceApi.h"

#include <QByteArray>
#include <QStringList>

#include <vector>

#include <windows.h>

#include <cfgmgr32.h>

// The same convention the Windows SDK headers use for plain GUIDs (initguid.h):
// <devpkey.h> only *declares* every DEVPKEY while this macro is undefined, and
// this is the one translation unit that defines the two keys it reads.
#define INITGUID
#include <devpkey.h>

// MinGW's <cfgmgr32.h> declares every neighbouring CM_* property call but not
// this one, while its import library (libcfgmgr32.a) exports the symbol.
// The prototype below is the exact Windows SDK signature — no substitute API
// and no wrapper around a different property store. Declared `dllimport`
// explicitly because cfgmgr32 is already linked into the app (and into
// tst_pnpdeviceapi), so the linker resolves it from the real DLL.
extern "C" __declspec(dllimport) DWORD WINAPI CM_Get_Device_Interface_PropertyW(
    DEVINSTID_W pszDeviceInterface, const DEVPROPKEY* PropertyKey,
    DEVPROPTYPE* PropertyType, PBYTE PropertyBuffer,
    PULONG PropertyBufferSize, ULONG ulFlags);

namespace
{
constexpr ULONG kMaxPropertyBytes = 4096;

bool isHexDigit(QChar c)
{
    return (c >= QLatin1Char('0') && c <= QLatin1Char('9'))
        || (c >= QLatin1Char('a') && c <= QLatin1Char('f'));
}

// One property read with the documented two-call sizing dance. Any answer other
// than a complete, correctly typed value is a refusal — the caller must not be
// able to distinguish "device is gone" from "property says nothing" and start
// guessing.
bool queryInterfaceProperty(const QString& interfacePath, const DEVPROPKEY& key,
                            DEVPROPTYPE expectedType, std::vector<BYTE>* out)
{
    const std::wstring path = interfacePath.toStdWString();
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET result = CM_Get_Device_Interface_PropertyW(
        const_cast<wchar_t*>(path.c_str()), &key, &type, nullptr, &size, 0);
    if ((result != CR_SUCCESS && result != CR_BUFFER_SMALL) || type != expectedType
        || size == 0 || size > kMaxPropertyBytes)
        return false;
    out->assign(size, 0);
    ULONG produced = size;
    result = CM_Get_Device_Interface_PropertyW(const_cast<wchar_t*>(path.c_str()), &key,
                                               &type, out->data(), &produced, 0);
    if (result != CR_SUCCESS || type != expectedType || produced == 0
        || produced > out->size())
        return false;
    out->resize(produced);
    return true;
}

bool queryDevNodeProperty(DEVINST devInst, const DEVPROPKEY& key,
                          DEVPROPTYPE expectedType, std::vector<BYTE>* out)
{
    DEVPROPTYPE type = 0;
    ULONG size = 0;
    CONFIGRET result = CM_Get_DevNode_PropertyW(devInst, &key, &type, nullptr, &size, 0);
    if ((result != CR_SUCCESS && result != CR_BUFFER_SMALL) || type != expectedType
        || size == 0 || size > kMaxPropertyBytes)
        return false;
    out->assign(size, 0);
    ULONG produced = size;
    result = CM_Get_DevNode_PropertyW(devInst, &key, &type, out->data(), &produced, 0);
    if (result != CR_SUCCESS || type != expectedType || produced == 0
        || produced > out->size())
        return false;
    out->resize(produced);
    return true;
}

// A GUID's 16 property bytes, formatted the way the rest of Windows prints a
// GUID: first three fields little-endian, the tail in order.
QString guidText(const BYTE* bytes)
{
    const quint32 data1 = quint32(bytes[0]) | (quint32(bytes[1]) << 8)
        | (quint32(bytes[2]) << 16) | (quint32(bytes[3]) << 24);
    const quint32 data2 = quint32(bytes[4]) | (quint32(bytes[5]) << 8);
    const quint32 data3 = quint32(bytes[6]) | (quint32(bytes[7]) << 8);
    return QString::asprintf("{%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", data1,
                             data2, data3, bytes[8], bytes[9], bytes[10], bytes[11],
                             bytes[12], bytes[13], bytes[14], bytes[15]);
}

class SystemPnpDeviceApi : public PnpDeviceApi
{
public:
    InstanceId deviceInstanceIdForInterfacePath(const QString& interfacePath) override;
    Container containerIdForDeviceInstance(const QString& deviceInstanceId) override;
};
} // namespace

PnpDeviceApi::~PnpDeviceApi() = default;

PnpDeviceApi::Container PnpDeviceApi::containerIdForInterfacePath(const QString& interfacePath)
{
    const InstanceId instance = deviceInstanceIdForInterfacePath(interfacePath);
    if (!instance.queried)
        return {};
    const QString canonicalInstance = normalizeDeviceInstanceId(instance.value);
    if (canonicalInstance.isEmpty())
        return {};
    Container answer = containerIdForDeviceInstance(canonicalInstance);
    if (!answer.queried)
        return {};
    // The outward contract is canonical text, whatever an implementation
    // printed. A container that cannot be canonicalised is not evidence, so it
    // leaves this call as the same honest refusal the OS would have given.
    const QString canonicalContainer = normalizeContainerId(answer.value);
    if (canonicalContainer.isEmpty())
        return {};
    answer.value = canonicalContainer;
    return answer;
}

QString PnpDeviceApi::normalizeInterfacePath(const QString& raw)
{
    // Comparison key only. Raw Input hands this out in one fixed spelling, but
    // a provider that lower-cases or pads it must still compare equal.
    return raw.trimmed().toLower();
}

QString PnpDeviceApi::normalizeDeviceInstanceId(const QString& raw)
{
    // Device instance IDs are case-insensitive by definition; PnP prints them
    // upper case, so that is the canonical form.
    return raw.trimmed().toUpper();
}

QString PnpDeviceApi::normalizeContainerId(const QString& raw)
{
    QString body = raw.trimmed().toLower();
    if (body.startsWith(QLatin1Char('{')) && body.endsWith(QLatin1Char('}')))
        body = body.mid(1, body.size() - 2);
    const QStringList parts = body.split(QLatin1Char('-'));
    if (parts.size() != 5)
        return {};
    static const int kLengths[5] = { 8, 4, 4, 4, 12 };
    for (int index = 0; index < 5; ++index) {
        if (parts.at(index).size() != kLengths[index])
            return {};
        for (const QChar c : parts.at(index)) {
            if (!isHexDigit(c))
                return {};
        }
    }
    return QLatin1Char('{') + body + QLatin1Char('}');
}

QString PnpDeviceApi::containerFromRawBytesHex(const QString& hex)
{
    const QString trimmed = hex.trimmed().toLower();
    if (trimmed.size() != 32)
        return {};
    for (const QChar c : trimmed) {
        if (!isHexDigit(c))
            return {};
    }
    const QByteArray bytes = QByteArray::fromHex(trimmed.toLatin1());
    if (bytes.size() != 16)
        return {};
    return guidText(reinterpret_cast<const BYTE*>(bytes.constData()));
}

PnpDeviceApi::InstanceId
SystemPnpDeviceApi::deviceInstanceIdForInterfacePath(const QString& interfacePath)
{
    InstanceId answer;
    const QString path = interfacePath.trimmed();
    if (path.isEmpty())
        return answer;
    std::vector<BYTE> buffer;
    if (!queryInterfaceProperty(path, DEVPKEY_Device_InstanceId, DEVPROP_TYPE_STRING,
                                &buffer))
        return answer;
    const QString instance = normalizeDeviceInstanceId(
        QString::fromWCharArray(reinterpret_cast<const wchar_t*>(buffer.data())));
    if (instance.isEmpty())
        return answer;
    answer.queried = true;
    answer.value = instance;
    return answer;
}

PnpDeviceApi::Container
SystemPnpDeviceApi::containerIdForDeviceInstance(const QString& deviceInstanceId)
{
    Container answer;
    const QString instance = normalizeDeviceInstanceId(deviceInstanceId);
    if (instance.isEmpty())
        return answer;
    // CM_LOCATE_DEVNODE_NORMAL refuses phantom (absent or cloaked) nodes, which
    // is exactly the fail-closed behaviour this seam promises: a device that is
    // not present right now has no container to report.
    DEVINST devInst = 0;
    const std::wstring wide = instance.toStdWString();
    if (CM_Locate_DevNodeW(&devInst, const_cast<wchar_t*>(wide.c_str()),
                           CM_LOCATE_DEVNODE_NORMAL)
        != CR_SUCCESS)
        return answer;
    std::vector<BYTE> buffer;
    if (!queryDevNodeProperty(devInst, DEVPKEY_Device_ContainerId, DEVPROP_TYPE_GUID,
                              &buffer)
        || buffer.size() != sizeof(GUID))
        return answer;
    answer.value = guidText(buffer.data());
    answer.queried = true;
    return answer;
}

PnpDeviceApi* PnpDeviceApi::createSystem()
{
    return new SystemPnpDeviceApi();
}
