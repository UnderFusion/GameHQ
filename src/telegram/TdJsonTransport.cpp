#include "telegram/TdJsonTransport.h"

#include <QDebug>
#include <QJsonDocument>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>

namespace telegram
{

// What the receive thread may touch. It never sees the transport itself
// except through `target`, read under `mutex`; stop() clears it under the
// same mutex, so once stop() returns the thread can no longer reach a
// transport that is about to be destroyed — however late td_receive returns.
struct TdJsonTransport::WorkerState
{
    std::atomic_bool running{ true };
    QMutex mutex;
    TdJsonTransport* target = nullptr;
};

namespace
{
// A receive call blocks for at most 0.5 s, so this is generous. Past it the
// thread is left to finish on its own rather than destroyed while running.
constexpr int kStopWaitMs = 3000;
}

TdJsonTransport::TdJsonTransport(TdRuntime* runtime, QObject* parent)
    : TdTransport(parent)
    , m_runtime(runtime)
{
}

TdJsonTransport::TdJsonTransport(const TdJsonApi& api, QObject* parent)
    : TdTransport(parent)
    , m_injectedApi(api)
{
}

TdJsonTransport::~TdJsonTransport()
{
    stop();
}

const TdJsonApi* TdJsonTransport::readyApi() const
{
    if (m_injectedApi.isValid())
        return &m_injectedApi;
    if (m_runtime && m_runtime->isReady())
        return &m_runtime->api();
    return nullptr;
}

bool TdJsonTransport::start()
{
    const TdJsonApi* api = readyApi();
    if (m_state || !api)
        return false;
    const TdJsonApi calls = *api;
    m_clientId = calls.createClientId();

    auto state = std::make_shared<WorkerState>();
    state->target = this;
    m_state = state;
    m_thread = QThread::create([state, calls] {
        while (state->running.load()) {
            const char* raw = calls.receive(0.5);
            if (!raw)
                continue;
            const QJsonObject object = QJsonDocument::fromJson(QByteArray(raw)).object();
            if (object.isEmpty())
                continue;
            QMutexLocker lock(&state->mutex);
            TdJsonTransport* target = state->target;
            if (!target)
                break;   // stopped while this receive was in flight
            // The transport is the context object: if it is destroyed before
            // the event is delivered, Qt drops the event with it.
            QMetaObject::invokeMethod(target, [target, object] {
                emit target->received(object);
            }, Qt::QueuedConnection);
        }
    });
    m_thread->start();
    // The first send makes TDLib start emitting updates for this client.
    calls.send(m_clientId, "{\"@type\":\"getOption\",\"name\":\"version\"}");
    return true;
}

void TdJsonTransport::send(const QJsonObject& request)
{
    const TdJsonApi* api = readyApi();
    if (!m_state || !api)
        return;
    const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    api->send(m_clientId, bytes.constData());
}

void TdJsonTransport::stop()
{
    if (!m_state)
        return;
    m_state->running = false;
    {
        QMutexLocker lock(&m_state->mutex);
        m_state->target = nullptr;
    }
    m_state.reset();   // the thread keeps its own reference while it runs

    QThread* thread = m_thread;
    m_thread = nullptr;
    if (!thread)
        return;
    if (thread->wait(kStopWaitMs)) {
        delete thread;
        return;
    }
    // Deleting a running QThread aborts the process. Let it end by itself
    // and clean up then; it no longer references this transport.
    qWarning() << "Telegram: TDLib receive did not return within" << kStopWaitMs
               << "ms; its worker will finish on its own";
    if (thread->isFinished())
        delete thread;
    else
        QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
}

} // namespace telegram
