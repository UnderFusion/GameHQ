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
};

void TestTrayStatus::idleBecomesReadyOnlyWithGameAndCapture()
{
    TrayStatus status;
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QVERIFY(status.tooltip().contains(QStringLiteral("None detected")));
    QVERIFY(status.tooltip().contains(QStringLiteral("Capture: Waiting for a supported game")));

    // A foreground game that GameHQ would not capture: never green.
    status.setForegroundGame(QStringLiteral("explorer"), false);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));

    // A qualifying game without an armed buffer is still not Ready.
    status.setForegroundGame(QStringLiteral("Elden Ring"), true);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QCOMPARE(captureName(status.capture()), QStringLiteral("waiting"));

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

    // A failed buffer drops back out of Ready even while the game is running.
    status.setReplayBufferState(kBufferFailed);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
    QCOMPARE(captureName(status.capture()), QStringLiteral("waiting"));

    status.setReplayBufferState(kBufferStopped);
    QCOMPARE(visualName(status.visual()), QStringLiteral("idle"));
}

void TestTrayStatus::screenshotBadgeReturnsToReady()
{
    TrayStatus status;
    status.setForegroundGame(QStringLiteral("Hades"), true);
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
    status.setForegroundGame(QStringLiteral("Hades"), true);
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
    status.setForegroundGame(QStringLiteral("Hades"), true);
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
    status.setForegroundGame(QStringLiteral("Hades"), true);
    status.setReplayBufferState(kBufferReady);

    QSignalSpy spy(&status, &TrayStatus::changed);
    QVERIFY(spy.isValid());

    status.setForegroundGame(QStringLiteral("Hades"), true);
    status.setForegroundGame(QStringLiteral("  Hades  "), true);
    status.setReplayBufferState(kBufferReady);
    status.retranslate();
    QCOMPARE(spy.count(), 0);

    status.setForegroundGame(QStringLiteral("Celeste"), true);
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
    status.setForegroundGame(QString(400, QLatin1Char('x')), true);
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
#include "tst_traystatus.moc"
