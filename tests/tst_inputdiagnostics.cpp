#include "input/InputDiagnostics.h"

#include <QCryptographicHash>
#include <QTest>

// Bounded rings, probe window semantics, privacy redaction, and export layout.
// Uses a local instance (not ::instance()) so cases stay independent.
class InputDiagnosticsTest : public QObject
{
    Q_OBJECT

private slots:
    void providerLifecycleIsBoundedAndSanitized()
    {
        InputDiagnostics diag;
        for (int i = 0; i < 40; ++i)
            diag.noteProviderTransition(QStringLiteral("edge-%1").arg(i), "GameInput",
                "private-device", "private-logical", "private-container", "private-endpoint",
                "private-root", "overlay=1 held=2 attached=0 runtime=registration failed");
        const QString text = diag.exportBetaText("0.7.8", "Windows", {}, {});
        QVERIFY(!text.contains("private-"));
        QVERIFY(!text.contains("event=edge-0 "));
        QVERIFY(text.contains("event=edge-39 "));
        QVERIFY(text.contains("container=sha256:"));
        QVERIFY(text.contains("held=2 attached=0 runtime=registration failed"));
        diag.clear();
        QVERIFY(!diag.exportBetaText("0.7.8", "Windows", {}, {}).contains("event=edge-"));
    }
    void betaPackageContainsOnlyRelevantSanitizedEvidence()
    {
        InputDiagnostics diag;
        diag.noteControl("gamepad.view_back", "XInput");
        diag.setReplayBindings("C:\\Users\\USERNAME\\controller", {
            "slot=1 trigger=gamepad.view_back activation=hold hold_ms=2000 tap_count=1",
            "slot=2 trigger=C:\\Users\\USERNAME\\private"});
        diag.setBoundPatterns({"unrelated screenshot binding"});
        diag.noteDevice("private serial", "private device", "unused");
        const QVariantMap config{{"capture.mode", "always"}, {"replay.auto", true},
            {"replay.clip_notify", false}, {"storage.clips_root", "C:\\Users\\USERNAME"}};
        const QString log = "Private game C:\\Users\\USERNAME\\game.exe\n"
            "ReplaySave[77 src=controller +12ms]: accepted\n"
            "ReplaySave[77 src=controller +12ms]: armed game=Private game\n"
            "ReplaySave[77]: exporting output=\\\\server\\share\\USERNAME.mp4\n"
            "ReplaySave[77]: failed - remux failed output=/home/user/name.mp4\n"
            "ReplaySave[78 src=keyboard +13ms]: accepted\n"
            "ReplaySave[78]: published\n";
        const QString text = diag.exportBetaText("0.7.23 compiled Sep 9 2026 12:00:00", "10.0.26200", config, log);
        for (const auto& expected : {"0.7.23", "10.0.26200", "Active provider: XInput",
                "Resolved controller profile: sha256:", "global.save_replay", "hold_ms=2000",
                "capture.mode=always", "replay.auto=1", "replay.clip_notify=0",
                "request=77 accepted", "request=77 armed", "request=77 exporting", "request=77 failed"})
            QVERIFY2(text.contains(QLatin1String(expected)), expected);
        for (const auto& secret : {"USERNAME", "private", "Private game", "server", "share",
                "C:\\", "/home/", "storage.clips_root", "screenshot", "request=78"})
            QVERIFY2(!text.contains(QLatin1String(secret)), secret);
    }

