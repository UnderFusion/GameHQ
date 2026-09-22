#include "capture/ReplayBufferState.h"
#include "capture/ReplayBufferOwners.h"

#include <QMetaEnum>
#include <QSignalSpy>
#include <QtTest>

// Exercise the state authority used by the real service, with worker facts
// supplied deterministically. WGC startup and real-game observation belong to
// the p2 group milestone, not this device-independent test.
class TestFramePumpService : public QObject
{
    Q_OBJECT
private slots:
    void manualColdArmBecomesReadyAndSaves()
    {
        ReplayBufferState buffer;
        ReplayBufferOwners owners;
        const auto token = owners.acquireManual(0, 90);
        const auto generation = buffer.requestStart("Manual game");
        QVERIFY(owners.needsSession());
        QVERIFY(!buffer.canSave());
        QVERIFY(!owners.savePending()); // cold rejection does not release or queue a save
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Recording));
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(buffer.canSave());
        QCOMPARE(owners.acquireManual(7000, 90), token);
        QVERIFY(owners.beginSave(token));
        QVERIFY(owners.savePending());
        QVERIFY(!owners.expireIdle(1000000)); // export cannot expire mid-flight
        QCOMPARE(owners.acquireManual(1000001, 90), quint64(0)); // repeated save is rejected
        QVERIFY(!owners.finishSave(token + 1));
        QVERIFY(owners.finishSave(token));
        QVERIFY(!owners.needsSession());
    }

    void manualOwnershipSurvivesSettingsRestartAndStaleCallbacks()
    {
        ReplayBufferState buffer;
        ReplayBufferOwners owners;
        owners.setAuto(true);
        const auto token = owners.acquireManual(0, 90);
        const auto oldGeneration = buffer.requestStart("Manual game");
        QVERIFY(buffer.confirm(oldGeneration, ReplayBufferState::Recording));
        QVERIFY(buffer.confirm(oldGeneration, ReplayBufferState::Ready));
        QVERIFY(owners.beginSave(token));
        owners.setAuto(false); // settings disable always-on while manually owned
        QVERIFY(owners.needsSession());
        const auto newGeneration = buffer.requestStart("Manual game");
        QVERIFY(!buffer.confirm(oldGeneration, ReplayBufferState::Failed));
        QCOMPARE(owners.manualToken(), token);
        QVERIFY(buffer.confirm(newGeneration, ReplayBufferState::Recording));
        QVERIFY(owners.finishSave(token)); // original export may finish after pipe replacement
        QVERIFY(!owners.needsSession());
        const auto newToken = owners.acquireManual(5000, 90);
        QVERIFY(newToken != token);
        QVERIFY(!owners.finishSave(token));
        QCOMPARE(owners.manualToken(), newToken);
    }

    void hdrAndManualOwnersReleaseIndependently()
    {
        ReplayBufferOwners owners;
        const auto manual = owners.acquireManual(0, 90);
        const auto hdr = owners.acquireHdr();
        QVERIFY(hdr != 0);
        QCOMPARE(owners.acquireHdr(), quint64(0));
        QVERIFY(owners.finishHdr(hdr));
        QVERIFY(owners.owns(ReplayBufferOwners::ManualSave));
        const auto nextHdr = owners.acquireHdr();
        QVERIFY(!owners.finishHdr(hdr));
        QVERIFY(owners.beginSave(manual));
        QVERIFY(owners.finishSave(manual));
        QVERIFY(owners.owns(ReplayBufferOwners::HdrScreenshot));
        QVERIFY(owners.needsSession());
        QVERIFY(owners.finishHdr(nextHdr));
        QVERIFY(!owners.needsSession());
    }

    void saveFailureReleasesManualOwnershipButKeepsAuto()
    {
        ReplayBufferState buffer;
        ReplayBufferOwners owners;
        owners.setAuto(true);
        const auto token = owners.acquireManual(0, 90);
        const auto generation = buffer.requestStart("Manual game");
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Recording));
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(owners.beginSave(token));
        // A recording failure does not release an export still using its lease.
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Failed, "recorder failed"));
        QVERIFY(owners.savePending());
        QVERIFY(owners.finishSave(token)); // terminal export failure acknowledgment
        QVERIFY(!owners.owns(ReplayBufferOwners::ManualSave));
        QVERIFY(owners.owns(ReplayBufferOwners::Auto));
        QVERIFY(owners.needsSession());
        owners.setAuto(false);
        QVERIFY(!owners.needsSession());
    }

    void coldArmExpiresWithoutStoppingOtherOwners()
    {
        ReplayBufferOwners owners;
        owners.acquireManual(1000, 90);
        QVERIFY(!owners.expireIdle(90999));
        QVERIFY(owners.expireIdle(91000));
        QVERIFY(!owners.needsSession());
        owners.setAuto(true);
        owners.acquireManual(100000, 90);
        owners.acquireManual(180000, 90); // an explicit retry refreshes the idle deadline
        QVERIFY(!owners.expireIdle(190000));
        QVERIFY(owners.expireIdle(270000));
        QVERIFY(owners.owns(ReplayBufferOwners::Auto));
        owners.setAuto(false);
        owners.acquireManual(0, -1);
        QVERIFY(!owners.expireIdle(9999));
        QVERIFY(owners.expireIdle(10000));
        owners.acquireManual(0, 1000000);
        QVERIFY(owners.expireIdle(600000));
    }

    void onlyConfirmedStartupReportsRecording()
    {
        ReplayBufferState buffer;
        QSignalSpy recording(&buffer, &ReplayBufferState::recordingStateChanged);
        QSignalSpy states(&buffer, &ReplayBufferState::stateChanged);
        const auto generation = buffer.requestStart("Game A");
        QCOMPARE(buffer.state(), ReplayBufferState::Starting);
        QVERIFY(buffer.startRequested());
        QVERIFY(!buffer.canSave());
        QCOMPARE(recording.size(), 0); // a queued start is not recording
        QVERIFY(!buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Recording));
        QCOMPARE(recording.size(), 1);
        QCOMPARE(recording.at(0).at(0).toBool(), true);
        QCOMPARE(recording.at(0).at(1).toString(), QString("Game A"));
        QVERIFY(buffer.canSave()); // worker may finalize the first partial segment
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(buffer.canSave());
        QCOMPARE(states.size(), 3);
        QCOMPARE(recording.size(), 1); // legacy bool remains true when Ready
    }

    void startupFailureIsTerminalForThatGeneration()
    {
        ReplayBufferState buffer;
        QSignalSpy failed(&buffer, &ReplayBufferState::failed);
        QSignalSpy recording(&buffer, &ReplayBufferState::recordingStateChanged);
        const auto generation = buffer.requestStart("Game A");
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Failed, "StartCapture failed"));
        QCOMPARE(buffer.state(), ReplayBufferState::Failed);
        QCOMPARE(failed.size(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QString("StartCapture failed"));
        QCOMPARE(recording.size(), 0);
        QVERIFY(!buffer.startRequested());
        QVERIFY(!buffer.canSave());
        QVERIFY(!buffer.confirm(generation, ReplayBufferState::Recording));
        QVERIFY(!buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(!buffer.confirm(generation, ReplayBufferState::Failed));
        QCOMPARE(failed.size(), 1);
        const auto retry = buffer.requestStart("Game A");
        QVERIFY(retry > generation);
        QVERIFY(buffer.confirm(retry, ReplayBufferState::Recording));
    }

    void staleWorkerFactsCannotOverwriteNewerRequests()
    {
        ReplayBufferState buffer;
        QSignalSpy failed(&buffer, &ReplayBufferState::failed);
        const auto first = buffer.requestStart("Game A");
        QVERIFY(buffer.confirm(first, ReplayBufferState::Recording));
        const auto second = buffer.requestStart("Game B");
        QVERIFY(!buffer.confirm(first, ReplayBufferState::Ready));
        QVERIFY(!buffer.confirm(first, ReplayBufferState::Failed, "old failure"));
        QCOMPARE(buffer.state(), ReplayBufferState::Starting);
        QCOMPARE(buffer.gameName(), QString("Game B"));
        QCOMPARE(failed.size(), 0);
        QVERIFY(buffer.confirm(second, ReplayBufferState::Recording));
        QVERIFY(buffer.confirm(second, ReplayBufferState::Ready));
        const auto stopped = buffer.requestStop();
        QVERIFY(stopped > second);
        QVERIFY(!buffer.confirm(second, ReplayBufferState::Failed));
        QVERIFY(!buffer.confirm(second, ReplayBufferState::Recording));
        QVERIFY(!buffer.confirm(second, ReplayBufferState::Ready));
        QCOMPARE(buffer.state(), ReplayBufferState::Stopped);
        QVERIFY(buffer.gameName().isEmpty());
        QCOMPARE(failed.size(), 0);
    }

    void activeRecordingAdmitsPartialSaveButInactiveStatesReject()
    {
        ReplayBufferState buffer;
        QVERIFY(!buffer.canSave());
        QVERIFY(!ReplayBufferState::saveRejection(buffer.state()).isEmpty());
        const auto generation = buffer.requestStart("Game A");
        QVERIFY(!buffer.canSave());
        QVERIFY(!ReplayBufferState::saveRejection(buffer.state()).isEmpty());
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Recording));
        QVERIFY(buffer.canSave()); // no closed segment or full duration required
        QVERIFY(ReplayBufferState::canSave(buffer.state())); // worker guard
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(buffer.canSave());
        QVERIFY(ReplayBufferState::saveRejection(buffer.state()).isEmpty());
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Recording));
        QVERIFY(buffer.canSave()); // worker can finalize current in-flight video
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Ready));
        QVERIFY(buffer.confirm(generation, ReplayBufferState::Failed, "recorder stopped"));
        QVERIFY(!buffer.canSave());
        QVERIFY(!ReplayBufferState::saveRejection(buffer.state()).isEmpty());
    }

    void stateEnumIsAvailableToTheUiMetaObject()
    {
        const QMetaEnum states = QMetaEnum::fromType<ReplayBufferState::State>();
        QVERIFY(states.isValid());
        QCOMPARE(states.keyCount(), 5);
        QCOMPARE(states.keyToValue("Ready"), int(ReplayBufferState::Ready));
    }
};

QTEST_GUILESS_MAIN(TestFramePumpService)
#include "tst_framepumpservice.moc"
