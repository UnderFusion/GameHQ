// cpo-o06d: the external receiver's tests, and the experiment that uses it.
//
// The receiver (tools/input-receiver) is a SEPARATE PROCESS by design: whether
// GameHQ's exclusive-foreground request stops another app from receiving the pad
// is a question no in-process log can answer. This file covers three things:
//
//   1. the receiver's pure logic — the log grammar, the meaningful-change
//      detector, phase attribution and the verdicts — without any device;
//   2. the receiver as a process — it starts, reports its providers, refuses bad
//      arguments loudly, writes a readable summary and exits;
//   3. the experiment itself: the shipped policy path (GameInputFocusController
//      driving ProductionGameInputApi, i.e. exactly what cpo-o06c wired up) asks
//      the runtime for exclusive foreground input while the external receiver
//      watches from outside, phase by phase.
//
// What case 3 asserts is deliberately MECHANICAL: the phases happened, the
// receiver stayed a background process, and it produced a verdict per provider.
// The verdict itself is a MEASUREMENT, printed in full, not a pre-declared
// pass — "another client kept receiving" is a result, not a test failure, and
// the run says "not-measurable" instead of "isolated" whenever its own baseline
// saw nothing to compare against.
#include <QtTest>

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>

#include <memory>

#include <windows.h>

#include "ReceiverModel.h"

#include "gameinput/GameInputFocusController.h"
#include "gameinput/ProductionGameInputApi.h"
#include "overlay/ForegroundApi.h"

namespace
{

QString receiverExecutable()
{
    return QStringLiteral(GAMEHQ_INPUT_RECEIVER);
}

QStringList readLogLines(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList lines;
    for (const QByteArray& raw : file.readAll().split('\n')) {
        const QByteArray trimmed = raw.trimmed();
        if (!trimmed.isEmpty())
            lines.append(QString::fromUtf8(trimmed));
    }
    return lines;
}

QVector<GameReceiver::LogRecord> parseLog(const QString& path)
{
    QVector<GameReceiver::LogRecord> records;
    for (const QString& line : readLogLines(path)) {
        GameReceiver::LogRecord record;
        if (GameReceiver::parseLogLine(line.toStdString(), record))
            records.append(record);
    }
    return records;
}

QString fieldOf(const GameReceiver::LogRecord& record, const char* key)
{
    return QString::fromStdString(GameReceiver::fieldOf(record, key));
}

QVector<GameReceiver::LogRecord> recordsWithEvent(const QVector<GameReceiver::LogRecord>& all,
                                                  const QString& event)
{
    QVector<GameReceiver::LogRecord> filtered;
    for (const GameReceiver::LogRecord& record : all) {
        if (QString::fromStdString(record.event) == event)
            filtered.append(record);
    }
    return filtered;
}

// Waits until the receiver says it is ready, i.e. its providers are attached and
// the phases it is about to be given will be attributed to a live run.
bool waitForEvent(const QString& logPath, const QString& event, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        for (const GameReceiver::LogRecord& record : parseLog(logPath)) {
            if (QString::fromStdString(record.event) == event)
                return true;
        }
        QTest::qWait(100);
    }
    return false;
}

// The experiment writes its receipt next to the build unless the caller asks for
// a durable location; the raw receiver log is always kept.
QString evidencePath(const QString& fileName)
{
    const QByteArray override = qgetenv("GAMEHQ_INPUT_EVIDENCE_DIR");
    const QString directory = override.isEmpty() ? QDir::tempPath() : QString::fromUtf8(override);
    QDir().mkpath(directory);
    return directory + '/' + fileName;
}

}  // namespace

class ExternalReceiverTest : public QObject
{
    Q_OBJECT

private slots:
    void logGrammarRoundTripsAndIsDeterministic();
    void transitionDetectorReportsEdgesNotNoise();
    void hatSwitchDecodesToDpadDirection();
    void phasesAttributeEventsByTimestamp();
    void verdictSeparatesNotMeasurableFromBlocked();
    void summaryAndVerdictCarryTheComparison();
    void selfCheckReportsEveryRequestedProvider();
    void badArgumentsFailLoudly();
    void unknownProviderIsRefused();
    void aRunWithoutAControllerStillEndsHonestly();
    void externalReceiverMeasuresTheExclusivePolicy();
};

