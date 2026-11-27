#include "tray/TrayIconRenderer.h"
#include "tray/TrayStatus.h"

#include <QIcon>
#include <QSignalSpy>
#include <QString>
#include <QTest>

namespace {

// ReplayBufferState::State values, spelled here so this test depends on the
// tray model only. The order is the enum's own:
// Stopped, Starting, Recording, Ready, Failed.
constexpr int kBufferStopped = 0;
constexpr int kBufferStarting = 1;
constexpr int kBufferRecording = 2;
constexpr int kBufferReady = 3;
constexpr int kBufferFailed = 4;

// QCOMPARE on a bare enum class needs a printable form; a name keeps the
// failure output readable without depending on metatype registration.
QString visualName(TrayStatus::Visual visual)
{
    switch (visual) {
    case TrayStatus::Visual::Idle:            return QStringLiteral("idle");
    case TrayStatus::Visual::Ready:           return QStringLiteral("ready");
    case TrayStatus::Visual::ScreenshotSaved: return QStringLiteral("screenshot");
    case TrayStatus::Visual::ClipSaved:       return QStringLiteral("clip");
    case TrayStatus::Visual::CaptureProblem:  return QStringLiteral("problem");
    }
    return QStringLiteral("?");
}

QString captureName(TrayStatus::Capture capture)
{
    switch (capture) {
    case TrayStatus::Capture::Waiting:  return QStringLiteral("waiting");
    case TrayStatus::Capture::Starting: return QStringLiteral("starting");
    case TrayStatus::Capture::Ready:    return QStringLiteral("ready");
    case TrayStatus::Capture::ScreenshotsOnly: return QStringLiteral("screenshots");
    }
    return QStringLiteral("?");
}

// Generous: the transient is 3 s, and the assertions only care that it ended.
constexpr int kTransientDeadline = TrayStatus::kTransientMs + 2500;

}  // namespace

// Issue #38: the tray status model turns capture facts into one visible state
// and one tooltip, and never becomes green just because a process exists.
class TestTrayStatus : public QObject
{
    Q_OBJECT

private slots:
    void idleBecomesReadyOnlyWithGameAndCapture();
    void screenshotBadgeReturnsToReady();
    void staleTransientTimeoutDoesNotClearNewerBadge();
    void gameLostDuringTransientFallsBackToIdle();
    void changedNotEmittedForIdenticalInputs();
    void captureProblemWarnsThenRestoresIdle();
    void tooltipStaysWithinWindowsLimit();
    void rendererCacheStaysBounded();
    void switchingGamesNeverKeepsTheOldName();
    void graceBufferDoesNotVouchForIneligibleWindow();
    void screenshotOnlyModeBadgesAndRestores();
    void pinnedTargetReportsPausedWithoutReadyBadge();
    void rapidScreenshotThenClipShowsNewest();
    void repeatedUpdatesStayBounded();
};

