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
    bool canSave() const { return m_state == Ready; }
    static bool isRecording(State state) { return state == Recording || state == Ready; }
    static QString saveRejection(State state);

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
};
