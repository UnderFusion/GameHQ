// cpo-o06f: the capability classification, and the one rule that matters most —
// an API call may never produce "full native isolation".
//
// The classification exists because the honest answer about controller isolation
// is narrower than the mechanism sounds. Since cpo-o06c the overlay asks
// GameInput for the exclusive-foreground policy; cpo-o06d measured from outside
// the process that another GameInput client's delivery really stops while that
// policy is in force and really resumes after release — twice, on a wired
// DualSense. That is evidence for one client class, on one pad. It is not
// evidence that every game stops seeing the pad, and nothing in this process can
// observe which input path a game uses.
//
// So: three levels, and `full-native` requires facts no running GameHQ can
// produce. The exhaustive case below walks all eight combinations of the three
// inputs and fails if any of them claims more than what it was given.
#include "gameinput/IsolationCapability.h"

#include <QTest>

namespace {

using GameInputIsolation::Coverage;
using GameInputIsolation::Evidence;
using GameInputIsolation::Facts;

Facts facts(bool policyInForce, bool measuredClients, bool confirmedGamePath)
{
    Facts f;
    f.policyInForce = policyInForce;
    f.evidence.gameInputClientsMeasured = measuredClients;
    f.evidence.gamePathConfirmed = confirmedGamePath;
    return f;
}

}  // namespace

class IsolationCapabilityTest : public QObject
{
    Q_OBJECT

private slots:
    // No mechanism, nothing narrowed — and recorded evidence about another run
    // must not turn that into a claim about this one.
    void noPolicyMeansUnavailable()
    {
        const auto verdict = GameInputIsolation::classify(facts(false, true, true));
        QCOMPARE(verdict.coverage, Coverage::Unavailable);
        QCOMPARE(verdict.name, QStringLiteral("unavailable"));
        QCOMPARE(verdict.evidenceTier, QStringLiteral("none"));
        QVERIFY2(verdict.detail.contains(QStringLiteral("keeps every press")),
                 qPrintable(verdict.detail));
        QVERIFY(!verdict.claimsFullNative());
    }

    // THE rule of this leaf: the mechanism alone is never "full native".
    void mechanismAloneIsNeverFullNative()
    {
        const auto verdict = GameInputIsolation::classify(facts(true, false, false));
        QCOMPARE(verdict.coverage, Coverage::Scoped);
        QCOMPARE(verdict.name, QStringLiteral("scoped"));
        QCOMPARE(verdict.evidenceTier, QStringLiteral("none"));
        QVERIFY(!verdict.claimsFullNative());
        QVERIFY2(verdict.detail.contains(QStringLiteral("no external measurement")),
                 qPrintable(verdict.detail));
    }

    // External evidence about a client class still is not evidence about a game.
    void measuredClientsStayScoped()
    {
        const auto verdict = GameInputIsolation::classify(facts(true, true, false));
        QCOMPARE(verdict.coverage, Coverage::Scoped);
        QCOMPARE(verdict.evidenceTier, QStringLiteral("external-gameinput-clients"));
        QVERIFY2(verdict.detail.contains(QStringLiteral("GameInput clients only")),
                 qPrintable(verdict.detail));
        QVERIFY2(verdict.detail.contains(QStringLiteral("unconfirmed")),
                 qPrintable(verdict.detail));
        QVERIFY(!verdict.claimsFullNative());
    }

    // A game-path result without the measurement behind it is not a result: the
    // rule is conjunction, not "any one fact".
    void confirmedGamePathWithoutMeasurementStaysScoped()
    {
        const auto verdict = GameInputIsolation::classify(facts(true, false, true));
        QCOMPARE(verdict.coverage, Coverage::Scoped);
        QVERIFY(!verdict.claimsFullNative());
    }

    void everyFactTogetherIsFullNative()
    {
        const auto verdict = GameInputIsolation::classify(facts(true, true, true));
        QCOMPARE(verdict.coverage, Coverage::FullNative);
        QCOMPARE(verdict.name, QStringLiteral("full-native"));
        QCOMPARE(verdict.evidenceTier, QStringLiteral("external-game"));
        QVERIFY(verdict.claimsFullNative());
    }

    // Exhaustive: over all eight input combinations, full native appears exactly
    // once — for the one combination a running GameHQ cannot assemble.
    void onlyTheImpossibleCombinationClaimsFullNative()
    {
        int claims = 0;
        for (int bits = 0; bits < 8; ++bits) {
            const bool policy = bits & 1;
            const bool measured = bits & 2;
            const bool confirmed = bits & 4;
            const auto verdict = GameInputIsolation::classify(facts(policy, measured, confirmed));
            if (verdict.claimsFullNative()) {
                ++claims;
                QVERIFY(policy && measured && confirmed);
            }
            // Whatever the levels, the log line never overstates the game path.
            const QString line = verdict.toLogString();
            QVERIFY2(line.startsWith(QStringLiteral("isolation=")), qPrintable(line));
            QVERIFY2(line.contains(confirmed && measured && policy
                                       ? QStringLiteral("real-game=confirmed")
                                       : QStringLiteral("real-game=unconfirmed")),
                     qPrintable(line));
        }
        QCOMPARE(claims, 1);
    }

    // What this build has on file: the receiver runs, and nothing about a game.
    void recordedEvidenceIsNarrowOnPurpose()
    {
        const Evidence evidence = GameInputIsolation::recordedEvidence();
        QVERIFY(evidence.gameInputClientsMeasured);
        QVERIFY(!evidence.gamePathConfirmed);

        // With the policy in force — the state the overlay puts the runtime in —
        // the strongest claim available is still "scoped".
        const auto verdict = GameInputIsolation::classify(facts(true, evidence.gameInputClientsMeasured,
                                                                evidence.gamePathConfirmed));
        QCOMPARE(verdict.coverage, Coverage::Scoped);
        QVERIFY(!verdict.claimsFullNative());
    }

    // The grammar is what the overlay record and the diagnostics export carry,
    // so it is pinned literally rather than by substring drift.
    void logGrammarIsStable()
    {
        QCOMPARE(GameInputIsolation::classify(facts(false, true, false)).toLogString(),
                 QStringLiteral("isolation=unavailable evidence=none real-game=unconfirmed"));
        QCOMPARE(GameInputIsolation::classify(facts(true, false, false)).toLogString(),
                 QStringLiteral("isolation=scoped evidence=none real-game=unconfirmed"));
        QCOMPARE(GameInputIsolation::classify(facts(true, true, false)).toLogString(),
                 QStringLiteral("isolation=scoped evidence=external-gameinput-clients "
                                "real-game=unconfirmed"));
        QCOMPARE(GameInputIsolation::classify(facts(true, true, true)).toLogString(),
                 QStringLiteral("isolation=full-native evidence=external-game "
                                "real-game=confirmed"));
    }

    // No wording may read as "all games are covered".
    void noVerdictClaimsUniversalCoverage()
    {
        const QStringList forbidden = {
            QStringLiteral("all games"), QStringLiteral("every game"),
            QStringLiteral("universally"), QStringLiteral("all input paths"),
        };
        for (int bits = 0; bits < 8; ++bits) {
            const auto verdict = GameInputIsolation::classify(
                facts(bits & 1, bits & 2, bits & 4));
            const QString text = verdict.name + QLatin1Char(' ') + verdict.detail
                + QLatin1Char(' ') + verdict.toLogString();
            for (const QString& phrase : forbidden)
                QVERIFY2(!text.contains(phrase, Qt::CaseInsensitive), qPrintable(text));
        }
    }
};

QTEST_MAIN(IsolationCapabilityTest)
#include "tst_isolationcapability.moc"