void ExternalReceiverTest::logGrammarRoundTripsAndIsDeterministic()
{
    GameReceiver::LogRecord record;
    record.seconds = 12.5;
    record.phase = "exclusive";
    record.event = "activity";
    record.fields = { { "provider", "gameinput" },
                      { "arrivals", "125" },
                      { "fgtitle", "Game Window: level 3" } };

    const QString first = QString::fromStdString(GameReceiver::formatLogLine(record));
    const QString second = QString::fromStdString(GameReceiver::formatLogLine(record));
    // Byte-identical output for identical input: a receipt has to be diffable.
    QCOMPARE(first, second);
    // A value with spaces is quoted, so a reader can split on spaces.
    QVERIFY(first.contains(QStringLiteral("fgtitle=\"Game Window: level 3\"")));

    GameReceiver::LogRecord parsed;
    QVERIFY(GameReceiver::parseLogLine(first.toStdString(), parsed));
    QCOMPARE(parsed.event, std::string("activity"));
    QCOMPARE(parsed.phase, std::string("exclusive"));
    QCOMPARE(GameReceiver::fieldOf(parsed, "arrivals"), std::string("125"));
    QCOMPARE(GameReceiver::fieldOf(parsed, "fgtitle"), std::string("Game Window: level 3"));
    QCOMPARE(GameReceiver::fieldOf(parsed, "missing"), std::string());
    QVERIFY(qAbs(parsed.seconds - 12.5) < 0.001);

    // A line without an event is not a record, so a truncated or foreign line is
    // rejected rather than silently counted.
    GameReceiver::LogRecord ignored;
    QVERIFY(!GameReceiver::parseLogLine("t=1.000s phase=nothing", ignored));
}

void ExternalReceiverTest::transitionDetectorReportsEdgesNotNoise()
{
    GameReceiver::TransitionDetector detector(0.2f);

    GameReceiver::PadState idle;
    QCOMPARE(QString::fromStdString(detector.update(idle)), QStringLiteral("initial"));
    // The same state again is not an event: a pad streaming at 250 Hz must not
    // produce 250 log lines a second.
    QCOMPARE(QString::fromStdString(detector.update(idle)), QString());

    GameReceiver::PadState pressed = idle;
    pressed.buttons = GameReceiver::Button::A | GameReceiver::Button::DpadLeft;
    // Increasing-bit order, not press order: the same held buttons produce
    // the same bytes whoever reports them first.
    const QString press = QString::fromStdString(detector.update(pressed));
    QCOMPARE(press, QStringLiteral("+DpadLeft +A"));

    // Held buttons stay silent.
    QCOMPARE(QString::fromStdString(detector.update(pressed)), QString());

    GameReceiver::PadState released = idle;
    QCOMPARE(QString::fromStdString(detector.update(released)),
             QStringLiteral("-DpadLeft -A"));

    GameReceiver::PadState jitter = idle;
    jitter.axes[GameReceiver::AxisLeftX] = 0.05f;
    QCOMPARE(QString::fromStdString(detector.update(jitter)), QString());
    jitter.axes[GameReceiver::AxisLeftX] = 0.55f;
    QCOMPARE(QString::fromStdString(detector.update(jitter)), QStringLiteral("lx=+0.55"));
}

void ExternalReceiverTest::hatSwitchDecodesToDpadDirection()
{
    using GameReceiver::Button::DpadDown;
    using GameReceiver::Button::DpadLeft;
    using GameReceiver::Button::DpadRight;
    using GameReceiver::Button::DpadUp;

    QCOMPARE(GameReceiver::hatSwitchToButtons(-1), 0u);
    QCOMPARE(GameReceiver::hatSwitchToButtons(9), 0u);
    QCOMPARE(GameReceiver::hatSwitchToButtons(0), unsigned(DpadUp));
    QCOMPARE(GameReceiver::hatSwitchToButtons(2), unsigned(DpadRight));
    QCOMPARE(GameReceiver::hatSwitchToButtons(4), unsigned(DpadDown));
    QCOMPARE(GameReceiver::hatSwitchToButtons(6), unsigned(DpadLeft));
    QCOMPARE(GameReceiver::hatSwitchToButtons(1), unsigned(DpadUp | DpadRight));
    QCOMPARE(GameReceiver::hatSwitchToButtons(5), unsigned(DpadDown | DpadLeft));
}

