#include "MediaSessionPrototype.h"

#include <algorithm>
#include <utility>

namespace MediaSessionPrototype {

Client::Client(IMediaSessionService& service)
    : m_service(service)
{
    connect(&m_service, &IMediaSessionService::sessionsChanged, this, &Client::updateSessions);
    connect(&m_service, &IMediaSessionService::commandFinished, this,
            [this](quint64 requestId, bool accepted) {
                if (!m_pending || requestId != m_pending)
                    return;
                m_pending = 0;
                emit commandFinished(accepted);
            });
}

Client::~Client()
{
    deactivate();
}

void Client::activate()
{
    if (m_generation)
        return;
    m_generation = ++m_sequence;
    m_service.subscribe(m_generation);
}

void Client::deactivate()
{
    if (!std::exchange(m_generation, 0))
        return;
    m_sessions.clear();
    m_selectedId.clear();
    cancelPending();
    m_service.unsubscribe();
}

const Session* Client::find(const QString& id) const
{
    const auto it = std::find_if(m_sessions.cbegin(), m_sessions.cend(),
                                 [&id](const Session& session) { return session.id == id; });
    return it == m_sessions.cend() ? nullptr : &*it;
}

void Client::updateSessions(quint64 generation, const QList<Session>& sessions)
{
    if (!m_generation || generation != m_generation)
        return;
    m_sessions.clear();
    for (const Session& session : sessions) {
        if (m_sessions.size() == maxSessions)
            break;
        if (session.id.isEmpty() || session.id.size() > maxTextLength || find(session.id))
            continue;
        m_sessions.append({session.id, session.appId.left(maxTextLength),
                           session.title.left(maxTextLength), session.artist.left(maxTextLength),
                           session.enabled});
    }
    if (!m_selectedId.isEmpty() && !find(m_selectedId)) {
        m_selectedId.clear();
        cancelPending();
    }
}

bool Client::select(const QString& id)
{
    if (!find(id))
        return false;
    if (id != m_selectedId) {
        m_selectedId = id;
        cancelPending();
    }
    return true;
}

bool Client::send(Command command)
{
    const Session* session = find(m_selectedId);
    if (!session || !session->enabled.testFlag(command) || m_pending)
        return false;
    const QString sessionId = session->id;
    m_pending = ++m_sequence;
    m_service.request(m_pending, sessionId, command);
    return true;
}

void Client::cancelPending()
{
    if (const quint64 requestId = std::exchange(m_pending, 0))
        m_service.cancel(requestId);
}

} // namespace MediaSessionPrototype