    // cpo-x01: the controller-routing, mapping and overlay/focus sections must
    // be present with canonical labels - and every identity value (logical
    // profile, running game key, assignment target key) enters only as a
    // pseudonym, never as a path.
    void routingMappingAndOverlayStateCarryCanonicalEvidence()
    {
        InputDiagnostics diag;
        const QString gamePath = QStringLiteral("C:\\Users\\USERNAME\\game.exe");
        const QString profile = QStringLiteral(R"(\\?\HID#VID_054C&PID_0CE6#9&DEADBEEF&0&0000)");
        diag.setControllerRouting(
            QStringLiteral("XInput controller"), QStringLiteral("connection fallback"),
            {QStringLiteral("route identity: legacy slot (weak or unknown confidence)"),
             QStringLiteral("provider last-control ages: XInput controller 0.2 s"),
             QStringLiteral("events dropped inside the mirror window: 3"),
             QStringLiteral("candidate presses held for confirmation: 1")});
        diag.setMappingState(
            gamePath,
            {{QStringLiteral("controller"), profile, QStringLiteral("game"),
              QStringLiteral("preset-alpha"), true, QStringLiteral("0f21aa")},
             {QStringLiteral("keyboard"), {}, QStringLiteral("builtin"), {}, true,
              QStringLiteral("77bb")}},
            {{QStringLiteral("controller"), QStringLiteral("game"),
              QStringLiteral("C:\\Users\\USERNAME\\other.exe"), QStringLiteral("preset-beta"),
              QStringLiteral("wrong_group")}});
        diag.noteOverlayShow(true);

        const QString text = diag.exportBetaText(QStringLiteral("0.7.26"),
                                                 QStringLiteral("10.0.26200"), {}, {});
        for (const auto& expected : {"Controller routing:", "serving provider: XInput controller",
                "last switch reason: connection fallback",
                "route identity: legacy slot (weak or unknown confidence)",
                "events dropped inside the mirror window: 3",
                "candidate presses held for confirmation: 1", "Mapping state:",
                "game context: present (key sha256:", "routes:",
                "source=game preset=preset-alpha owned=yes fp=0f21aa",
                "keyboard -> source=builtin preset=none owned=yes",
                "stale assignments (skipped, next winner served):", "wrong_group",
                "Overlay/focus:", "overlay visible: yes",
                "game foreground preserved on show: yes",
                "game session transitions:", "present sha256:", "overlay show/hide:",
                "overlay show | game foreground preserved"})
            QVERIFY2(text.contains(QLatin1String(expected)), expected);

        // The pseudonyms themselves must be there: redaction without the stable
        // hash would make two reports of one profile impossible to correlate.
        const auto pseudonym = [](const QString& value) {
            return QStringLiteral("sha256:")
                + QString::fromLatin1(QCryptographicHash::hash(
                      value.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        };
        QVERIFY(text.contains(pseudonym(gamePath)));
        QVERIFY(text.contains(pseudonym(profile)));
        QVERIFY(text.contains(pseudonym(QStringLiteral("C:\\Users\\USERNAME\\other.exe"))));
        for (const auto& secret : {"USERNAME", "C:\\", "HID#VID", "DEADBEEF", "game.exe"})
            QVERIFY2(!text.contains(QLatin1String(secret)), secret);
    }

    // cpo-x01: a package with no overlay/routing activity says so explicitly,
    // and the two stamped timelines move with the state - that separation is
    // what distinguishes "the overlay was open" from "the game session changed".
    void routingMappingAndOverlayStateStayExplicitAndMoveWithTheState()
    {
        InputDiagnostics diag;
        QString text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        for (const auto& expected : {"Controller routing:",
                "serving provider: unavailable (no provider switch observed)",
                "arbitration: unavailable (no routing transition observed)", "Mapping state:",
                "game context: unavailable (no mapping refresh observed)", "routes: none tracked",
                "Overlay/focus:", "overlay visible: unavailable (no overlay activity this session)",
                "game foreground preserved on show: unavailable (no overlay show this session)",
                "game session transitions:",
                "overlay show/hide:", "no overlay open/close this session"})
            QVERIFY2(text.contains(QLatin1String(expected)), expected);
        QVERIFY(!text.contains(QStringLiteral("stale assignments")));

        diag.setMappingState(QStringLiteral("D:\\Games\\Alpha\\alpha.exe"), {}, {});
        // A show that did not preserve the foreground: the export must state
        // the observation, never reinterpret the policy as "the game keeps
        // input".
        diag.noteOverlayShow(false);
        text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral("game context: present (key sha256:")));
        QVERIFY(text.contains(QStringLiteral("overlay visible: yes")));
        QVERIFY(text.contains(QStringLiteral(
            "game foreground preserved on show: no (the foreground changed during show)")));
        QVERIFY(text.contains(QStringLiteral(
            "overlay show | foreground CHANGED (game lost foreground)")));
        QVERIFY(!text.contains(QStringLiteral("keeps input")));
        QVERIFY(!text.contains(QStringLiteral("D:\\Games")));
        QVERIFY(!text.contains(QStringLiteral("alpha.exe")));

        // Game exit and overlay close: both timelines get their new entry, and
        // the raw key is still absent.
        diag.setMappingState(QString(), {}, {});
        diag.noteOverlayHide();
        text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral("game context: none")));
        QVERIFY(text.contains(QStringLiteral("overlay visible: no")));
        QVERIFY(text.contains(QStringLiteral("overlay hide")));
        QVERIFY(text.contains(QStringLiteral("present sha256:")));
        QVERIFY(text.contains(QStringLiteral("s none")));
    }

    // cpo-x01: the overlay timeline is written by real show/hide only. A hide
    // entry carries no preservation value (there is none to observe at close),
    // a failed show exports failure wording rather than the old policy claim,
    // and the ring stays bounded like every other ring in the package.
    void overlayTransitionsComeFromShowAndHideOnly()
    {
        InputDiagnostics diag;
        QString text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral(
            "game foreground preserved on show: unavailable (no overlay show this session)")));
        QVERIFY(text.contains(QStringLiteral("no overlay open/close this session")));

        diag.noteOverlayShow(true);
        text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral("overlay visible: yes")));
        QVERIFY(text.contains(QStringLiteral("game foreground preserved on show: yes")));
        QVERIFY(text.contains(QStringLiteral("overlay show | game foreground preserved")));

        diag.noteOverlayHide();
        text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral("overlay visible: no")));
        QVERIFY(text.contains(QStringLiteral("overlay hide")));
        // The hide branch must not serialize a preservation boolean.
        QVERIFY(!text.contains(QStringLiteral("overlay hide |")));

        diag.noteOverlayShow(false);
        text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral(
            "game foreground preserved on show: no (the foreground changed during show)")));
        QVERIFY(text.contains(QStringLiteral(
            "overlay show | foreground CHANGED (game lost foreground)")));
        QVERIFY(!text.contains(QStringLiteral("keeps input")));

        InputDiagnostics bounded;
        for (int i = 0; i < InputDiagnostics::kMaxOverlayTransitions + 5; ++i)
            bounded.noteOverlayShow(true);
        const QString boundedText =
            bounded.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QCOMPARE(boundedText.count(QStringLiteral("overlay show | ")),
                 InputDiagnostics::kMaxOverlayTransitions);

        diag.clear();
        text = diag.exportBetaText(QStringLiteral("build"), QStringLiteral("windows"), {}, {});
        QVERIFY(text.contains(QStringLiteral("no overlay open/close this session")));
        QVERIFY(text.contains(QStringLiteral(
            "game foreground preserved on show: unavailable (no overlay show this session)")));
    }

    // cpo-x01: an unknown source or stale-reason label is not evidence; it is
    // dropped rather than passed through.
    void unknownMappingLabelsNeverEnterTheExport()
    {
        InputDiagnostics diag;
        diag.setMappingState(
            {},
            {{QStringLiteral("controller"), {}, QStringLiteral("rogue_source"), {}, true, {}}},
            {{QStringLiteral("controller"), QStringLiteral("weird_kind"), {},
              {}, QStringLiteral("spontaneous")}});
        const QString text = diag.exportBetaText(QStringLiteral("build"),
                                                 QStringLiteral("windows"), {}, {});
        for (const auto& unknown : {"rogue_source", "weird_kind", "spontaneous"})
            QVERIFY2(!text.contains(QLatin1String(unknown)), unknown);
        QVERIFY(text.contains(QStringLiteral("[redacted]")));
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