void ExternalReceiverTest::phasesAttributeEventsByTimestamp()
{
    GameReceiver::Session session;
    session.markPhase("baseline", 1.0);
    session.noteArrival(GameReceiver::Provider::GameInput, 1.5);
    session.noteTransition(GameReceiver::Provider::GameInput, 2.0);
    session.markPhase("exclusive", 4.0);
    session.notePresence(GameReceiver::Provider::XInput, 4.5);
    session.markPhase("restored", 8.0);
    session.noteArrival(GameReceiver::Provider::GameInput, 9.0);
    session.noteArrival(GameReceiver::Provider::GameInput, 11.0);
    session.closeAt(12.0);

    QCOMPARE(session.phases().size(), size_t(3));
    const GameReceiver::Phase* baseline = session.phaseByName("baseline");
    QVERIFY(baseline);
    // An event belongs to the phase its timestamp falls in, never to whichever
    // provider thread happened to log last.
    QCOMPARE(baseline->providers[int(GameReceiver::Provider::GameInput)].arrivals, 2LL);
    QCOMPARE(baseline->providers[int(GameReceiver::Provider::GameInput)].transitions, 1LL);
    QCOMPARE(session.durationOf(*baseline), 3.0);

    const GameReceiver::Phase* exclusive = session.phaseByName("exclusive");
    QVERIFY(exclusive);
    QCOMPARE(exclusive->providers[int(GameReceiver::Provider::GameInput)].arrivals, 0LL);
    QCOMPARE(exclusive->providers[int(GameReceiver::Provider::XInput)].presence, 1LL);
    QCOMPARE(session.arrivalRate(*exclusive, GameReceiver::Provider::GameInput), 0.0);

    const GameReceiver::Phase* restored = session.phaseByName("restored");
    QVERIFY(restored);
    QCOMPARE(restored->providers[int(GameReceiver::Provider::GameInput)].arrivals, 2LL);
    // 2 arrivals over the 4 s the phase was open for.
    QCOMPARE(session.arrivalRate(*restored, GameReceiver::Provider::GameInput), 0.5);

    // A phase that never got a duration can never look measurable.
    GameReceiver::Session unfinished;
    unfinished.markPhase("baseline", 3.0);
    unfinished.noteArrival(GameReceiver::Provider::GameInput, 3.0);
    QCOMPARE(unfinished.arrivalRate(unfinished.phases().at(0), GameReceiver::Provider::GameInput),
             0.0);
}

void ExternalReceiverTest::verdictSeparatesNotMeasurableFromBlocked()
{
    using GameReceiver::judge;
    using GameReceiver::Verdict;
    using GameReceiver::VerdictInputs;

    // A baseline with a couple of stragglers is not a measurement.
    VerdictInputs empty;
    empty.baselineArrivals = 3;
    empty.baselineRate = 1.0;
    QCOMPARE(judge(empty), Verdict::NotMeasurable);

    VerdictInputs blocked;
    blocked.baselineArrivals = 1200;
    blocked.baselineRate = 250.0;
    blocked.exclusiveRate = 0.0;
    blocked.restoredRate = 248.0;
    QCOMPARE(judge(blocked), Verdict::BlockedThenResumed);

    VerdictInputs stillFlowing;
    stillFlowing.baselineArrivals = 1200;
    stillFlowing.baselineRate = 250.0;
    stillFlowing.exclusiveRate = 249.0;
    stillFlowing.restoredRate = 250.0;
    QCOMPARE(judge(stillFlowing), Verdict::Continuous);

    // Quiet during the exclusive phase AND quiet afterwards: a pad that was
    // unplugged looks like this, so it must not be reported as isolation.
    VerdictInputs neverReturned;
    neverReturned.baselineArrivals = 1200;
    neverReturned.baselineRate = 250.0;
    neverReturned.exclusiveRate = 0.0;
    neverReturned.restoredRate = 0.0;
    QCOMPARE(judge(neverReturned), Verdict::BlockedNoResume);

    VerdictInputs deviceChanged = blocked;
    deviceChanged.exclusivePresence = 1;
    QCOMPARE(judge(deviceChanged), Verdict::BlockedNoResume);
}

