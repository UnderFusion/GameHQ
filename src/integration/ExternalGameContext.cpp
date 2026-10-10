#include "integration/ExternalGameContext.h"

#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
#include <QReadLocker>
#include <QTimer>
#include <QWriteLocker>

#include <cmath>
#include <limits>
#include <utility>

namespace
{
bool boundedString(const QJsonObject &object, const QString &key, int maximum,
                   QString &value, QString &error, bool required = false)
{
    const QJsonValue field = object.value(key);
    if (field.isUndefined() && !required) {
        value.clear();
        return true;
    }
    if (!field.isString() || field.toString().isEmpty() || field.toString().size() > maximum) {
        error = key + QStringLiteral(" is missing or invalid");
        return false;
    }
    value = field.toString();
    return true;
}

bool pathInside(const QString &candidate, const QString &directory)
{
    if (candidate.isEmpty() || directory.isEmpty())
        return false;
    const QString cleanCandidate = QDir::cleanPath(QDir::fromNativeSeparators(candidate));
    const QString cleanDirectory = QDir::cleanPath(QDir::fromNativeSeparators(directory));
    if (!QDir::isAbsolutePath(cleanCandidate) || !QDir::isAbsolutePath(cleanDirectory)
        || QDir(cleanDirectory).isRoot()) {
        return false;
    }
    const QString relative = QDir(cleanDirectory).relativeFilePath(cleanCandidate);
    // On another drive relativeFilePath() returns the candidate unchanged
    // (still absolute); that is never "inside" the install directory.
    return !QDir::isAbsolutePath(relative)
        && relative != QStringLiteral(".") && relative != QStringLiteral("..")
        && !relative.startsWith(QStringLiteral("../"));
}
}

namespace integration
{
bool isXboxShellSurface(const QString &processName, const QString &windowTitle)
{
    const QString exe = processName.toLower();
    if (exe == QStringLiteral("xboxpcapp.exe") || exe == QStringLiteral("gamebar.exe"))
        return true;
    // ApplicationFrameHost hosts every packaged app; only its Xbox window
    // counts, never Calculator, Settings or any other app it frames.
    return exe == QStringLiteral("applicationframehost.exe")
        && windowTitle.trimmed().compare(QStringLiteral("Xbox"), Qt::CaseInsensitive) == 0;
}

ExternalGameContext::ExternalGameContext(QObject *parent)
    : QObject(parent)
{
}

QString ExternalGameContext::key(const QString &sourceId, const QString &sessionId)
{
    return sourceId + QChar(0x1f) + sessionId;
}

bool ExternalGameContext::parseSession(const QString &sourceId, const QJsonObject &object,
                                       const QString &phase, ExternalGameSession &session,
                                       QString &error)
{
    error.clear();
    session = {};
    session.sourceId = sourceId;
    session.phase = phase;
    if (!boundedString(object, QStringLiteral("sessionId"), 128,
                       session.sessionId, error, true)
        || !boundedString(object, QStringLiteral("playniteGameId"), 128,
                          session.playniteGameId, error)
        || !boundedString(object, QStringLiteral("name"), 256, session.name, error)
        || !boundedString(object, QStringLiteral("sourceName"), 128,
                          session.sourceName, error)
        || !boundedString(object, QStringLiteral("installDirectory"), 4096,
                          session.installDirectory, error)
        || !boundedString(object, QStringLiteral("selectedRomFile"), 4096,
                          session.selectedRomFile, error)) {
        return false;
    }

    const QJsonValue platforms = object.value(QStringLiteral("platformNames"));
    if (!platforms.isUndefined()) {
        if (!platforms.isArray() || platforms.toArray().size() > 16) {
            error = QStringLiteral("platformNames is invalid");
            return false;
        }
        for (const QJsonValue value : platforms.toArray()) {
            if (!value.isString() || value.toString().isEmpty() || value.toString().size() > 128) {
                error = QStringLiteral("platformNames contains an invalid value");
                return false;
            }
            session.platformNames.push_back(value.toString());
        }
    }

    const QJsonValue processId = object.value(QStringLiteral("startedProcessId"));
    if (!processId.isUndefined()) {
        const double value = processId.toDouble(-1);
        if (!processId.isDouble() || value < 0 || std::floor(value) != value
            || value > std::numeric_limits<quint32>::max()) {
            error = QStringLiteral("startedProcessId is invalid");
            return false;
        }
        // Playnite reports 0 when it could not identify the started process
        // (folder- or script-tracked launches). That is "unknown", not a
        // reason to drop the whole lifecycle message or state snapshot.
        session.startedProcessId = static_cast<quint32>(value);
    }

    const QJsonValue occurred = object.value(QStringLiteral("occurredAtUtc"));
    if (!occurred.isUndefined()) {
        if (!occurred.isString()) {
            error = QStringLiteral("occurredAtUtc is invalid");
            return false;
        }
        session.occurredAtUtc = QDateTime::fromString(occurred.toString(), Qt::ISODate);
        if (!session.occurredAtUtc.isValid()) {
            error = QStringLiteral("occurredAtUtc is invalid");
            return false;
        }
        session.occurredAtUtc = session.occurredAtUtc.toUTC();
    }
    return true;
}

bool ExternalGameContext::upsert(const QString &sourceId, const QJsonObject &object,
                                 const QString &phase, QString &error)
{
    ExternalGameSession session;
    if (!parseSession(sourceId, object, phase, session, error))
        return false;
    QWriteLocker locker(&m_lock);
    m_sessions.insert(key(sourceId, session.sessionId), session);
    locker.unlock();
    emit changed();
    return true;
}

bool ExternalGameContext::replaceSource(const QString &sourceId, const QJsonArray &games,
                                        QString &error)
{
    if (games.size() > 64) {
        error = QStringLiteral("state snapshot contains too many games");
        return false;
    }
    QList<ExternalGameSession> parsed;
    QHash<QString, bool> seen;
    for (const QJsonValue value : games) {
        if (!value.isObject()) {
            error = QStringLiteral("state snapshot contains a non-object game");
            return false;
        }
        ExternalGameSession session;
        if (!parseSession(sourceId, value.toObject(), QStringLiteral("started"),
                          session, error)) {
            return false;
        }
        if (seen.contains(session.sessionId)) {
            error = QStringLiteral("state snapshot contains duplicate sessions");
            return false;
        }
        seen.insert(session.sessionId, true);
        parsed.push_back(session);
    }
    {
        QWriteLocker locker(&m_lock);
        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            if (it->sourceId == sourceId)
                it = m_sessions.erase(it);
            else
                ++it;
        }
        for (const ExternalGameSession &session : std::as_const(parsed))
            m_sessions.insert(key(sourceId, session.sessionId), session);
    }
    emit changed();
    return true;
}

