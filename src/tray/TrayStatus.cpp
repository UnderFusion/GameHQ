#include "tray/TrayStatus.h"

#include "Brand.h"
#include "capture/ReplayBufferState.h"
#include "localization/NativeText.h"

#include <QTimer>

TrayStatus::TrayStatus(QObject* parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    // One single-shot timer for every transient. Precise so the 3 s badge
    // lifetime is the same on a busy capture thread as on an idle desktop.
    m_timer->setSingleShot(true);
    m_timer->setTimerType(Qt::PreciseTimer);
    connect(m_timer, &QTimer::timeout, this, &TrayStatus::expireTransient);

    // Seed the published snapshot without emitting: construction is not a
    // state change the view needs to react to.
    m_tooltip = composeTooltip();
    m_publishedVisual = visual();
}

TrayStatus::~TrayStatus() = default;

void TrayStatus::setForegroundGame(const QString& name, bool recognizedGame, bool captureEligible)
{
    setCaptureTarget(name, recognizedGame, captureEligible, false, false, false);
}

void TrayStatus::setCaptureTarget(const QString& name, bool recognizedGame, bool captureEligible,
                                  bool background, bool pinned, bool paused)
{
    // Only a recognized game counts. FramePumpService reports every valid
    // foreground window, including ones the game heuristic rejects, and those
    // must not make the badge green.
    const QString trimmed = recognizedGame ? name.trimmed() : QString();
    const bool recognized = recognizedGame && !trimmed.isEmpty();
    const bool eligible = recognized && captureEligible;
    if (trimmed == m_gameName && recognized == m_recognized && eligible == m_eligible
        && background == m_background && pinned == m_pinned && paused == m_paused)
        return;
    m_gameName = trimmed;
    m_recognized = recognized;
    m_eligible = eligible;
    m_background = background;
    m_pinned = pinned;
    m_paused = paused;
    publish();
}

void TrayStatus::clearForegroundGame()
{
    setForegroundGame(QString(), false, false);
}

void TrayStatus::setReplayBufferState(int state)
{
    if (state == m_bufferState)
        return;
    m_bufferState = state;
    publish();
}

void TrayStatus::reportScreenshotSaved()
{
    startTransient(Visual::ScreenshotSaved);
}

void TrayStatus::reportClipSaved()
{
    startTransient(Visual::ClipSaved);
}

void TrayStatus::reportCaptureProblem()
{
    startTransient(Visual::CaptureProblem);
}

void TrayStatus::retranslate()
{
    publish();
}

TrayStatus::Visual TrayStatus::baseVisual() const
{
    const Capture c = capture();
    return (m_recognized && (c == Capture::Ready || c == Capture::ScreenshotsOnly))
        ? Visual::Ready : Visual::Idle;
}

TrayStatus::Capture TrayStatus::capture() const
{
    // A buffer still armed for the previous window (foreground grace) must not
    // vouch for a window GameHQ may not capture.
    if (!m_eligible)
        return Capture::Waiting;
    if (m_paused)
        return Capture::Paused;
    switch (m_bufferState) {
    case ReplayBufferState::Starting:
        return Capture::Starting;
    case ReplayBufferState::Recording:
    case ReplayBufferState::Ready:
        return Capture::Ready;
    case ReplayBufferState::Stopped:
    case ReplayBufferState::Failed:
    default:
        return Capture::ScreenshotsOnly;
    }
}

void TrayStatus::startTransient(Visual which)
{
    m_transient = which;
    // Every transient owns a generation and the timer is restarted for it. A
    // timeout armed for an older transient therefore finds a different running
    // generation and cannot clear the newer badge.
    ++m_transientGeneration;
    m_armedGeneration = m_transientGeneration;
    m_timer->start(kTransientMs);
    publish();
}

void TrayStatus::expireTransient()
{
    if (m_armedGeneration != m_transientGeneration)
        return;   // stale timeout from a superseded transient
    if (m_transient == Visual::Idle)
        return;
    m_transient = Visual::Idle;
    m_timer->stop();
    // The underlying state is derived on demand from the game and buffer
    // inputs, so restoring it automatically respects a game that came or went
    // while the badge was up.
    publish();
}

void TrayStatus::publish()
{
    const QString text = composeTooltip();
    const Visual now = visual();
    if (text == m_tooltip && now == m_publishedVisual)
        return;
    m_tooltip = text;
    m_publishedVisual = now;
    emit changed();
}

