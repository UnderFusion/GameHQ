#include "gameinput/FakeGameInputApi.h"

#include <QMutexLocker>

#include <thread>

namespace ModernInput {

bool FakeGameInputApi::initialize(QString& error)
{
    QMutexLocker lock(&m_mutex);
    m_callLog.push_back(QStringLiteral("initialize"));
    if (!m_initializeError.isEmpty()) {
        error = m_initializeError;
        return false;
    }
    m_loaded.store(true);
    return true;
}

void FakeGameInputApi::applyFocusPolicy(GameInputFocusMode mode)
{
    QMutexLocker lock(&m_mutex);
    // The log entry is the test-visible contract: which policy the runtime was
    // asked for, in order, across a whole session (cpo-o06c).
    m_callLog.push_back(QStringLiteral("focus-policy:%1").arg(gameInputFocusModeName(mode)));
}

IGameInputApi::CallbackToken FakeGameInputApi::registerDeviceCallback(EventSink sink)
{ return add(Kind::Device, std::move(sink)); }
IGameInputApi::CallbackToken FakeGameInputApi::registerReadingCallback(EventSink sink)
{ return add(Kind::Reading, std::move(sink)); }
IGameInputApi::CallbackToken FakeGameInputApi::registerSystemButtonCallback(EventSink sink)
{ return add(Kind::System, std::move(sink)); }

void FakeGameInputApi::stopCallback(CallbackToken token)
{
    QMutexLocker lock(&m_mutex);
    m_callLog.push_back(QStringLiteral("stop:%1").arg(token));
    if (auto it = m_entries.find(token); it != m_entries.end())
        it->stopped = true;
}

bool FakeGameInputApi::unregisterCallback(CallbackToken token)
{
    QMutexLocker lock(&m_mutex);
    m_callLog.push_back(QStringLiteral("unregister:%1").arg(token));
    auto it = m_entries.find(token);
    if (it == m_entries.end())
        return false;
    m_retired.push_back(it->sink);
    m_entries.erase(it);
    return true;
}

void FakeGameInputApi::releaseDevices()
{
    QMutexLocker lock(&m_mutex);
    m_callLog.push_back(QStringLiteral("release-devices"));
}

void FakeGameInputApi::unload()
{
    QMutexLocker lock(&m_mutex);
    m_callLog.push_back(QStringLiteral("unload"));
    m_loaded.store(false);
}

void FakeGameInputApi::setInitializeFailure(QString error)
{
    QMutexLocker lock(&m_mutex);
    m_initializeError = std::move(error);
}

void FakeGameInputApi::emitDevice(GameInputEvent event, bool asynchronous)
{ emitKind(Kind::Device, std::move(event), asynchronous); }
void FakeGameInputApi::emitReading(GameInputEvent event, bool asynchronous)
{ emitKind(Kind::Reading, std::move(event), asynchronous); }
void FakeGameInputApi::emitSystem(GameInputEvent event, bool asynchronous)
{ emitKind(Kind::System, std::move(event), asynchronous); }

void FakeGameInputApi::emitRetired(GameInputEvent event)
{
    QVector<EventSink> sinks;
    {
        QMutexLocker lock(&m_mutex);
        sinks = m_retired;
    }
    for (const EventSink& sink : sinks)
        sink(event);
}

QStringList FakeGameInputApi::callLog() const
{
    QMutexLocker lock(&m_mutex);
    return m_callLog;
}

IGameInputApi::CallbackToken FakeGameInputApi::add(Kind kind, EventSink sink)
{
    QMutexLocker lock(&m_mutex);
    const CallbackToken token = m_nextToken++;
    m_entries.insert(token, Entry{kind, std::move(sink), false});
    m_callLog.push_back(QStringLiteral("register:%1").arg(token));
    return token;
}

void FakeGameInputApi::emitKind(Kind kind, GameInputEvent event, bool asynchronous)
{
    QVector<EventSink> sinks;
    {
        QMutexLocker lock(&m_mutex);
        for (auto it = m_entries.cbegin(); it != m_entries.cend(); ++it) {
            if (it->kind == kind && !it->stopped)
                sinks.push_back(it->sink);
        }
    }
    auto deliver = [sinks = std::move(sinks), event = std::move(event)]() mutable {
        for (const EventSink& sink : sinks)
            sink(event);
    };
    if (asynchronous)
        std::thread(deliver).detach();
    else
        deliver();
}

} // namespace ModernInput