bool ExternalGameContext::remove(const QString &sourceId, const QString &sessionId)
{
    QWriteLocker locker(&m_lock);
    if (m_sessions.remove(key(sourceId, sessionId)) == 0)
        return false;
    locker.unlock();
    emit changed();
    return true;
}

void ExternalGameContext::clearSource(const QString &sourceId)
{
    bool removed = false;
    {
        QWriteLocker locker(&m_lock);
        for (auto it = m_sessions.begin(); it != m_sessions.end();) {
            if (it->sourceId == sourceId) {
                it = m_sessions.erase(it);
                removed = true;
            } else {
                ++it;
            }
        }
    }
    if (removed)
        emit changed();
}

void ExternalGameContext::scheduleSourceExpiry(const QString &sourceId, int graceMs)
{
    cancelSourceExpiry(sourceId);
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, sourceId, timer] {
        m_expiryTimers.remove(sourceId);
        clearSource(sourceId);
        timer->deleteLater();
    });
    m_expiryTimers.insert(sourceId, timer);
    timer->start(qMax(0, graceMs));
}

void ExternalGameContext::cancelSourceExpiry(const QString &sourceId)
{
    if (QTimer *timer = m_expiryTimers.take(sourceId)) {
        timer->stop();
        timer->deleteLater();
    }
}

QList<ExternalGameSession> ExternalGameContext::sessions() const
{
    QReadLocker locker(&m_lock);
    return m_sessions.values();
}

int ExternalGameContext::sessionCount() const
{
    QReadLocker locker(&m_lock);
    return m_sessions.size();
}

QList<ExternalGameSession> ExternalGameContext::launchedSessions() const
{
    QList<ExternalGameSession> launched;
    for (const ExternalGameSession &session : sessions()) {
        if (session.startedProcessId != 0 && session.phase == QStringLiteral("started"))
            launched.push_back(session);
    }
    return launched;
}

ExternalGameMatch ExternalGameContext::matchForeground(
    quint32 foregroundPid, const QString &executablePath, bool alreadyGame,
    const std::function<bool(quint32, quint32)> &isDescendant) const
{
    const QList<ExternalGameSession> snapshot = sessions();
    for (const ExternalGameSession &session : snapshot) {
        if (session.startedProcessId != 0 && session.startedProcessId == foregroundPid)
            return { MatchConfidence::ExactProcess, session };
    }
    if (foregroundPid != 0 && isDescendant) {
        for (const ExternalGameSession &session : snapshot) {
            if (session.startedProcessId != 0
                && isDescendant(foregroundPid, session.startedProcessId)) {
                return { MatchConfidence::DescendantProcess, session };
            }
        }
    }
    if (alreadyGame) {
        for (const ExternalGameSession &session : snapshot) {
            if (pathInside(executablePath, session.installDirectory))
                return { MatchConfidence::InstallDirectory, session };
        }
    }
    return {};
}
} // namespace integration
