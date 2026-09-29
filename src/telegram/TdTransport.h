#pragma once

#include <QJsonObject>
#include <QObject>

#include <functional>
#include <memory>

namespace telegram
{

// The seam between GameHQ's Telegram logic and TDLib. Production talks to the
// real tdjson.dll (TdJsonTransport); tests script a fake one, so the whole
// auth and send logic runs without a Telegram account or credentials.
//
// Everything crosses this seam as JSON objects in TDLib's own vocabulary
// ("@type": "setTdlibParameters", ...). `received` is always delivered on the
// owner's thread.
class TdTransport : public QObject
{
    Q_OBJECT
public:
    explicit TdTransport(QObject* parent = nullptr) : QObject(parent) {}
    ~TdTransport() override = default;

    // Starts the client; false if it cannot (runtime missing...). After true,
    // updates begin to arrive via received().
    virtual bool start() = 0;
    virtual void send(const QJsonObject& request) = 0;
    // Stops delivering and releases the client. Idempotent.
    virtual void stop() = 0;

signals:
    void received(const QJsonObject& object);
};

using TdTransportFactory = std::function<std::unique_ptr<TdTransport>()>;

} // namespace telegram