void TestTrayStatus::idleBecomesReadyOnlyWithGameAndCapture()
{
    TrayStatus status;
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QVERIFY(status.tooltip().contains(QStringLiteral("None detected")));
    QVERIFY(status.tooltip().contains(QStringLiteral("Capture: Waiting for a supported game")));

    // A foreground game that GameHQ would not capture: never green.
    status.setForegroundGame(QStringLiteral("explorer"), false, false);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));

    // A recognized game GameHQ may not capture (whitelist): never green.
    status.setForegroundGame(QStringLiteral("Elden Ring"), true, false);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QCOMPARE(captureName(status.capture()), QStringLiteral("waiting"));

    // A capturable game without a replay buffer: screenshots still work.
    status.setForegroundGame(QStringLiteral("Elden Ring"), true, true);
    QCOMPARE(visualName(status.visual()), QStringLiteral("ready"));
    QCOMPARE(captureName(status.capture()), QStringLiteral("screenshots"));
    QVERIFY(status.tooltip().contains(QStringLiteral("Capture: Screenshots only")));

    // Buffer coming up: reported as Starting, still not green.
    status.setReplayBufferState(kBufferStarting);
    QCOMPARE(captureName(status.capture()), QStringLiteral("starting"));
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));

    // Armed + qualifying game -> the only path to Ready.
    status.setReplayBufferState(kBufferRecording);
    QCOMPARE(visualName(status.visual()), QStringLiteral("ready"));
    QCOMPARE(captureName(status.capture()), QStringLiteral("ready"));
    const QString tip = status.tooltip();
    QVERIFY(tip.contains(QStringLiteral("Game: Elden Ring")));
    QVERIFY(tip.contains(QStringLiteral("Status: Game detected")));
    QVERIFY(tip.contains(QStringLiteral("Capture: Ready")));

    // A failed or stopped buffer leaves screenshots only, never "Ready".
    status.setReplayBufferState(kBufferFailed);
    QCOMPARE(captureName(status.capture()), QStringLiteral("screenshots"));
    QVERIFY(!status.tooltip().contains(QStringLiteral("Capture: Ready")));

    status.setReplayBufferState(kBufferStopped);
    QCOMPARE(captureName(status.capture()), QStringLiteral("screenshots"));

    // Losing the game always returns to the plain icon.
    status.clearForegroundGame();
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QVERIFY(status.tooltip().contains(QStringLiteral("None detected")));
}

void TestTrayStatus::screenshotBadgeReturnsToReady()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setReplayBufferState(kBufferReady);
    QCOMPARE(visualName(status.visual()), QStringLiteral("ready"));

    status.reportScreenshotSaved();
    QCOMPARE(visualName(status.visual()), QStringLiteral("screenshot"));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Screenshot saved")));

    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("ready"),
                              kTransientDeadline);
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Game detected")));
}

void TestTrayStatus::staleTransientTimeoutDoesNotClearNewerBadge()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setReplayBufferState(kBufferReady);

    status.reportScreenshotSaved();
    QCOMPARE(visualName(status.visual()), QStringLiteral("screenshot"));

    // A clip saved half a transient later: its badge owns the icon from now
    // on, and the screenshot's own deadline must not clear it.
    QTest::qWait(TrayStatus::kTransientMs / 2);
    status.reportClipSaved();
    QCOMPARE(visualName(status.visual()), QStringLiteral("clip"));

    // Past the screenshot's original deadline the red badge is still up.
    QTest::qWait(TrayStatus::kTransientMs / 2 + 300);
    QCOMPARE(visualName(status.visual()), QStringLiteral("clip"));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Clip saved")));

    // And the clip's own deadline does end it.
    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("ready"),
                              kTransientDeadline);
}

void TestTrayStatus::gameLostDuringTransientFallsBackToIdle()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setReplayBufferState(kBufferReady);
    QCOMPARE(visualName(status.visual()), QStringLiteral("ready"));

    status.reportClipSaved();
    // The game goes away while the badge is up.
    status.clearForegroundGame();
    status.setReplayBufferState(kBufferStopped);
    QCOMPARE(visualName(status.visual()), QStringLiteral("clip"));

    // When the transient ends the *current* underlying state is restored, not
    // the Ready state that was current when the badge started.
    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("idle"),
                              kTransientDeadline);
    QVERIFY(status.tooltip().contains(QStringLiteral("None detected")));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Idle")));
}

void TestTrayStatus::changedNotEmittedForIdenticalInputs()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setReplayBufferState(kBufferReady);

    QSignalSpy spy(&status, &TrayStatus::changed);
    QVERIFY(spy.isValid());

    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setForegroundGame(QStringLiteral("  Hades  "), true, true);
    status.setReplayBufferState(kBufferReady);
    status.retranslate();
    QCOMPARE(spy.count(), 0);

    status.setForegroundGame(QStringLiteral("Celeste"), true, true);
    QCOMPARE(spy.count(), 1);
}

void TestTrayStatus::captureProblemWarnsThenRestoresIdle()
{
    TrayStatus status;
    status.reportCaptureProblem();
    QCOMPARE(visualName(status.visual()), QStringLiteral("problem"));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Capture problem")));

    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("idle"),
                              kTransientDeadline);
}

