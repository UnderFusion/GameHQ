#include "capture/ReplayBufferState.h"

quint64 ReplayBufferState::requestStart(const QString& gameName)
{
    ++m_generation;
    setState(Starting, gameName);
    return m_generation;
}

quint64 ReplayBufferState::requestStop()
{
    ++m_generation;
    setState(Stopped, {});
    return m_generation;
}

bool ReplayBufferState::confirm(quint64 generation, State state, const QString& reason)
{
    if (generation != m_generation || !startRequested())
        return false;
    const bool allowed = (m_state == Starting && state == Recording)
        || (m_state == Recording && state == Ready)
        || (m_state == Ready && state == Recording) || state == Failed;
    if (!allowed)
        return false;
    if (state == Recording)
        m_audioFailure.clear();
    else if (state == Failed)
        m_audioFailure = isAudioFailure(reason) ? reason : QString();
    setState(state, m_gameName);
    if (state == Failed)
        emit failed(reason.isEmpty() ? saveRejection(Failed) : reason);
    return true;
}

void ReplayBufferState::setState(State state, const QString& gameName)
{
    const bool wasRecording = isRecording(m_state);
    const QString previousGame = wasRecording ? m_gameName : QString();
    const bool changed = m_state != state || m_gameName != gameName;
    m_state = state;
    m_gameName = gameName;
    if (changed)
        emit stateChanged(state, gameName);
    const bool recording = isRecording(state);
    const QString recordingGame = recording ? gameName : QString();
    if (wasRecording != recording || previousGame != recordingGame)
        emit recordingStateChanged(recording, recordingGame);
}

QString ReplayBufferState::saveRejection(State state)
{
    switch (state) {
    case Starting:
        return QStringLiteral("Replay buffer is starting; try saving again in a few seconds");
    case Recording:
        return QStringLiteral("Replay buffer is collecting footage; try saving again in a few seconds");
    case Failed:
        return QStringLiteral("Replay buffer failed; no clip was saved");
    case Stopped:
        return QStringLiteral("Replay buffer is not running");
    case Ready:
        return {};
    }
    return QStringLiteral("Replay buffer is not running");
}

QString ReplayBufferState::audioFailure()
{
    return QStringLiteral("Desktop audio could not be recorded, so no clip was saved. "
                          "Check the Windows sound output or change the audio source in Replay settings");
}

QString ReplayBufferState::gameAudioFailure()
{
    return QStringLiteral("Game audio could not be recorded, so no clip was saved. "
                          "Choose Desktop audio or Off as the audio source in Replay settings");
}

QString ReplayBufferState::gameAudioUnsupported()
{
    return QStringLiteral("Game audio needs Windows 11, so no clip was saved. "
                          "Choose Desktop audio or Off as the audio source in Replay settings");
}

bool ReplayBufferState::isAudioFailure(const QString& reason)
{
    return reason == audioFailure() || reason == gameAudioFailure()
        || reason == gameAudioUnsupported();
}

QString ReplayBufferState::saveRejection() const
{
    if (!m_audioFailure.isEmpty() && (m_state == Failed || m_state == Starting))
        return m_audioFailure;
    return saveRejection(m_state);
}
