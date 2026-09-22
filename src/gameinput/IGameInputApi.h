#pragma once

#include "gameinput/GameInputEvent.h"
#include "gameinput/GameInputFocusPolicy.h"

#include <QString>

#include <functional>

namespace ModernInput {

class IGameInputApi
{
public:
    using CallbackToken = quint64;
    using EventSink = std::function<void(GameInputEvent)>;

    virtual ~IGameInputApi() = default;

    virtual bool initialize(QString& error) = 0;
    // cpo-o06c: GameInput's focus policy is one process-wide setting, and
    // GameInputFocusController is its single owner — this method is the only
    // entry point, so the mask and the transition logic can never be duplicated
    // across callers. Called after initialize() and before any callback
    // registration, because the runtime only honours the policy for callbacks
    // registered afterwards.
    //
    // Background (the default, and what every normal session runs under) must
    // request delivery for standard input AND the system Guide/Share buttons:
    // EnableBackgroundInput alone does not cover the system buttons.
    //
    // ExclusiveForeground is requested only while an interactive overlay holds
    // the verified foreground, and is released on every exit path (see
    // GameInputFocusController). It is best-effort and GameInput-scoped: it binds
    // other GameInput clients, does nothing to games reading XInput/DirectInput/
    // Raw Input, and nothing in-process can verify its effect.
    virtual void applyFocusPolicy(GameInputFocusMode mode) = 0;
    virtual CallbackToken registerDeviceCallback(EventSink sink) = 0;
    virtual CallbackToken registerReadingCallback(EventSink sink) = 0;
    virtual CallbackToken registerSystemButtonCallback(EventSink sink) = 0;
    virtual void stopCallback(CallbackToken token) = 0;
    virtual bool unregisterCallback(CallbackToken token) = 0;
    virtual void releaseDevices() = 0;
    virtual void unload() = 0;
    virtual bool loaded() const = 0;
    virtual QString runtimeDescription() const = 0;
};

// Move-only lifetime guard. reset() always performs StopCallback before
// UnregisterCallback, matching GameInput's callback shutdown contract.
class GameInputCallbackRegistration
{
public:
    GameInputCallbackRegistration() = default;
    GameInputCallbackRegistration(IGameInputApi* api, IGameInputApi::CallbackToken token)
        : m_api(api), m_token(token) {}
    ~GameInputCallbackRegistration() { reset(); }

    GameInputCallbackRegistration(const GameInputCallbackRegistration&) = delete;
    GameInputCallbackRegistration& operator=(const GameInputCallbackRegistration&) = delete;

    GameInputCallbackRegistration(GameInputCallbackRegistration&& other) noexcept
        : m_api(other.m_api), m_token(other.m_token)
    {
        other.m_api = nullptr;
        other.m_token = 0;
    }

    GameInputCallbackRegistration& operator=(GameInputCallbackRegistration&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_api = other.m_api;
            m_token = other.m_token;
            other.m_api = nullptr;
            other.m_token = 0;
        }
        return *this;
    }

    bool valid() const { return m_api != nullptr && m_token != 0; }
    IGameInputApi::CallbackToken token() const { return m_token; }

    void reset()
    {
        if (!valid())
            return;
        IGameInputApi* api = m_api;
        const auto token = m_token;
        m_api = nullptr;
        m_token = 0;
        api->stopCallback(token);
        api->unregisterCallback(token);
    }

private:
    IGameInputApi* m_api = nullptr;
    IGameInputApi::CallbackToken m_token = 0;
};

} // namespace ModernInput

