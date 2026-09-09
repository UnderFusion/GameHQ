#include "input/InputDiagnostics.h"

#include <QTest>

// Bounded rings, probe window semantics, privacy redaction, and export layout.
// Uses a local instance (not ::instance()) so cases stay independent.
class InputDiagnosticsTest : public QObject
{
    Q_OBJECT

private slots:
    void betaPackageContainsOnlyRelevantSanitizedEvidence()
    {
        InputDiagnostics diag;
        diag.noteControl("gamepad.view_back", "XInput");
        diag.setReplayBindings("C:\\Users\\Secret Person\\controller", {
            "slot=1 trigger=gamepad.view_back activation=hold hold_ms=2000 tap_count=1",
            "slot=2 trigger=C:\\Users\\Secret Person\\private"});
        diag.setBoundPatterns({"unrelated screenshot binding"});
        diag.noteDevice("private serial", "private device", "unused");
        const QVariantMap config{{"capture.mode", "always"}, {"replay.auto", true},
            {"replay.clip_notify", false}, {"storage.clips_root", "C:\\Users\\Secret Person"}};
        const QString log = "Private game C:\\Users\\Secret Person\\game.exe\n"
            "ReplaySave[77 src=controller +12ms]: accepted\n"
            "ReplaySave[77 src=controller +12ms]: armed game=Private game\n"
            "ReplaySave[77]: exporting output=\\\\server\\share\\Secret Person.mp4\n"
            "ReplaySave[77]: failed - remux failed output=/home/private/name.mp4\n"
            "ReplaySave[78 src=keyboard +13ms]: accepted\n"
            "ReplaySave[78]: published\n";
        const QString text = diag.exportBetaText("0.7.23 compiled Sep 9 2026 12:00:00", "10.0.26200", config, log);
        for (const auto& expected : {"0.7.23", "10.0.26200", "Active provider: XInput",
                "Resolved controller profile: sha256:", "global.save_replay", "hold_ms=2000",
                "capture.mode=always", "replay.auto=1", "replay.clip_notify=0",
                "request=77 accepted", "request=77 armed", "request=77 exporting", "request=77 failed"})
            QVERIFY2(text.contains(QLatin1String(expected)), expected);
        for (const auto& secret : {"Secret Person", "private", "Private game", "server", "share",
                "C:\\", "/home/", "storage.clips_root", "screenshot", "request=78"})
            QVERIFY2(!text.contains(QLatin1String(secret)), secret);
    }

    void betaTraceIsBoundedAndMissingEvidenceIsExplicit()
    {
        InputDiagnostics diag;
        QString log;
        for (int i = 1; i <= 50; ++i)
            log += QStringLiteral("ReplaySave[%1 src=controller +1ms]: accepted\n").arg(i);
        const QString text = diag.exportBetaText("build", "10.0.26200", {}, log);
        QCOMPARE(text.count("request="), InputDiagnostics::kMaxTraceEvents);
        QVERIFY(!text.contains("request=1 "));
        QVERIFY(text.contains("request=50 accepted"));
        const QString empty = diag.exportBetaText("C:/Users/secret/build", "\\\\server\\secret", {}, {});
        QVERIFY(empty.contains("unavailable: no controller requests"));
        QVERIFY(!empty.contains("secret"));
        log += QString(InputDiagnostics::kMaxTraceBytes, QLatin1Char('x'));
        QVERIFY(!diag.exportBetaText("build", "windows", {}, log).contains("request="));
        diag.setReplayBindings("secret-profile", {"test"});
        diag.clear();
        QVERIFY(diag.exportBetaText("build", "windows", {}, {}).contains("profile: none"));
    }

    void ringsKeepOnlyTheLatestEntries()
    {
        InputDiagnostics diag;
        for (int i = 0; i < InputDiagnostics::kMaxSwitches + 10; ++i)
            diag.noteBackendSwitch(QStringLiteral("backend%1").arg(i),
                                   QStringLiteral("reason"));
        const QString text = diag.exportText();
        QVERIFY(!text.contains(QStringLiteral("backend0 ")));
        QVERIFY(!text.contains(QStringLiteral("backend9 (")));
        QVERIFY(text.contains(QStringLiteral("backend%1")
                                  .arg(InputDiagnostics::kMaxSwitches + 9)));
    }

