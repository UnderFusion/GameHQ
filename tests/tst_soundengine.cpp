// Load-status reporting rules for the UI sounds (docs/sound-system.md).
//
// Pure logic: SoundLoadTracker deliberately knows nothing about Qt Multimedia,
// so these cases run without an audio device and without waiting on
// QSoundEffect's asynchronous loading. SoundEngine only maps
// QSoundEffect::Status onto SoundLoadTracker::Status and forwards the verdict.
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "sound/SoundLevels.h"
#include "sound/SoundLoadTracker.h"

#include <QTemporaryDir>

#include <QtTest>

using Status = SoundLoadTracker::Status;
using Report = SoundLoadTracker::Report;

class SoundEngineTest : public QObject
{
    Q_OBJECT
private slots:
    void loadingIsNotReported();
    void readyIsReportedOnce();
    void failureIsReportedOnceWithTheEventName();
    void statusAfterResolutionIsIgnored();
    void firstFailureRaisesTheOnlyWarning();
    void warningNamesEveryEventThatFailedSoFar();
    void aRunWithoutFailuresNeverWarns();

    void onlyCaptureFeedbackUsesTheCaptureLevel();
    void theCaptureSliderIsReadPerceptually();
    void theInterfaceSliderKeepsItsStraightMapping();
    void thePreviewPlaysTheRealCaptureSound();
    void anExistingConfigKeepsItsVolumeAndGainsTheCaptureDefault();
};

void SoundEngineTest::loadingIsNotReported()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("nav_tick"), Status::Loading), Report::Nothing);
    QCOMPARE(tracker.note(QStringLiteral("nav_tick"), Status::Loading), Report::Nothing);
    QVERIFY(!tracker.hasFailures());
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

void SoundEngineTest::readyIsReportedOnce()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Loading), Report::Nothing);
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Ready), Report::Ready);
    // QSoundEffect re-emits statusChanged; the log must not repeat.
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Ready), Report::Nothing);
    QVERIFY(!tracker.hasFailures());
}

void SoundEngineTest::failureIsReportedOnceWithTheEventName()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("screenshot"), Status::Error), Report::Failed);
    QCOMPARE(tracker.note(QStringLiteral("screenshot"), Status::Error), Report::Nothing);
    QVERIFY(tracker.hasFailures());
    QCOMPARE(tracker.failedEvents(), QStringList{QStringLiteral("screenshot")});

    // A source that never resolves is as unusable as a decode error.
    SoundLoadTracker nullTracker;
    QCOMPARE(nullTracker.note(QStringLiteral("error"), Status::Null), Report::Failed);
    QCOMPARE(nullTracker.failedEvents(), QStringList{QStringLiteral("error")});
}

void SoundEngineTest::statusAfterResolutionIsIgnored()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("favorite"), Status::Ready), Report::Ready);
    // Teardown drops the backend back to Null. The effect did load; that is
    // not a new failure and must never reach the user.
    QCOMPARE(tracker.note(QStringLiteral("favorite"), Status::Null), Report::Nothing);
    QVERIFY(!tracker.hasFailures());
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

void SoundEngineTest::firstFailureRaisesTheOnlyWarning()
{
    SoundLoadTracker tracker;
    QCOMPARE(tracker.note(QStringLiteral("nav_tick"), Status::Error), Report::Failed);
    QCOMPARE(tracker.takeFailureWarning(), QStringLiteral("nav_tick"));

    // Every later failure is still logged, but the visible warning is spent.
    QCOMPARE(tracker.note(QStringLiteral("confirm"), Status::Error), Report::Failed);
    QVERIFY(tracker.takeFailureWarning().isEmpty());
    QCOMPARE(tracker.failedEvents(),
             QStringList({QStringLiteral("nav_tick"), QStringLiteral("confirm")}));
}