void TestTrayStatus::tooltipStaysWithinWindowsLimit()
{
    TrayStatus status;
    status.setForegroundGame(QString(400, QLatin1Char('x')), true, true);
    status.setReplayBufferState(kBufferReady);

    const QString tip = status.tooltip();
    QVERIFY(tip.size() <= TrayStatus::kTooltipMaxChars);
    QVERIFY(tip.contains(QChar(ushort(0x2026))));   // ellipsis marks the cut
    // The fixed lines survive intact; only the game name is shortened.
    QVERIFY(tip.contains(QStringLiteral("Status: Game detected")));
    QVERIFY(tip.contains(QStringLiteral("Capture: Ready")));
}

void TestTrayStatus::rendererCacheStaysBounded()
{
    TrayIconRenderer renderer;
    const TrayStatus::Visual visuals[] = {
        TrayStatus::Visual::Idle,
        TrayStatus::Visual::Ready,
        TrayStatus::Visual::ScreenshotSaved,
        TrayStatus::Visual::ClipSaved,
        TrayStatus::Visual::CaptureProblem,
    };
    const TrayIconRenderer::Mode modes[] = {
        TrayIconRenderer::Mode::Badge,
        TrayIconRenderer::Mode::Replace,
        TrayIconRenderer::Mode::Static,
    };
    constexpr int kVisualCount = int(sizeof(visuals) / sizeof(visuals[0]));
    constexpr int kModeCount = int(sizeof(modes) / sizeof(modes[0]));

    // 1000 updates must warm the cache once and then never grow it.
    for (int i = 0; i < 1000; ++i) {
        const QIcon icon = renderer.icon(visuals[i % kVisualCount], modes[i % kModeCount]);
        QVERIFY(!icon.isNull());
        QVERIFY(renderer.cacheSize() <= TrayIconRenderer::kMaxCacheEntries);
    }

    const int warmed = renderer.cacheSize();
    QVERIFY(warmed <= kVisualCount * kModeCount);
    for (int i = 0; i < 500; ++i)
        (void)renderer.icon(visuals[i % kVisualCount], modes[i % kModeCount]);
    QCOMPARE(renderer.cacheSize(), warmed);
}

QTEST_MAIN(TestTrayStatus)
void TestTrayStatus::switchingGamesNeverKeepsTheOldName()
{
    TrayStatus status;
    status.setReplayBufferState(kBufferRecording);
    status.setForegroundGame(QStringLiteral("Overwatch"), true, true);
    QVERIFY(status.tooltip().contains(QStringLiteral("Game: Overwatch")));

    // Direct switch to a second detected game, buffer restarting for it.
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setReplayBufferState(kBufferStarting);
    QVERIFY(status.tooltip().contains(QStringLiteral("Game: Hades")));
    QVERIFY(!status.tooltip().contains(QStringLiteral("Overwatch")));
    QCOMPARE(captureName(status.capture()), QStringLiteral("starting"));

    // A badge up while the game changes: the tooltip already names the new
    // game, and the restored state after expiry belongs to it too.
    status.setReplayBufferState(kBufferRecording);
    status.reportScreenshotSaved();
    status.setForegroundGame(QStringLiteral("Celeste"), true, true);
    QVERIFY(status.tooltip().contains(QStringLiteral("Game: Celeste")));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Screenshot saved")));
    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("ready"), kTransientDeadline);
    QVERIFY(status.tooltip().contains(QStringLiteral("Game: Celeste")));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Game detected")));
    QVERIFY(!status.tooltip().contains(QStringLiteral("Hades")));
}

void TestTrayStatus::graceBufferDoesNotVouchForIneligibleWindow()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Overwatch"), true, true);
    status.setReplayBufferState(kBufferRecording);
    QCOMPARE(visualName(status.visual()), QStringLiteral("ready"));

    // A recognized game the whitelist excludes takes focus while the previous
    // buffer is still armed during the foreground grace.
    status.setForegroundGame(QStringLiteral("Solitaire"), true, false);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QCOMPARE(captureName(status.capture()), QStringLiteral("waiting"));
    QVERIFY(status.tooltip().contains(QStringLiteral("Game: Solitaire")));
    QVERIFY(!status.tooltip().contains(QStringLiteral("Capture: Ready")));
}

