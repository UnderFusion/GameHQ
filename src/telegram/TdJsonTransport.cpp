#include "telegram/TdJsonTransport.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <QPointer>

namespace telegram
{

TdJsonTransport::TdJsonTransport(TdRuntime* runtime, QObject* parent)
    : TdTransport(parent)
    , m_runtime(runtime)
{
}

TdJsonTransport::~TdJsonTransport()
{
    stop();
}

bool TdJsonTransport::start()
{
    if (m_running || !m_runtime || !m_runtime->isReady())
        return false;
    const TdJsonApi api = m_runtime->api();
    m_clientId = api.createClientId();
    m_running = true;

    QPointer<TdJsonTransport> self(this);
    m_thread = QThread::create([this, self, api] {
        while (m_running) {
            const char* raw = api.receive(0.5);
            if (!raw)
                continue;
            const QJsonObject object = QJsonDocument::fromJson(QByteArray(raw)).object();
            if (object.isEmpty())
                continue;
            QMetaObject::invokeMethod(this, [self, object] {
                if (self)
                    emit self->received(object);
            }, Qt::QueuedConnection);
        }
    });
    m_thread->start();
    // The first send makes TDLib start emitting updates for this client.
    api.send(m_clientId, "{\"@type\":\"getOption\",\"name\":\"version\"}");
    return true;
}

void TdJsonTransport::send(const QJsonObject& request)
{
    if (!m_running || !m_runtime)
        return;
    const QByteArray bytes = QJsonDocument(request).toJson(QJsonDocument::Compact);
    m_runtime->api().send(m_clientId, bytes.constData());
}

void TdJsonTransport::stop()
{
    if (!m_running.exchange(false))
        return;
    if (m_thread) {
        m_thread->wait(3000);
        delete m_thread;
        m_thread = nullptr;
    }
}

} // namespace telegram
