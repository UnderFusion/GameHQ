#include "gameinput/ProductionGameInputApi.h"

#include "gameinput/GameInputLabelMap.h"
#include "input/InputDiagnostics.h"

#include "GameInput.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QVarLengthArray>
#include <QDebug>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <windows.h>

namespace ModernInput {
namespace GI = GameInput::v3;

namespace {

QString bytesId(const void* value, size_t size)
{
    return QString::fromLatin1(QByteArray(static_cast<const char*>(value),
                                         qsizetype(size)).toHex());
}

GameInputDeviceDescriptor describeDevice(GI::IGameInputDevice* device)
{
    GameInputDeviceDescriptor result;
    if (!device)
        return result;

    const GI::GameInputDeviceInfo* info = nullptr;
    if (FAILED(device->GetDeviceInfo(&info)) || !info)
        return result;

    result.deviceId = bytesId(&info->deviceId, sizeof(info->deviceId));
    result.rootId = bytesId(&info->deviceRootId, sizeof(info->deviceRootId));
    result.containerId = bytesId(&info->containerId, sizeof(info->containerId));
    result.displayName = info->displayName ? QString::fromUtf8(info->displayName) : QString();
    result.vendorId = info->vendorId;
    result.productId = info->productId;
    result.supportedInput = quint32(info->supportedInput);
    result.supportedSystemButtons = quint32(info->supportedSystemButtons);
    if (info->controllerInfo) {
        result.buttons.reserve(int(info->controllerInfo->controllerButtonCount));
        for (quint32 i = 0; i < info->controllerInfo->controllerButtonCount; ++i) {
            const auto button = GameInputLabelMap::describe(
                info->controllerInfo->controllerButtonLabels[i]);
            if (GameInputLabelMap::isExtra(button))
                ++result.extraButtonCount;
            result.buttons.push_back(button);
        }
    }
    return result;
}

} // namespace

class ProductionGameInputApi::Impl
{
public:
    enum class CallbackKind { Device, Reading, System };

    struct CallbackContext
    {
        std::atomic_bool active{true};
        EventSink sink;
        CallbackKind kind = CallbackKind::Reading;
        Impl* owner = nullptr;
    };

    using InitializeFn = HRESULT (WINAPI*)(REFIID, void**);

    explicit Impl(QString overridePath) : runtimeOverride(std::move(overridePath)) {}

    ~Impl() { unload(); }

    bool initialize(QString& error)
    {
        if (input)
            return true;

        const QString appLocal = QCoreApplication::applicationDirPath()
            + QStringLiteral("/GameInputRedist.dll");
        QString chosen;
        if (!runtimeOverride.isEmpty())
            chosen = QFileInfo(runtimeOverride).absoluteFilePath();
        else if (QFileInfo::exists(appLocal))
            chosen = appLocal;

        if (!chosen.isEmpty()) {
            module = LoadLibraryExW(reinterpret_cast<LPCWSTR>(chosen.utf16()), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
                                        | LOAD_LIBRARY_SEARCH_SYSTEM32);
            loadedPath = chosen;
        } else {
            module = LoadLibraryExW(L"GameInputRedist.dll", nullptr,
                                    LOAD_LIBRARY_SEARCH_SYSTEM32);
            loadedPath = QStringLiteral("system GameInputRedist.dll");
        }
        if (!module) {
            error = QStringLiteral("GameInput runtime unavailable (Win32 %1).")
                        .arg(GetLastError());
            return false;
        }

        auto initialize = reinterpret_cast<InitializeFn>(
            GetProcAddress(module, "GameInputInitialize"));
        if (!initialize) {
            error = QStringLiteral("GameInput runtime has no GameInputInitialize export.");
            unload();
            return false;
        }

        const HRESULT hr = initialize(GI::IID_IGameInput,
                                      reinterpret_cast<void**>(&input));
        if (FAILED(hr) || !input) {
            error = QStringLiteral("GameInput v3 initialization failed (0x%1).")
                        .arg(quint32(hr), 8, 16, QLatin1Char('0'));
            unload();
            return false;
        }
        return true;
    }