void SoundEngineTest::warningNamesEveryEventThatFailedSoFar()
{
    SoundLoadTracker tracker;
    // Two effects fail before anyone asks: the single warning covers both.
    QCOMPARE(tracker.note(QStringLiteral("screenshot"), Status::Error), Report::Failed);
    QCOMPARE(tracker.note(QStringLiteral("replay_saved"), Status::Null), Report::Failed);
    QCOMPARE(tracker.takeFailureWarning(), QStringLiteral("screenshot, replay_saved"));
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

void SoundEngineTest::aRunWithoutFailuresNeverWarns()
{
    SoundLoadTracker tracker;
    for (const QString& event : {QStringLiteral("nav_tick"), QStringLiteral("confirm"),
                                 QStringLiteral("screenshot")}) {
        QCOMPARE(tracker.note(event, Status::Loading), Report::Nothing);
        QCOMPARE(tracker.note(event, Status::Ready), Report::Ready);
    }
    QVERIFY(!tracker.hasFailures());
    QVERIFY(tracker.takeFailureWarning().isEmpty());
}

// The slider is only honest if it moves the sounds it claims to move.
void SoundEngineTest::onlyCaptureFeedbackUsesTheCaptureLevel()
{
    using Level = SoundLevels::Level;
    for (const QString& event : {QStringLiteral("screenshot"), QStringLiteral("replay_saved"),
                                 QStringLiteral("capture_accepted")})
        QCOMPARE(SoundLevels::levelFor(event), Level::Capture);

    // "error" also reports settings failures such as a refused portable
    // import, so it stays where it has always been.
    for (const QString& event : {QStringLiteral("nav_tick"), QStringLiteral("confirm"),
                                 QStringLiteral("favorite"), QStringLiteral("overlay_open"),
                                 QStringLiteral("overlay_close"), QStringLiteral("error"),
                                 QStringLiteral("not_an_event")})
        QCOMPARE(SoundLevels::levelFor(event), Level::Interface);
}

// A straight percentage slider spends most of its travel in a range the ear
// barely separates. Half-way must sound about half as loud, which means well
// under half the amplitude.
void SoundEngineTest::theCaptureSliderIsReadPerceptually()
{
    QCOMPARE(SoundLevels::perceptualAmplitude(0), 0.0);
    QVERIFY(qFuzzyCompare(SoundLevels::perceptualAmplitude(100) + 1.0, 2.0));

    const qreal low = SoundLevels::perceptualAmplitude(10);
    const qreal mid = SoundLevels::perceptualAmplitude(50);
    QVERIFY(low > 0.0);
    QVERIFY(low < 0.10);
    QVERIFY(mid > 0.0);
    QVERIFY(mid < 0.50);

    // Nothing may go backwards: a slider that dips as it is raised is a bug
    // the user reads as broken audio.
    qreal previous = -1.0;
    for (int percent = 0; percent <= 100; percent += 5) {
        const qreal amplitude = SoundLevels::perceptualAmplitude(percent);
        QVERIFY(amplitude >= previous);
        QVERIFY(amplitude >= 0.0 && amplitude <= 1.0);
        previous = amplitude;
    }

    // Out-of-range values are clamped, not wrapped.
    QCOMPARE(SoundLevels::perceptualAmplitude(-40), 0.0);
    QCOMPARE(SoundLevels::perceptualAmplitude(400), SoundLevels::perceptualAmplitude(100));
}

// Existing users set their UI volume against the old straight mapping, so it
// must still mean exactly what it meant before.
void SoundEngineTest::theInterfaceSliderKeepsItsStraightMapping()
{
    QCOMPARE(SoundLevels::linearAmplitude(0), 0.0);
    QCOMPARE(SoundLevels::linearAmplitude(80), 0.8);
    QCOMPARE(SoundLevels::linearAmplitude(100), 1.0);
    QCOMPARE(SoundLevels::linearAmplitude(-5), 0.0);
    QCOMPARE(SoundLevels::linearAmplitude(150), 1.0);
}

// Previewing a test-only asset would prove nothing about the slider.
void SoundEngineTest::thePreviewPlaysTheRealCaptureSound()
{
    QCOMPARE(SoundLevels::previewEvent(), QStringLiteral("screenshot"));
    QCOMPARE(SoundLevels::levelFor(SoundLevels::previewEvent()), SoundLevels::Level::Capture);
}

// Upgrading must not move a volume the user already chose, and must not leave
// the new level unset.
void SoundEngineTest::anExistingConfigKeepsItsVolumeAndGainsTheCaptureDefault()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("config.json"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("{\"sounds.enabled\": true, \"sounds.volume\": 55}");
    }

    ConfigManager config(path);
    QVERIFY(config.load());
    QCOMPARE(config.value(ConfigKeys::SoundsVolume, 80).toInt(), 55);
    QCOMPARE(config.value(ConfigKeys::SoundsCaptureVolume, 0).toInt(), 100);
    QCOMPARE(config.defaultValue(ConfigKeys::SoundsCaptureVolume).toInt(), 100);

    // The capture level is a real, settable key, not a constant.
    config.setValue(ConfigKeys::SoundsCaptureVolume, 40);
    QVERIFY(config.save());
    ConfigManager reloaded(path);
    QVERIFY(reloaded.load());
    QCOMPARE(reloaded.value(ConfigKeys::SoundsCaptureVolume, 100).toInt(), 40);
    QCOMPARE(reloaded.value(ConfigKeys::SoundsVolume, 80).toInt(), 55);
}

QTEST_APPLESS_MAIN(SoundEngineTest)
#include "tst_soundengine.moc"
