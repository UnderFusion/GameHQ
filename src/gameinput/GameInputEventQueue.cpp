#include "gameinput/GameInputEventQueue.h"

#include <QMutexLocker>

#include <algorithm>

namespace ModernInput {

GameInputEventQueue::GameInputEventQueue(int normalCapacity, int emergencyReserve)
    : m_normalCapacity(std::max(1, normalCapacity))
    , m_emergencyReserve(std::max(1, emergencyReserve))
{
}

bool GameInputEventQueue::push(GameInputEvent event)
{
    if (!accepting())
        return false;

    QMutexLocker lock(&m_mutex);
    if (!accepting())
        return false;

    event.sequence = m_nextSequence++;

    if (event.kind == GameInputEventKind::Reading) {
        // Only a reading that leaves every button bit unchanged may coalesce.
        // Button transitions are lossless, ordered edges: coalescing them
        // could erase a quick press→release that lands before one drain.
        const auto last = m_lastButtons.constFind(event.deviceId);
        const bool buttonsChanged = last == m_lastButtons.cend()
            || last->standardButtons != event.standardButtons
            || last->buttonStates != event.buttonStates;
        if (!buttonsChanged) {
            const int pending = m_discrete.size() + m_latestStates.size();
            if (m_latestStates.contains(event.deviceId)
                || pending < m_normalCapacity) {
                m_latestStates.insert(event.deviceId, std::move(event));
            } else {
                noteOverflowLocked(event.deviceId);
            }
            m_available.wakeOne();
            return true;
        }
        m_lastButtons.insert(event.deviceId,
                             {event.standardButtons, event.buttonStates});
        // Any pending coalesced reading is older than this edge; delivering
        // it after the edge would replay stale state. Drop it.
        m_latestStates.remove(event.deviceId);
    } else if (event.kind == GameInputEventKind::DeviceRemoved) {
        // Readings queued before the removal must not be delivered after it:
        // the consumer would re-observe a device that is already gone.
        m_latestStates.remove(event.deviceId);
        m_lastButtons.remove(event.deviceId);
    }

    const int pending = m_discrete.size() + m_latestStates.size();

    // Discrete edges outrank coalescible readings. Evict one reading before
    // entering the emergency reserve; no press/release/lifecycle event is
    // coalesced or reordered while reserve space remains.
    if (pending >= m_normalCapacity && !m_latestStates.isEmpty())
        m_latestStates.erase(m_latestStates.begin());

    if (m_discrete.size() + m_latestStates.size()
            < m_normalCapacity + m_emergencyReserve) {
        m_discrete.enqueue(std::move(event));
    } else {
        noteOverflowLocked(event.deviceId);
    }

    m_available.wakeOne();
    return true;
}

GameInputEventBatch GameInputEventQueue::take()
{
    QMutexLocker lock(&m_mutex);
    return takeLocked();
}

GameInputEventBatch GameInputEventQueue::waitTake()
{
    QMutexLocker lock(&m_mutex);
    while (m_discrete.isEmpty() && m_latestStates.isEmpty() && !m_forceResync
           && accepting()) {
        m_available.wait(&m_mutex);
    }
    return takeLocked();
}

bool GameInputEventQueue::waitForDrainRequest()
{
    QMutexLocker lock(&m_mutex);
    while (accepting()
           && (m_drainRequested
               || (m_discrete.isEmpty() && m_latestStates.isEmpty() && !m_forceResync))) {
        m_available.wait(&m_mutex);
    }
    if (!accepting())
        return false;
    m_drainRequested = true;
    return true;
}

GameInputEventBatch GameInputEventQueue::takeForDrain()
{
    QMutexLocker lock(&m_mutex);
    // Events pushed while this drain was pending are taken here too; any
    // pushed after it see no outstanding drain and wake the waiter.
    m_drainRequested = false;
    return takeLocked();
}

void GameInputEventQueue::stopAccepting()
{
    m_accepting.store(false, std::memory_order_release);
    QMutexLocker lock(&m_mutex);
    m_available.wakeAll();
}

int GameInputEventQueue::pendingCount() const
{
    QMutexLocker lock(&m_mutex);
    return m_discrete.size() + m_latestStates.size();
}

GameInputEventBatch GameInputEventQueue::takeLocked()
{
    GameInputEventBatch batch;
    batch.events.reserve(m_discrete.size() + m_latestStates.size()
                         + m_uncertainDevices.size());
    while (!m_discrete.isEmpty())
        batch.events.push_back(m_discrete.dequeue());

    // State order is intentionally unspecified: coalesced readings are
    // button-stable by construction, so each device contributes only its
    // newest edge-free reading after every discrete event already queued.
    for (auto it = m_latestStates.cbegin(); it != m_latestStates.cend(); ++it)
        batch.events.push_back(it.value());
    m_latestStates.clear();

    batch.forceResync = m_forceResync;
    batch.overflowCount = m_overflowCount;
    batch.uncertainDevices = QStringList(m_uncertainDevices.cbegin(),
                                         m_uncertainDevices.cend());
    batch.uncertainDevices.sort();
    if (batch.forceResync) {
        for (const QString& deviceId : batch.uncertainDevices) {
            GameInputEvent recovery;
            recovery.kind = GameInputEventKind::RecoveryRequired;
            recovery.sequence = m_nextSequence++;
            recovery.deviceId = deviceId;
            batch.events.push_back(std::move(recovery));
        }
    }

    m_forceResync = false;
    m_uncertainDevices.clear();
    return batch;
}

void GameInputEventQueue::noteOverflowLocked(const QString& deviceId)
{
    ++m_overflowCount;
    m_forceResync = true;
    m_uncertainDevices.insert(deviceId.isEmpty() ? QStringLiteral("*") : deviceId);
}

} // namespace ModernInput