QString TrayStatus::statusText() const
{
    switch (m_transient) {
    case Visual::ScreenshotSaved:
        return NativeText::get(
            //: Tray tooltip status line while the screenshot-saved badge is up.
            //% "Screenshot saved"
            QT_TRID_NOOP("gamehq.tray.status.state.screenshot_saved"), "Screenshot saved");
    case Visual::ClipSaved:
        return NativeText::get(
            //: Tray tooltip status line while the replay-clip-saved badge is up.
            //% "Clip saved"
            QT_TRID_NOOP("gamehq.tray.status.state.clip_saved"), "Clip saved");
    case Visual::CaptureProblem:
        return NativeText::get(
            //: Tray tooltip status line while the capture-problem warning is up.
            //% "Capture problem"
            QT_TRID_NOOP("gamehq.tray.status.state.capture_problem"), "Capture problem");
    case Visual::Idle:
    case Visual::Ready:
        break;
    }
    if (m_recognized)
        return m_pinned ? NativeText::get(
            //% "Pinned game"
            QT_TRID_NOOP("gamehq.tray.status.state.pinned"), "Pinned game")
            : m_background ? NativeText::get(
            //% "Background game"
            QT_TRID_NOOP("gamehq.tray.status.state.background"), "Background game")
            : NativeText::get(
            //: Tray tooltip status line when a supported game is in the foreground.
            //% "Game detected"
            QT_TRID_NOOP("gamehq.tray.status.state.game_detected"), "Game detected");
    return NativeText::get(
        //: Tray tooltip status line when no supported game is in the foreground.
        //% "Idle"
        QT_TRID_NOOP("gamehq.tray.status.state.idle"), "Idle");
}

QString TrayStatus::captureText() const
{
    switch (capture()) {
    case Capture::Paused:
        return NativeText::get(
            //% "Paused"
            QT_TRID_NOOP("gamehq.tray.status.capture.paused"), "Paused");
    case Capture::Ready:
        return NativeText::get(
            //: Tray tooltip capture line when the replay buffer is armed and recording.
            //% "Ready"
            QT_TRID_NOOP("gamehq.tray.status.capture.ready"), "Ready");
    case Capture::Starting:
        return NativeText::get(
            //: Tray tooltip capture line while the replay buffer is still coming up.
            //% "Starting"
            QT_TRID_NOOP("gamehq.tray.status.capture.starting"), "Starting");
    case Capture::ScreenshotsOnly:
        return NativeText::get(
            //: Tray tooltip capture line when the foreground game can be captured but no replay buffer is running (replay off, stopped or failed).
            //% "Screenshots only"
            QT_TRID_NOOP("gamehq.tray.status.capture.screenshots_only"), "Screenshots only");
    case Capture::Waiting:
        break;
    }
    return NativeText::get(
        //: Tray tooltip capture line when capture cannot run for the current window.
        //% "Waiting for a supported game"
        QT_TRID_NOOP("gamehq.tray.status.capture.waiting"), "Waiting for a supported game");
}

QString TrayStatus::composeTooltip() const
{
    // Brand and version are not translated: the product name is a proper noun
    // and the version is a number.
    const QString header = QString::fromUtf8(Brand::Name) + QLatin1Char(' ')
        + QStringLiteral(GAMEHQ_VERSION);
    const QString status = statusText();
    const QString cap = captureText();
    const QString noGame = NativeText::get(
        //: Tray tooltip game line when no supported game is in the foreground.
        //% "None detected"
        QT_TRID_NOOP("gamehq.tray.status.game.none"), "None detected");
    const QString gameLabel = m_recognized ? m_gameName : noGame;

    const auto assemble = [&](const QString& game) {
        return header + QLatin1Char('\n')
            + NativeText::get(
                  //: Tray tooltip line naming the foreground game; %1 is the game name.
                  //% "Game: %1"
                  QT_TRID_NOOP("gamehq.tray.status.tooltip.game"), "Game: %1").arg(game)
            + QLatin1Char('\n')
            + NativeText::get(
                  //: Tray tooltip line summarizing the current state; %1 is the state.
                  //% "Status: %1"
                  QT_TRID_NOOP("gamehq.tray.status.tooltip.status"), "Status: %1").arg(status)
            + QLatin1Char('\n')
            + NativeText::get(
                  //: Tray tooltip line describing capture readiness; %1 is the readiness.
                  //% "Capture: %1"
                  QT_TRID_NOOP("gamehq.tray.status.tooltip.capture"), "Capture: %1").arg(cap);
    };

    QString text = assemble(gameLabel);
    if (text.size() <= kTooltipMaxChars)
        return text;

    // Windows cuts a tray tooltip at 127 characters, so shorten the one part
    // that has no fixed meaning — the game name — and mark the cut. The room
    // left is measured against an empty game label, so this stays one string
    // build instead of a shrink loop.
    const int overhead = assemble(QString()).size();
    const int room = kTooltipMaxChars - overhead - 1;   // -1 for the ellipsis
    if (room > 0 && !gameLabel.isEmpty()) {
        text = assemble(gameLabel.left(room).trimmed()
                        + QChar(ushort(0x2026)));
    } else {
        text = assemble(noGame);
    }
    return text.size() <= kTooltipMaxChars ? text : text.left(kTooltipMaxChars);
}
