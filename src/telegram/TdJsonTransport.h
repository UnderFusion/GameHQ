#pragma once

#include "telegram/TdRuntime.h"
#include "telegram/TdTransport.h"

#include <QThread>

#include <atomic>
#include <memory>

namespace telegram
{

// TdTransport over the pinned tdjson runtime. td_receive blocks, so it runs on
// its own thread and hands each parsed object to the owner's thread. Each
// transport owns one TDLib client; TDLib needs a fresh one after it closed.
class TdJsonTransport : public TdTransport
{
    Q_OBJECT
public:
    // `runtime` must outlive the transport and already be Ready.
    explicit TdJsonTransport(TdRuntime* runtime, QObject* parent = nullptr);
    // Drives the given entry points directly instead of a loaded runtime
    // (tests script td_receive this way).
    explicit TdJsonTransport(const TdJsonApi& api, QObject* parent = nullptr);
    ~TdJsonTransport() override;

    bool start() override;
    void send(const QJsonObject& request) override;
    void stop() override;

private:
    struct WorkerState;
    const TdJsonApi* readyApi() const;

    TdRuntime* m_runtime = nullptr;
    TdJsonApi m_injectedApi;
    int m_clientId = 0;
    // Shared with the receive thread, which may outlive a stop() that timed out.
    std::shared_ptr<WorkerState> m_state;
    QThread* m_thread = nullptr;
};

} // namespace telegram