void TestTrayStatus::screenshotOnlyModeBadgesAndRestores()
{
    TrayStatus status;   // the replay buffer never starts (replay disabled)
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    QCOMPARE(visualName(status.visual()), QStringLiteral("ready"));
    status.reportScreenshotSaved();
    QCOMPARE(visualName(status.visual()), QStringLiteral("screenshot"));
    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("ready"), kTransientDeadline);
    QVERIFY(status.tooltip().contains(QStringLiteral("Capture: Screenshots only")));
}

void TestTrayStatus::pinnedTargetReportsPausedWithoutReadyBadge()
{
    TrayStatus status;
    status.setCaptureTarget(QStringLiteral("Game A"), true, true, true, true, true);
    status.setReplayBufferState(kBufferReady);
    QCOMPARE(status.gameName(), QStringLiteral("Game A"));
    QCOMPARE(status.capture(), TrayStatus::Capture::Paused);
    QCOMPARE(status.baseVisual(), TrayStatus::Visual::Idle);
    QVERIFY(status.tooltip().contains(QStringLiteral("Pinned game")));

    status.setCaptureTarget(QStringLiteral("Game B"), true, true, true, false, false);
    QCOMPARE(status.capture(), TrayStatus::Capture::Ready);
    QVERIFY(status.tooltip().contains(QStringLiteral("Background game")));
    QVERIFY(!status.tooltip().contains(QStringLiteral("Game A")));
}

void TestTrayStatus::rapidScreenshotThenClipShowsNewest()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Hades"), true, true);
    status.setReplayBufferState(kBufferRecording);

    status.reportScreenshotSaved();
    QTest::qWait(TrayStatus::kTransientMs / 2);
    status.reportClipSaved();
    QCOMPARE(visualName(status.visual()), QStringLiteral("clip"));
    // When the screenshot's original deadline passes, the newer clip badge stays.
    QTest::qWait(TrayStatus::kTransientMs / 2 + 400);
    QCOMPARE(visualName(status.visual()), QStringLiteral("clip"));
    QTRY_COMPARE_WITH_TIMEOUT(visualName(status.visual()), QStringLiteral("ready"), kTransientDeadline);

    // And the reverse order within one event-loop turn.
    status.reportClipSaved();
    status.reportScreenshotSaved();
    QCOMPARE(visualName(status.visual()), QStringLiteral("screenshot"));
    QVERIFY(status.tooltip().contains(QStringLiteral("Status: Screenshot saved")));
}

void TestTrayStatus::repeatedUpdatesStayBounded()
{
    TrayStatus status;
    TrayIconRenderer renderer;
    int changes = 0;
    connect(&status, &TrayStatus::changed, this, [&] { ++changes; });
    const auto childrenBefore = status.children().size();
    for (int i = 0; i < 5000; ++i) {
        status.setForegroundGame(i % 2 ? QStringLiteral("Hades") : QStringLiteral("Celeste"), true, true);
        status.setReplayBufferState(i % 3 == 0 ? kBufferStarting : kBufferRecording);
        if (i % 7 == 0)
            status.reportScreenshotSaved();
        if (i % 11 == 0)
            status.reportClipSaved();
        for (auto mode : { TrayIconRenderer::Mode::Badge, TrayIconRenderer::Mode::Replace,
                           TrayIconRenderer::Mode::Static })
            renderer.icon(status.visual(), mode);
    }
    // One timer, no per-update QObject allocation, a bounded icon cache.
    QCOMPARE(status.children().size(), childrenBefore);
    QVERIFY(renderer.cacheSize() <= TrayIconRenderer::kMaxCacheEntries);
    QVERIFY(changes > 0);
}

#include "tst_traystatus.moc"
