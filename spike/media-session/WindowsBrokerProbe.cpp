#include <windows.h>
#include <appmodel.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <winstring.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

namespace MediaSessionBrokerAbi {

// External linkage lets MinGW emit virtual calls to the OS implementations.
// API contract v7. The MinGW kit lacks Windows.Media.Control declarations.
// Sizes and slot order match the Windows metadata for the statics interface
// and IAsyncOperation<GlobalSystemMediaTransportControlsSessionManager>.
struct ManagerOperation : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE put_Completed(IUnknown* handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Completed(IUnknown** handler) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetResults(IInspectable** manager) = 0;
};
struct ManagerStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE RequestAsync(ManagerOperation** operation) = 0;
};

} // namespace MediaSessionBrokerAbi

namespace {

using namespace std::chrono_literals;
using namespace MediaSessionBrokerAbi;

constexpr wchar_t className[] =
    L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
constexpr GUID managerStaticsId{
    0x2050c4ee, 0x11a0, 0x57de, {0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45}
};

struct Release {
    void operator()(IUnknown* object) const { object->Release(); }
};

HRESULT requestManager()
{
    HSTRING_HEADER header{};
    HSTRING name{};
    HRESULT hr = WindowsCreateStringReference(className, ARRAYSIZE(className) - 1,
                                              &header, &name);
    if (FAILED(hr))
        return hr;

    void* factoryObject = nullptr;
    hr = RoGetActivationFactory(name, managerStaticsId, &factoryObject);
    if (FAILED(hr))
        return hr;
    const std::unique_ptr<ManagerStatics, Release> factory(
        static_cast<ManagerStatics*>(factoryObject));

    ManagerOperation* operationObject = nullptr;
    hr = factory->RequestAsync(&operationObject);
    if (FAILED(hr))
        return hr;
    const std::unique_ptr<ManagerOperation, Release> operation(operationObject);

    void* infoObject = nullptr;
    hr = operation->QueryInterface(__uuidof(IAsyncInfo), &infoObject);
    if (FAILED(hr))
        return hr;
    const std::unique_ptr<IAsyncInfo, Release> info(static_cast<IAsyncInfo*>(infoObject));

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    AsyncStatus status = Started;
    do {
        hr = info->get_Status(&status);
        if (FAILED(hr))
            return hr;
        if (status != Started)
            break;
        if (std::chrono::steady_clock::now() >= deadline) {
            info->Cancel();
            return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
        }
        std::this_thread::sleep_for(10ms);
    } while (true);

    if (status == Completed) {
        IInspectable* managerObject = nullptr;
        hr = operation->GetResults(&managerObject);
        const std::unique_ptr<IInspectable, Release> manager(managerObject);
        if (SUCCEEDED(hr) && !manager)
            hr = E_UNEXPECTED;
    } else {
        HRESULT operationError = E_ABORT;
        hr = info->get_ErrorCode(&operationError);
        if (SUCCEEDED(hr))
            hr = FAILED(operationError) ? operationError : E_ABORT;
    }
    info->Close();
    return hr;
}

} // namespace

int main()
{
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized)) {
        std::fprintf(stderr, "RoInitialize failed (0x%08lx)\n",
                     static_cast<unsigned long>(initialized));
        return 1;
    }
    const HRESULT result = requestManager();
    RoUninitialize();
    if (FAILED(result)) {
        std::fprintf(stderr, "Media session manager request failed (0x%08lx)\n",
                     static_cast<unsigned long>(result));
        return 1;
    }
    UINT32 length = 0;
    const auto identity = GetCurrentPackageFullName(&length, nullptr);
    std::printf("Manager retrieved. Unpackaged: %s. Session queries: 0. Commands: 0.\n",
                identity == APPMODEL_ERROR_NO_PACKAGE ? "yes" : "no");
}