    void applyFocusPolicy(GameInputFocusMode mode)
    {
        if (!input)
            return;
        // cpo-o06c: the focus-policy masks are derived in one place
        // (gameInputFocusMask), so the flags sent to Windows and the words the
        // export prints cannot drift apart. See GameInputFocusPolicy.h for the
        // three rules the masks encode (background system buttons in both modes,
        // exclusive input replacing background input, guide/share never made
        // exclusive).
        GameInputFocusPolicyFlags flags;
        flags.backgroundInput = int(GI::GameInputEnableBackgroundInput);
        flags.backgroundGuideButton = int(GI::GameInputEnableBackgroundGuideButton);
        flags.backgroundShareButton = int(GI::GameInputEnableBackgroundShareButton);
        flags.exclusiveForegroundInput = int(GI::GameInputExclusiveForegroundInput);
        input->SetFocusPolicy(
            GI::GameInputFocusPolicy(gameInputFocusMask(mode, flags)));
        // cpo-o06a/c: the export must state the policy actually in force, not the
        // one a reader assumes from the class name. Reported here, where the value
        // is decided, so the two cannot drift apart — and phrased so the exclusive
        // state cannot be mistaken for verified controller isolation.
        InputDiagnostics::instance().setGameInputFocusPolicy(
            gameInputFocusPolicyDescription(mode));
    }

    CallbackToken registerCallback(CallbackKind kind, EventSink sink)
    {
        if (!input || !sink)
            return 0;
        auto context = std::make_unique<CallbackContext>();
        context->sink = std::move(sink);
        context->kind = kind;
        context->owner = this;
        GI::GameInputCallbackToken token = 0;
        HRESULT hr = E_FAIL;
        if (kind == CallbackKind::Device) {
            const auto kinds = GI::GameInputKind(GI::GameInputKindController
                | GI::GameInputKindGamepad | GI::GameInputKindArcadeStick
                | GI::GameInputKindFlightStick | GI::GameInputKindRacingWheel);
            hr = input->RegisterDeviceCallback(nullptr, kinds,
                GI::GameInputDeviceAnyStatus, GI::GameInputBlockingEnumeration,
                context.get(), &Impl::deviceCallback, &token);
        } else if (kind == CallbackKind::Reading) {
            const auto kinds = GI::GameInputKind(GI::GameInputKindController
                | GI::GameInputKindGamepad | GI::GameInputKindArcadeStick
                | GI::GameInputKindFlightStick | GI::GameInputKindRacingWheel);
            hr = input->RegisterReadingCallback(nullptr, kinds, context.get(),
                                                &Impl::readingCallback, &token);
        } else {
            const auto buttons = GI::GameInputSystemButtons(
                GI::GameInputSystemButtonGuide | GI::GameInputSystemButtonShare);
            hr = input->RegisterSystemButtonCallback(nullptr, buttons, context.get(),
                                                      &Impl::systemButtonCallback,
                                                      &token);
        }
        if (FAILED(hr) || token == 0) {
            qWarning().noquote() << QStringLiteral("GameInput callback registration failed: kind=%1 hr=0x%2 token=%3")
                .arg(kind == CallbackKind::Device ? QStringLiteral("device")
                    : kind == CallbackKind::Reading ? QStringLiteral("reading") : QStringLiteral("system-button"))
                .arg(quint32(hr), 8, 16, QLatin1Char('0')).arg(quint64(token));
            return 0;
        }

        QMutexLocker lock(&mutex);
        callbacks.emplace(quint64(token), std::move(context));
        return quint64(token);
    }

    void stopCallback(CallbackToken token)
    {
        if (!input || token == 0)
            return;
        {
            QMutexLocker lock(&mutex);
            if (auto it = callbacks.find(token); it != callbacks.end())
                it->second->active.store(false, std::memory_order_release);
        }
        input->StopCallback(GI::GameInputCallbackToken(token));
    }

    bool unregisterCallback(CallbackToken token)
    {
        if (!input || token == 0)
            return false;
        const bool result = input->UnregisterCallback(GI::GameInputCallbackToken(token));
        QMutexLocker lock(&mutex);
        callbacks.erase(token);
        return result;
    }

