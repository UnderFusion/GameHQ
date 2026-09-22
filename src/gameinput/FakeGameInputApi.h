#pragma once

#include "gameinput/IGameInputApi.h"

#include <QHash>
#include <QMutex>
#include <QStringList>

#include <atomic>

namespace ModernInput {

// Deterministic test implementation. It deliberately retains retired sinks so
// lifetime tests can emulate a misbehaving runtime callback after unregister.
class FakeGameInputApi final : public IGameInputApi
{
public:
    bool initialize(QString& error) override;
    void applyFocusPolicy(GameInputFocusMode mode) override;
    CallbackToken registerDeviceCallback(EventSink sink) override;
    CallbackToken registerReadingCallback(EventSink sink) override;
    CallbackToken registerSystemButtonCallback(EventSink sink) override;
    void stopCallback(CallbackToken token) override;
    bool unregisterCallback(CallbackToken token) override;
    void releaseDevices() override;
    void unload() override;
    bool loaded() const override { return m_loaded.load(); }
    QString runtimeDescription() const override { return QStringLiteral("fake GameInput"); }

    void setInitializeFailure(QString error);
    void emitDevice(GameInputEvent event, bool asynchronous = false);
    void emitReading(GameInputEvent event, bool asynchronous = false);
    void emitSystem(GameInputEvent event, bool asynchronous = false);
    void emitRetired(GameInputEvent event);

    QStringList callLog() const;

private:
    enum class Kind { Device, Reading, System };
    struct Entry { Kind kind; EventSink sink; bool stopped = false; };

    CallbackToken add(Kind kind, EventSink sink);
    void emitKind(Kind kind, GameInputEvent event, bool asynchronous);

    mutable QMutex m_mutex;
    QHash<CallbackToken, Entry> m_entries;
    QVector<EventSink> m_retired;
    QStringList m_callLog;
    QString m_initializeError;
    CallbackToken m_nextToken = 1;
    std::atomic_bool m_loaded{false};
};

} // namespace ModernInput

