#pragma once

#include <QObject>
#include <QString>

class QTimer;

// Tray status model (issue #38).
//
// Pure logic: it turns capture facts into one visible state plus one tooltip
// string, and nothing else. No icons, no widgets, no QSystemTrayIcon — the
// view (TrayIcon + TrayIconRenderer) draws whatever this says. That split is
// the whole point: the transient-badge timing and the "only green when we
// really would capture" rule are testable without a GUI or a game running.
//
// Everything is event driven. The only timer is the single-shot transient
// timer below; there is no polling loop here, and the model never asks the
// system anything.
class TrayStatus : public QObject
{
    Q_OBJECT
public:
    // What the icon shows right now. Idle and Ready are the two stable states;
    // the other three are transient and always fall back to one of them.
    enum class Visual {
        Idle,             // no qualifying game: plain GameHQ icon
        Ready,            // qualifying game foreground AND capture armed
        ScreenshotSaved,  // transient: yellow badge
        ClipSaved,        // transient: red badge
        CaptureProblem,   // transient: amber warning
    };
    Q_ENUM(Visual)

    // The "Capture:" tooltip line. Kept separate from Visual because a game can
    // be foreground while the buffer is still coming up ("Starting"), which is
    // neither Ready nor Idle.
    // ScreenshotsOnly: the foreground game may be captured, but no replay
    // buffer is running (replay off, stopped or failed) - screenshots still work.
    enum class Capture { Waiting, Starting, Ready, ScreenshotsOnly };
    Q_ENUM(Capture)

    // How long a transient badge stays up. One value for all of them so the
    // model needs exactly one timer.
    static constexpr int kTransientMs = 3000;

    // Windows clips a tray tooltip at 127 characters. Stay one under so the
    // platform never truncates mid-word behind our back.
    static constexpr int kTooltipMaxChars = 126;

    explicit TrayStatus(QObject* parent = nullptr);
    ~TrayStatus() override;

    // Foreground game from FramePumpService::foregroundGameDetected.
    // `recognizedGame` is that signal's "this is a game we would capture"
    // verdict: a process that merely exists never turns the state Ready.
    // `captureEligible` is GameDetector::shouldCapture for that window: the
    // same gate screenshots and the replay buffer use (whitelist, pause menu).
    void setForegroundGame(const QString& name, bool recognizedGame, bool captureEligible);
    void clearForegroundGame();

    // ReplayBufferState::State as delivered by
    // FramePumpService::bufferStateChanged. Taken as int so this header stays
    // free of the capture headers.
    void setReplayBufferState(int state);

    // Confirmed facts only. These are wired to ScreenshotService::captured,
    // FramePumpService::clipSaved and the matching failure signals — never to
    // a "saving" or "requested" notification.
    void reportScreenshotSaved();
    void reportClipSaved();
    void reportCaptureProblem();

    // Rebuild the tooltip after a language change. Emits changed() only when
    // the text really moved.
    void retranslate();

    Visual visual() const { return m_transient == Visual::Idle ? baseVisual() : m_transient; }
    Visual baseVisual() const;
    Capture capture() const;
    QString gameName() const { return m_gameName; }
    bool hasGame() const { return m_recognized; }
    QString tooltip() const { return m_tooltip; }

signals:
    // Visible state or tooltip text actually changed. Never emitted for a
    // repeated identical input, so the tray icon is only re-rendered on real
    // transitions.
    void changed();

private:
    void startTransient(Visual which);
    void expireTransient();
    void publish();
    QString composeTooltip() const;
    QString statusText() const;
    QString captureText() const;

    QTimer* m_timer = nullptr;
    quint64 m_transientGeneration = 0;   // bumped by every new transient
    quint64 m_armedGeneration = 0;       // generation the running timer belongs to
    Visual m_transient = Visual::Idle;   // Idle == "no transient active"

    QString m_gameName;
    bool m_recognized = false;
    bool m_eligible = false;
    int m_bufferState = 0;               // ReplayBufferState::Stopped

    QString m_tooltip;                   // published tooltip, rebuilt by publish()
    Visual m_publishedVisual = Visual::Idle;
};