    void releaseDevices()
    {
        // Device pointers are callback-local and released before returning;
        // this explicit phase remains in the lifecycle for future retained
        // device handles and is asserted by the fake implementation.
        QMutexLocker lock(&descriptorMutex);
        descriptorCache.clear();
    }

    // Reading callbacks fire at controller report rate (up to kHz). The full
    // descriptor (GetDeviceInfo, ID conversions, label list) is built only on
    // device lifecycle callbacks and served from this cache per reading.
    GameInputDeviceDescriptor cachedDescriptor(GI::IGameInputDevice* device,
                                               bool refresh)
    {
        if (!device)
            return {};
        if (!refresh) {
            QMutexLocker lock(&descriptorMutex);
            if (const auto it = descriptorCache.find(device);
                it != descriptorCache.end())
                return it->second;
        }
        GameInputDeviceDescriptor descriptor = describeDevice(device);
        QMutexLocker lock(&descriptorMutex);
        // GetDeviceInfo may fail after Windows has already detached the
        // device. A removal must retain the last good identity long enough to
        // evict the correct registry attachment; never poison that cache with
        // an empty refresh result.
        if (descriptor.deviceId.isEmpty()) {
            if (const auto it = descriptorCache.find(device);
                it != descriptorCache.end())
                return it->second;
            return descriptor;
        }
        descriptorCache[device] = descriptor;
        return descriptor;
    }

    void dropDescriptor(GI::IGameInputDevice* device)
    {
        QMutexLocker lock(&descriptorMutex);
        descriptorCache.erase(device);
    }

    void unload()
    {
        QVector<CallbackToken> tokens;
        {
            QMutexLocker lock(&mutex);
            tokens.reserve(callbacks.size());
            for (const auto &[token, context] : callbacks) {
                Q_UNUSED(context)
                tokens.push_back(token);
            }
        }
        for (CallbackToken token : tokens) {
            stopCallback(token);
            unregisterCallback(token);
        }
        releaseDevices();
        if (input) {
            input->Release();
            input = nullptr;
        }
        if (module) {
            FreeLibrary(module);
            module = nullptr;
        }
        loadedPath.clear();
    }

    static void CALLBACK deviceCallback(GI::GameInputCallbackToken, void* opaque,
        GI::IGameInputDevice* device, quint64 timestamp,
        GI::GameInputDeviceStatus currentStatus, GI::GameInputDeviceStatus previousStatus)
    {
        auto* context = static_cast<CallbackContext*>(opaque);
        if (!context || !context->active.load(std::memory_order_acquire))
            return;
        GameInputEvent event;
        event.timestamp = timestamp;
        // Lifecycle callbacks are the only place the descriptor is rebuilt;
        // readings and system buttons reuse the cached copy.
        event.device = context->owner->cachedDescriptor(device, /*refresh=*/true);
        event.deviceId = event.device.deviceId;
        const bool connected = (currentStatus & GI::GameInputDeviceConnected) != 0;
        const bool wasConnected = (previousStatus & GI::GameInputDeviceConnected) != 0;
        if (connected != wasConnected)
            event.kind = connected ? GameInputEventKind::DeviceAdded
                                   : GameInputEventKind::DeviceRemoved;
        else
            event.kind = GameInputEventKind::CapabilityChanged;
        if (event.kind == GameInputEventKind::DeviceRemoved)
            context->owner->dropDescriptor(device);
        context->sink(std::move(event));
    }