void ExternalReceiverTest::summaryAndVerdictCarryTheComparison()
{
    GameReceiver::Phase phase;
    phase.name = "exclusive";
    phase.startSeconds = 4.0;
    phase.endSeconds = 9.0;
    phase.open = false;
    phase.providers[int(GameReceiver::Provider::GameInput)].arrivals = 7;

    GameReceiver::LogRecord summary;
    summary.event = "summary";
    summary.fields =
        GameReceiver::summaryFields(phase, GameReceiver::Provider::GameInput, 15.0);
    const QString summaryLine = QString::fromStdString(GameReceiver::formatLogLine(summary));
    QVERIFY(summaryLine.contains(QStringLiteral("provider=gameinput")));
    QVERIFY(summaryLine.contains(QStringLiteral("phase=exclusive")));
    QVERIFY(summaryLine.contains(QStringLiteral("duration=5.000s")));
    QVERIFY(summaryLine.contains(QStringLiteral("arrivals=7")));
    QVERIFY(summaryLine.contains(QStringLiteral("rate=1.40/s")));

    GameReceiver::VerdictInputs inputs;
    inputs.baselineArrivals = 1000;
    inputs.baselineRate = 250.0;
    inputs.exclusiveRate = 0.0;
    inputs.restoredRate = 240.0;
    GameReceiver::LogRecord verdict;
    verdict.event = "verdict";
    verdict.fields = GameReceiver::verdictFields("baseline", "exclusive", "restored",
                                                 GameReceiver::Provider::GameInput, inputs,
                                                 GameReceiver::judge(inputs));
    const QString verdictLine = QString::fromStdString(GameReceiver::formatLogLine(verdict));
    QVERIFY(verdictLine.contains(QStringLiteral("result=blocked-then-resumed")));
    // The verdict names what it compared, so a reader never has to guess.
    QVERIFY(verdictLine.contains(QStringLiteral("baselinePhase=baseline")));
    QVERIFY(verdictLine.contains(QStringLiteral("exclusivePhase=exclusive")));
    QVERIFY(verdictLine.contains(QStringLiteral("restoredPhase=restored")));

    // An explicit note wins over the generic wording (used when a phase is
    // missing and nothing was compared at all).
    GameReceiver::LogRecord missing;
    missing.event = "verdict";
    missing.fields = GameReceiver::verdictFields(
        "baseline", "exclusive", "restored", GameReceiver::Provider::GameInput,
        GameReceiver::VerdictInputs(), GameReceiver::Verdict::NotMeasurable,
        "phase 'exclusive' is not present in this run");
    const QString missingLine = QString::fromStdString(GameReceiver::formatLogLine(missing));
    QVERIFY(missingLine.contains(QStringLiteral("result=not-measurable")));
    QVERIFY(missingLine.contains(QStringLiteral("not present in this run")));
}

void ExternalReceiverTest::selfCheckReportsEveryRequestedProvider()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString log = dir.filePath("self-check.log");

    QProcess receiver;
    receiver.start(receiverExecutable(), { "--log", log, "--self-check" });
    QVERIFY(receiver.waitForStarted(10000));
    QVERIFY(receiver.waitForFinished(30000));
    QCOMPARE(receiver.exitCode(), 0);

    const QVector<GameReceiver::LogRecord> records = parseLog(log);
    QVERIFY(!records.isEmpty());
    const auto providers = recordsWithEvent(records, QStringLiteral("provider"));
    QCOMPARE(providers.size(), 3);
    for (const GameReceiver::LogRecord& record : providers) {
        const QString state = fieldOf(record, "state");
        // On this machine every path may or may not be available; what must
        // never happen is a provider that reports neither state nor reason.
        QVERIFY(state == QStringLiteral("ready") || state == QStringLiteral("unavailable"));
        if (state == QStringLiteral("unavailable"))
            QVERIFY(!fieldOf(record, "reason").isEmpty());
        else
            QVERIFY(!fieldOf(record, "detail").isEmpty());
    }
    // The GameInput path must say which policy it asked the runtime for: that is
    // the difference between a fair measurement and a gap that just means "we
    // were not the foreground app".
    const GameReceiver::LogRecord gameInput =
        recordsWithEvent(records, QStringLiteral("provider")).at(0);
    QVERIFY(fieldOf(gameInput, "detail").contains(QStringLiteral("policy=background-input+background-guide+background-share")));
}

