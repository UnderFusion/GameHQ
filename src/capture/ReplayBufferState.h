#pragma once

#include <QObject>
#include <QString>

// GUI-thread state shared by FramePumpService and its focused tests. A command
// creates a generation; only confirmed worker facts from that generation may
// advance it. Failed and Stopped are terminal until a new command is issued.
class ReplayBufferState : public QObject
{
    Q_OBJECT
public:
    enum State { Stopped, Starting, Recording, Ready, Failed };
    Q_ENUM(State)

    explicit ReplayBufferState(QObject* parent = nullptr) : QObject(parent) {}
    State state() const { return m_state; }
    quint64 generation() const { return m_generation; }
    QString gameName() const { return m_gameName; }
    bool startRequested() const { return m_state == Starting || isRecording(m_state); }
    // Admit a save attempt once capture has started. The worker finalizes the
    // current partial segment and verifies usable video before exporting it.
    bool canSave() const { return canSave(m_state); }
    static bool canSave(State state) { return isRecording(state); }
    static bool isRecording(State state) { return state == Recording || state == Ready; }
    static QString saveRejection(State state);
    // Worker failure reasons when audio was requested but cannot be recorded.
    // Clips must not silently fall back to video-only, and game audio must not
    // silently become desktop audio. audioFailure() is the desktop source.
    static QString audioFailure();
    static QString gameAudioFailure();
    static QString gameAudioUnsupported();
    static bool isAudioFailure(const QString& reason);
    // Why the next save must be refused. A save on a failed buffer re-arms it,
    // so an audio failure stays the answer until a generation records again.
    QString saveRejection() const;

    // Audio recovery budget shared by capture-read and encoder failures: at
    // most kMax re-arms per kWindowMs, then the caller fails the buffer.
    struct AudioRearmBudget {
        static constexpr int kMax = 3;
        static constexpr qint64 kWindowMs = 120000;
        int used = 0;
        qint64 windowStartMs = 0;
        bool allow(qint64 nowMs)
        {
            if (used == 0 || nowMs - windowStartMs > kWindowMs) {
                windowStartMs = nowMs;
                used = 0;
            }
            return ++used <= kMax;
        }
    };

    quint64 requestStart(const QString& gameName);
    quint64 requestStop();
    bool confirm(quint64 generation, State state, const QString& reason = {});

signals:
    void stateChanged(ReplayBufferState::State state, const QString& gameName);
    void recordingStateChanged(bool active, const QString& gameName);
    void failed(const QString& reason);

private:
    void setState(State state, const QString& gameName);
    State m_state = Stopped;
    quint64 m_generation = 0;
    QString m_gameName;
    QString m_audioFailure;   // set while the current Failed state is an audio failure
};