    static void CALLBACK readingCallback(GI::GameInputCallbackToken, void* opaque,
                                         GI::IGameInputReading* reading)
    {
        auto* context = static_cast<CallbackContext*>(opaque);
        if (!context || !reading
            || !context->active.load(std::memory_order_acquire))
            return;

        GameInputEvent event;
        event.kind = GameInputEventKind::Reading;
        event.timestamp = reading->GetTimestamp();
        GI::IGameInputDevice* device = nullptr;
        reading->GetDevice(&device);
        // Hot path: descriptor comes from the lifecycle cache (implicitly
        // shared copy) — no GetDeviceInfo, ID conversion or label rebuild
        // per reading.
        event.device = context->owner->cachedDescriptor(device, /*refresh=*/false);
        event.deviceId = event.device.deviceId;
        if (device)
            device->Release();

        GI::GameInputGamepadState gamepad{};
        if (reading->GetGamepadState(&gamepad))
            event.standardButtons = quint32(gamepad.buttons);

        const quint32 count = reading->GetControllerButtonCount();
        if (count > 0) {
            QVarLengthArray<bool, 64> states(static_cast<int>(count));
            const quint32 written = reading->GetControllerButtonState(count, states.data());
            event.buttonStates.reserve(int(written));
            for (quint32 i = 0; i < written; ++i)
                event.buttonStates.push_back(states[i] ? 1 : 0);
        }
        context->sink(std::move(event));
    }

    static void CALLBACK systemButtonCallback(GI::GameInputCallbackToken, void* opaque,
        GI::IGameInputDevice* device, quint64 timestamp,
        GI::GameInputSystemButtons currentButtons,
        GI::GameInputSystemButtons previousButtons)
    {
        auto* context = static_cast<CallbackContext*>(opaque);
        if (!context || !context->active.load(std::memory_order_acquire))
            return;
        const auto changed = GI::GameInputSystemButtons(currentButtons ^ previousButtons);
        const GameInputDeviceDescriptor descriptor =
            context->owner->cachedDescriptor(device, /*refresh=*/false);
        const auto publish = [&](GI::GameInputSystemButtons button, const QString& controlId) {
            if ((changed & button) == 0)
                return;
            GameInputEvent event;
            event.kind = (currentButtons & button)
                ? GameInputEventKind::SystemButtonPressed
                : GameInputEventKind::SystemButtonReleased;
            event.timestamp = timestamp;
            event.device = descriptor;
            event.deviceId = descriptor.deviceId;
            event.controlId = controlId;
            context->sink(std::move(event));
        };
        publish(GI::GameInputSystemButtonGuide, QStringLiteral("gamepad.guide"));
        publish(GI::GameInputSystemButtonShare, QStringLiteral("gamepad.capture"));
    }

    QString runtimeOverride;
    QString loadedPath;
    HMODULE module = nullptr;
    GI::IGameInput* input = nullptr;
    QMutex mutex;
    std::unordered_map<CallbackToken, std::unique_ptr<CallbackContext>> callbacks;
    QMutex descriptorMutex;
    std::unordered_map<GI::IGameInputDevice*, GameInputDeviceDescriptor> descriptorCache;
};

ProductionGameInputApi::ProductionGameInputApi(QString runtimeOverride)
    : m_impl(std::make_unique<Impl>(std::move(runtimeOverride)))
{
}

ProductionGameInputApi::~ProductionGameInputApi() = default;

bool ProductionGameInputApi::initialize(QString& error) { return m_impl->initialize(error); }
void ProductionGameInputApi::applyFocusPolicy(GameInputFocusMode mode) { m_impl->applyFocusPolicy(mode); }
IGameInputApi::CallbackToken ProductionGameInputApi::registerDeviceCallback(EventSink sink)
{ return m_impl->registerCallback(Impl::CallbackKind::Device, std::move(sink)); }
IGameInputApi::CallbackToken ProductionGameInputApi::registerReadingCallback(EventSink sink)
{ return m_impl->registerCallback(Impl::CallbackKind::Reading, std::move(sink)); }
IGameInputApi::CallbackToken ProductionGameInputApi::registerSystemButtonCallback(EventSink sink)
{ return m_impl->registerCallback(Impl::CallbackKind::System, std::move(sink)); }
void ProductionGameInputApi::stopCallback(CallbackToken token) { m_impl->stopCallback(token); }
bool ProductionGameInputApi::unregisterCallback(CallbackToken token)
{ return m_impl->unregisterCallback(token); }
void ProductionGameInputApi::releaseDevices() { m_impl->releaseDevices(); }
void ProductionGameInputApi::unload() { m_impl->unload(); }
bool ProductionGameInputApi::loaded() const { return m_impl->input != nullptr; }
QString ProductionGameInputApi::runtimeDescription() const { return m_impl->loadedPath; }

} // namespace ModernInput