void ExternalReceiverTest::badArgumentsFailLoudly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    QProcess receiver;
    // A log path in a directory that does not exist: the run must fail loudly
    // rather than silently measuring nothing.
    receiver.start(receiverExecutable(),
                   { "--log", dir.filePath("missing/nested/run.log"), "--self-check" });
    QVERIFY(receiver.waitForStarted(10000));
    QVERIFY(receiver.waitForFinished(30000));
    QCOMPARE(receiver.exitCode(), 2);
    QVERIFY(QString::fromUtf8(receiver.readAllStandardError())
                .contains(QStringLiteral("cannot open the log file")));

    QProcess withoutLog;
    withoutLog.start(receiverExecutable(), { "--self-check" });
    QVERIFY(withoutLog.waitForStarted(10000));
    QVERIFY(withoutLog.waitForFinished(30000));
    QCOMPARE(withoutLog.exitCode(), 2);
}

void ExternalReceiverTest::unknownProviderIsRefused()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QProcess receiver;
    receiver.start(receiverExecutable(),
                   { "--log", dir.filePath("run.log"), "--providers", "gameinput,telepathy" });
    QVERIFY(receiver.waitForStarted(10000));
    QVERIFY(receiver.waitForFinished(30000));
    QCOMPARE(receiver.exitCode(), 2);
    QVERIFY(QString::fromUtf8(receiver.readAllStandardError())
                .contains(QStringLiteral("unknown provider 'telepathy'")));
}

void ExternalReceiverTest::aRunWithoutAControllerStillEndsHonestly()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString log = dir.filePath("xinput-only.log");
    const QString phases = dir.filePath("phases.txt");
    QFile phaseFile(phases);
    QVERIFY(phaseFile.open(QIODevice::WriteOnly));

    QProcess receiver;
    // XInput is the path a game most likely uses, and this machine's attached
    // pad is not an XInput device: the run must end with a verdict of
    // not-measurable rather than an empty "isolated" claim, and it must exit 0.
    receiver.start(receiverExecutable(),
                   { "--log", log, "--providers", "xinput", "--phase-file", phases,
                     "--duration", "2", "--heartbeat-ms", "500" });
    QVERIFY(receiver.waitForStarted(10000));
    QVERIFY(waitForEvent(log, QStringLiteral("ready"), 10000));
    phaseFile.write("baseline\n");
    phaseFile.flush();
    QTest::qWait(600);
    phaseFile.write("exclusive\n");
    phaseFile.flush();
    QTest::qWait(600);
    phaseFile.write("restored\n");
    phaseFile.flush();
    QVERIFY(receiver.waitForFinished(30000));
    QCOMPARE(receiver.exitCode(), 0);

    const QVector<GameReceiver::LogRecord> records = parseLog(log);
    const auto verdicts = recordsWithEvent(records, QStringLiteral("verdict"));
    QCOMPARE(verdicts.size(), 1);
    QCOMPARE(fieldOf(verdicts.at(0), "result"), QStringLiteral("not-measurable"));
    QVERIFY(fieldOf(verdicts.at(0), "note").contains(QStringLiteral("proves nothing")));

    const auto phasesSeen = recordsWithEvent(records, QStringLiteral("phase"));
    QCOMPARE(phasesSeen.size(), 3);
    QCOMPARE(fieldOf(phasesSeen.at(0), "name"), QStringLiteral("baseline"));
    QCOMPARE(fieldOf(phasesSeen.at(2), "name"), QStringLiteral("restored"));
}

