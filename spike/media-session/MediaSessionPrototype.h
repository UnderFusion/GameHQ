#pragma once

#include <QFlags>
#include <QList>
#include <QObject>
#include <QString>

namespace MediaSessionPrototype {

enum class Command { Play = 1, Pause = 2, Next = 4, Previous = 8 };
Q_DECLARE_FLAGS(Commands, Command)

struct Session {
    // An opaque backend key identifies the session, including when one
    // application has multiple sessions. The application ID is display data.
    QString id;
    QString appId;
    QString title;
    QString artist;
    Commands enabled;
};

class IMediaSessionService : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    virtual void subscribe(quint64 generation) = 0;
    virtual void unsubscribe() = 0;
    virtual void request(quint64 requestId, const QString& sessionId, Command command) = 0;
    virtual void cancel(quint64 requestId) = 0;

signals:
    void sessionsChanged(quint64 generation, const QList<Session>& sessions);
    void commandFinished(quint64 requestId, bool accepted);
};

// The prototype lives on one Qt thread. The backend outlives the client
// and marshals external callbacks to that thread before publishing signals.
class Client : public QObject
{
    Q_OBJECT
public:
    explicit Client(IMediaSessionService& service);
    ~Client() override;

    static constexpr int maxSessions = 16;
    static constexpr int maxTextLength = 256;

    void activate();
    void deactivate();
    QList<Session> sessions() const { return m_sessions; }
    QString selectedId() const { return m_selectedId; }
    bool select(const QString& id);
    bool send(Command command);

signals:
    void commandFinished(bool accepted);

private:
    const Session* find(const QString& id) const;
    void updateSessions(quint64 generation, const QList<Session>& sessions);
    void cancelPending();

    IMediaSessionService& m_service;
    QList<Session> m_sessions;
    QString m_selectedId;
    quint64 m_sequence = 0;
    quint64 m_generation = 0;
    quint64 m_pending = 0;
};

} // namespace MediaSessionPrototype
