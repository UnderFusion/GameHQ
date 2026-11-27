#pragma once

#include "gameinput/GameInputEvent.h"

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QWaitCondition>

#include <atomic>

namespace ModernInput {

// Multi-producer/single-consumer queue used by GameInput callback threads.
// Discrete events retain arrival order. Only button-stable readings are
// latest-state-only per logical device; a reading that changes any button
// bit is a real edge and is queued discretely so a press→release pair that
// arrives before one drain can never be coalesced away.
class GameInputEventQueue
{
public:
    explicit GameInputEventQueue(int normalCapacity = 256, int emergencyReserve = 64);

    bool push(GameInputEvent event);
    GameInputEventBatch take();
    GameInputEventBatch waitTake();

    // Bounded hand-off to a GUI consumer. waitForDrainRequest() blocks until
    // something is pending and no drain is outstanding, then marks one
    // outstanding and returns true (false once stopped). takeForDrain()
    // clears that mark and takes everything under the same lock, so an event
    // pushed afterwards always wakes the waiter for a new drain: never more
    // than one outstanding, never a lost wakeup.
    bool waitForDrainRequest();
    GameInputEventBatch takeForDrain();

    void stopAccepting();
    bool accepting() const { return m_accepting.load(std::memory_order_acquire); }
    int pendingCount() const;

private:
    struct LastButtons
    {
        quint32 standardButtons = 0;
        QVector<quint8> buttonStates;
    };

    GameInputEventBatch takeLocked();
    void noteOverflowLocked(const QString& deviceId);

    const int m_normalCapacity;
    const int m_emergencyReserve;
    mutable QMutex m_mutex;
    QWaitCondition m_available;
    QQueue<GameInputEvent> m_discrete;
    QHash<QString, GameInputEvent> m_latestStates;
    QHash<QString, LastButtons> m_lastButtons;
    QSet<QString> m_uncertainDevices;
    quint64 m_nextSequence = 1;
    quint64 m_overflowCount = 0;
    bool m_forceResync = false;
    bool m_drainRequested = false;
    std::atomic_bool m_accepting{true};
};

} // namespace ModernInput