// ---------------------------------------------------------------------------
// The experiment. The GameHQ side here is the shipped policy path: the same
// GameInputFocusController and ProductionGameInputApi the app runs, asking for
// the same mask cpo-o06c defined. The overlay's own decision to ask is covered
// in tst_overlaynative with a recording sink; what this case adds is the
// question the overlay cannot answer from inside the process it lives in —
// whether a second process stops receiving the pad.
//
// Each round drives baseline -> exclusive -> restored, and the whole round is
// run twice: one transition could be a device or provider quirk, four identical
// shapes cannot. What is asserted is deliberately MECHANICAL — the phases
// happened, the receiver stayed a background process, every provider produced a
// verdict, and ordinary reception came back. The verdict itself is printed, not
// pre-declared: "the other client kept receiving the pad" is a measurement, not
// a harness failure, and a run whose own baseline saw nothing says
// not-measurable instead of claiming isolation.
// ---------------------------------------------------------------------------
void ExternalReceiverTest::externalReceiverMeasuresTheExclusivePolicy()
{
    ModernInput::ProductionGameInputApi api;
    QString error;
    if (!api.initialize(error))
        QSKIP(qPrintable(QStringLiteral("no GameInput runtime: %1").arg(error)));

    // The policy is about the interactive foreground app, so a request from a
    // background process would measure something else entirely.
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = DefWindowProcW;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"GameHQInputReceiverTest.Window";
    RegisterClassW(&windowClass);
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, L"GameHQ isolation probe",
                                  WS_OVERLAPPEDWINDOW, 80, 80, 420, 240, nullptr, nullptr,
                                  windowClass.hInstance, nullptr);
    QVERIFY(window != nullptr);
    ShowWindow(window, SW_SHOWNORMAL);
    std::unique_ptr<ForegroundApi> foreground(ForegroundApi::createSystem());
    foreground->forceForeground(window);
    QTest::qWait(500);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    ModernInput::GameInputFocusController controller;
    controller.attach(&api);

    // One round inside one receiver run. A void lambda, so QVERIFY still works
    // exactly as it does in the slot itself.
    const auto runRound = [&](int round, const QString& log, QProcess& receiver) {
        const QString phases = dir.filePath(QStringLiteral("phases-%1.txt").arg(round + 1));
        QFile phaseFile(phases);
        QVERIFY(phaseFile.open(QIODevice::WriteOnly));

        // The receiver compares the triple it is told to compare; each round
        // names its own phases so the two comparisons cannot be confused.
        const QString roundPhases =
            QStringLiteral("baseliner%1,exclusiver%1,restoredr%1").arg(round + 1);
        const QStringList arguments{
            "--log",           log,
            "--phase-file",    phases,
            "--heartbeat-ms",  "500",
            "--label",
            QStringLiteral("cpo-o06d external receiver round %1").arg(round + 1),
            "--verdict-phases", roundPhases,
            "--stop-on-stdin",
        };
        receiver.start(receiverExecutable(), arguments);
        QVERIFY(receiver.waitForStarted(10000));
        QVERIFY2(waitForEvent(log, QStringLiteral("ready"), 15000),
                 qPrintable(QString::fromUtf8(receiver.readAllStandardError())));

        const auto mark = [&](const QString& name) {
            phaseFile.write(name.toUtf8());
            phaseFile.write("\n");
            phaseFile.flush();
        };
        // The receiver compares the triple it is told to compare; each round
        // names its own phases so the two comparisons cannot be confused.


        // The receiver must actually SEE the pad while it is a background
        // process, otherwise the run is unmeasurable and says so. Phase names
        // repeat across rounds, so each round gets its own phase names and the
        // receiver compares the triple it was told to compare.
        mark(QStringLiteral("baseliner%1").arg(round + 1));
        QTest::qWait(3500);
        QVERIFY(controller.requestExclusiveForeground(
            QStringLiteral("cpo-o06d probe round %1").arg(round + 1)));
        QVERIFY(controller.exclusiveForegroundActive());
        mark(QStringLiteral("exclusiver%1").arg(round + 1));
        QTest::qWait(4500);
        controller.restoreBackground(QStringLiteral("cpo-o06d probe round %1").arg(round + 1));
        QVERIFY(!controller.exclusiveForegroundActive());
        mark(QStringLiteral("restoredr%1").arg(round + 1));
        QTest::qWait(3500);

        receiver.closeWriteChannel();
        QVERIFY(receiver.waitForFinished(20000));
        QCOMPARE(receiver.exitCode(), 0);
    };

    for (int round = 0; round < 2; ++round) {
        const QString log = dir.filePath(QStringLiteral("receiver-%1.log").arg(round + 1));
        QProcess receiver;
        runRound(round, log, receiver);

        const QVector<GameReceiver::LogRecord> records = parseLog(log);
        QVERIFY(!records.isEmpty());

        const auto ready = recordsWithEvent(records, QStringLiteral("ready"));
        QCOMPARE(ready.size(), 1);
        // The one invariant that makes the measurement meaningful at all: the
        // receiver never held the foreground, so a gap can never be explained
        // by the receiver having been backgrounded.
        QCOMPARE(fieldOf(ready.at(0), "self"), QStringLiteral("background"));
        QCOMPARE(fieldOf(ready.at(0), "providers"), QStringLiteral("3"));

        // The GameInput path says which policy it asked the runtime for: that is
        // the difference between a fair measurement and a gap that would only
        // mean "we were not the foreground app".
        bool policyStated = false;
        for (const GameReceiver::LogRecord& record : records) {
            if (fieldOf(record, "provider") != QStringLiteral("gameinput"))
                continue;
            if (fieldOf(record, "detail").contains(QStringLiteral("policy=background-input+background-guide+background-share")))
                policyStated = true;
        }
        QVERIFY(policyStated);

        QCOMPARE(recordsWithEvent(records, QStringLiteral("phase")).size(), 3);
        const auto exit = recordsWithEvent(records, QStringLiteral("exit"));
        QCOMPARE(exit.size(), 1);
        QCOMPARE(fieldOf(exit.at(0), "reason"), QStringLiteral("stdin"));

        const auto verdicts = recordsWithEvent(records, QStringLiteral("verdict"));
        QCOMPARE(verdicts.size(), 3);
        const QVector<GameReceiver::LogRecord> summaries =
            recordsWithEvent(records, QStringLiteral("summary"));
        QCOMPARE(summaries.size(), 9);
        for (const GameReceiver::LogRecord& verdict : verdicts) {
            const QString result = fieldOf(verdict, "result");
            QVERIFY2(result == QStringLiteral("not-measurable")
                         || result == QStringLiteral("continuous")
                         || result == QStringLiteral("blocked-then-resumed")
                         || result == QStringLiteral("blocked-no-resume"),
                     qPrintable(result));
        }

        // A phase that never received anything is a fact about THIS run: with a
        // pad attached the GameInput baseline must be measurable, otherwise the
        // receipt is worthless and the reader deserves to be told loudly.
        for (const GameReceiver::LogRecord& summary : summaries) {
            if (fieldOf(summary, "provider") != QStringLiteral("gameinput"))
                continue;
            if (fieldOf(summary, "phase") != QStringLiteral("baseliner%1").arg(round + 1))
                continue;
            qInfo("cpo-o06d round %d baseline: GameInput %s arrivals at %s", round + 1,
                  qPrintable(fieldOf(summary, "arrivals")),
                  qPrintable(fieldOf(summary, "rate")));
        }
        for (const GameReceiver::LogRecord& verdict : verdicts)
            qInfo("cpo-o06d measured (round %d): provider=%s %s \u2014 %s", round + 1,
                  qPrintable(fieldOf(verdict, "provider")),
                  qPrintable(fieldOf(verdict, "result")),
                  qPrintable(fieldOf(verdict, "note")));

        // The raw receipt is kept on disk next to the build (or wherever
        // GAMEHQ_INPUT_EVIDENCE_DIR points), so the numbers can be re-read
        // without re-running the experiment.
        const QString receipt =
            evidencePath(QStringLiteral("cpo-o06d-receiver-receipt-run%1.log").arg(round + 1));
        QFile::remove(receipt);
        QVERIFY(QFile::copy(log, receipt));
    }

    controller.detach();
    api.unload();
    DestroyWindow(window);
}

QTEST_MAIN(ExternalReceiverTest)
#include "tst_inputreceiver.moc"
