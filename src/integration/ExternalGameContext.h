#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QReadWriteLock>
#include <QStringList>

#include <functional>

class QTimer;

namespace integration
{
struct ExternalGameSession
{
    QString sourceId;
    QString sessionId;
    QString playniteGameId;
    QString name;
    QString sourceName;
    QStringList platformNames;
    QString installDirectory;
    QString selectedRomFile;
    quint32 startedProcessId = 0;
    QDateTime occurredAtUtc;
    QString phase;
};

enum class MatchConfidence { None, InstallDirectory, DescendantProcess, ExactProcess };

struct ExternalGameMatch
{
    MatchConfidence confidence = MatchConfidence::None;
    ExternalGameSession session;
    bool authorizesWindowedCapture() const
    {
        return confidence == MatchConfidence::ExactProcess
            || confidence == MatchConfidence::DescendantProcess;
    }
};

// Xbox full screen experience surfaces (the Xbox app home, Game Bar) that can
// hold the foreground while a game launched from Playnite is running. Only
// these may hand the capture gate over to a verified Playnite game window.
bool isXboxShellSurface(const QString &processName, const QString &windowTitle);

class ExternalGameContext final : public QObject
{
    Q_OBJECT
public:
    explicit ExternalGameContext(QObject *parent = nullptr);

    bool upsert(const QString &sourceId, const QJsonObject &object,
                const QString &phase, QString &error);
    bool replaceSource(const QString &sourceId, const QJsonArray &games,
                       QString &error);
    bool remove(const QString &sourceId, const QString &sessionId);
    void clearSource(const QString &sourceId);
    void scheduleSourceExpiry(const QString &sourceId, int graceMs);
    void cancelSourceExpiry(const QString &sourceId);

    QList<ExternalGameSession> sessions() const;
    int sessionCount() const;
    // Started sessions whose launched process id is known: the only ones whose
    // windows can be verified by process identity rather than by name or path.
    QList<ExternalGameSession> launchedSessions() const;
    ExternalGameMatch matchForeground(
        quint32 foregroundPid, const QString &executablePath, bool alreadyGame,
        const std::function<bool(quint32, quint32)> &isDescendant) const;

signals:
    void changed();

private:
    static bool parseSession(const QString &sourceId, const QJsonObject &object,
                             const QString &phase, ExternalGameSession &session,
                             QString &error);
    static QString key(const QString &sourceId, const QString &sessionId);

    QHash<QString, ExternalGameSession> m_sessions;
    mutable QReadWriteLock m_lock;
    QHash<QString, QTimer *> m_expiryTimers;
};
} // namespace integration
