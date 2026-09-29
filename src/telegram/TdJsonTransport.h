#pragma once

#include "telegram/TdRuntime.h"
#include "telegram/TdTransport.h"

#include <QThread>

#include <atomic>

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
    ~TdJsonTransport() override;

    bool start() override;
    void send(const QJsonObject& request) override;
    void stop() override;

private:
    TdRuntime* m_runtime;
    int m_clientId = 0;
    std::atomic_bool m_running{ false };
    QThread* m_thread = nullptr;
};

} // namespace telegram