    void probeRespectsWindowAndEventCap()
    {
        InputDiagnostics diag;
        QVERIFY(!diag.probeActive());
        QVERIFY(!diag.noteProbeEvent(QStringLiteral("054C:0CE6"),
                                     QStringLiteral("Raw Input"),
                                     QStringLiteral("outside window")));

        diag.startProbe(400);
        QVERIFY(diag.probeActive());
        int accepted = 0;
        for (int i = 0; i < InputDiagnostics::kMaxProbeEvents + 20; ++i) {
            if (diag.noteProbeEvent(QStringLiteral("054C:0CE6"),
                                    QStringLiteral("Raw Input"),
                                    QStringLiteral("event %1").arg(i)))
                ++accepted;
        }
        QCOMPARE(accepted, InputDiagnostics::kMaxProbeEvents);
        QVERIFY(diag.probeSummary().contains(QStringLiteral("event cap reached")));

        QTRY_VERIFY_WITH_TIMEOUT(!diag.probeActive(), 1000);
        QVERIFY(!diag.noteProbeEvent(QStringLiteral("054C:0CE6"),
                                     QStringLiteral("Raw Input"),
                                     QStringLiteral("late")));
        QVERIFY(diag.probeSummary().startsWith(QStringLiteral("Probe: finished")));
    }

    void probeSummaryStatesWhenNothingArrived()
    {
        InputDiagnostics diag;
        QCOMPARE(diag.probeSummary(), QStringLiteral("Probe: never run"));
        diag.startProbe(250);
        QTRY_VERIFY_WITH_TIMEOUT(!diag.probeActive(), 1000);
        QVERIFY(diag.probeSummary().contains(
            QStringLiteral("no button change reached GameHQ")));
    }

    void redactionKeepsVidPidAndDropsTheRest()
    {
        const QString path = QStringLiteral(
            R"(\\?\HID#VID_054C&PID_0CE6&MI_03#9&2f4ab73&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030})");
        const QString redacted = InputDiagnostics::redactDevicePath(path);
        QVERIFY(redacted.startsWith(QStringLiteral("054C:0CE6/#")));
        QVERIFY(!redacted.contains(QStringLiteral("2f4ab73")));
        QVERIFY(!redacted.contains(QStringLiteral("{4d1e55b2")));
        // Stable for the same path, different for a different instance path.
        QCOMPARE(InputDiagnostics::redactDevicePath(path), redacted);
        const QString other = InputDiagnostics::redactDevicePath(
            QStringLiteral(R"(\\?\HID#VID_054C&PID_0CE6&MI_03#9&DEADBEEF&0&0000#{x})"));
        QVERIFY(other != redacted);
        QCOMPARE(InputDiagnostics::redactDevicePath({}), QString());
    }

    void exportContainsEverySection()
    {
        InputDiagnostics diag;
        diag.setPreviousSessionCrashed(true);
        diag.noteBackendSwitch(QStringLiteral("Sony Raw Input"),
                               QStringLiteral("control activity"));
        diag.noteDevice(QStringLiteral("054C:0CE6"), QStringLiteral("054C:0CE6/#abcd1234"),
                        QStringLiteral("tracked (DualSense)"));
        diag.noteRate(QStringLiteral("054C:0CE6"), 290);
        diag.noteControl(QStringLiteral("gamepad.capture"), QStringLiteral("Sony Raw Input"));
        diag.noteForeground(QStringLiteral("overlay show"), true);
        diag.setCloakStatus({QStringLiteral("DualSense")}, true);

        const QString text = diag.exportText();
        QVERIFY(text.contains(QStringLiteral("Previous session ended unexpectedly: YES")));
        QVERIFY(text.contains(QStringLiteral("Active backend: Sony Raw Input")));
        QVERIFY(text.contains(QStringLiteral("Sony Raw Input (control activity)")));
        QVERIFY(text.contains(QStringLiteral("054C:0CE6 054C:0CE6/#abcd1234 -> tracked (DualSense) @ 290 events/s")));
        QVERIFY(text.contains(QStringLiteral("gamepad.capture (Sony Raw Input)")));
        QVERIFY(text.contains(QStringLiteral("overlay show acquired")));
        QVERIFY(text.contains(QStringLiteral("HidHide installed")));
        QVERIFY(text.contains(QStringLiteral("Probe: never run")));
    }

    void clearForgetsEverything()
    {
        InputDiagnostics diag;
        diag.setPreviousSessionCrashed(true);
        diag.noteBackendSwitch(QStringLiteral("XInput"), QStringLiteral("fallback"));
        diag.startProbe(5000);
        diag.clear();
        QVERIFY(!diag.probeActive());
        const QString text = diag.exportText();
        QVERIFY(text.contains(QStringLiteral("Previous session ended unexpectedly: no")));
        QVERIFY(text.contains(QStringLiteral("Active backend: none")));
    }
};

QTEST_GUILESS_MAIN(InputDiagnosticsTest)
#include "tst_inputdiagnostics.moc"
