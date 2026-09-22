#pragma once

#include "gameinput/IGameInputApi.h"

#include <memory>

namespace ModernInput {

// Explicit LoadLibrary/GetProcAddress wrapper. Unlike the vendor sample loader,
// this object owns the module handle so ordered shutdown can release the COM
// interface and then unload the runtime deterministically.
class ProductionGameInputApi final : public IGameInputApi
{
public:
    explicit ProductionGameInputApi(QString runtimeOverride = {});
    ~ProductionGameInputApi() override;

    bool initialize(QString& error) override;
    void applyFocusPolicy(GameInputFocusMode mode) override;
    CallbackToken registerDeviceCallback(EventSink sink) override;
    CallbackToken registerReadingCallback(EventSink sink) override;
    CallbackToken registerSystemButtonCallback(EventSink sink) override;
    void stopCallback(CallbackToken token) override;
    bool unregisterCallback(CallbackToken token) override;
    void releaseDevices() override;
    void unload() override;
    bool loaded() const override;
    QString runtimeDescription() const override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace ModernInput

