#include "gameinput/IsolationCapability.h"

namespace GameInputIsolation
{

Verdict classify(const Facts& facts)
{
    Verdict verdict;

    // No mechanism: say so plainly, and do not let recorded evidence about any
    // other run imply that this one narrows anything.
    if (!facts.policyInForce) {
        verdict.coverage = Coverage::Unavailable;
        verdict.name = QStringLiteral("unavailable");
        verdict.evidenceTier = QStringLiteral("none");
        verdict.detail = QStringLiteral(
            "no exclusive policy is in force for this run: the game keeps every "
            "press its own input path delivers");
        return verdict;
    }

    // The one level that needs everything: an active mechanism, a measurement
    // made outside this process, and a recorded result for this game and pad.
    // Two of the three are never enough, and production cannot set the second.
    if (facts.evidence.gameInputClientsMeasured && facts.evidence.gamePathConfirmed) {
        verdict.coverage = Coverage::FullNative;
        verdict.name = QStringLiteral("full-native");
        verdict.evidenceTier = QStringLiteral("external-game");
        verdict.detail = QStringLiteral(
            "exclusive policy in force and this game, pad and configuration was "
            "confirmed externally");
        return verdict;
    }

    verdict.coverage = Coverage::Scoped;
    verdict.name = QStringLiteral("scoped");
    verdict.evidenceTier = facts.evidence.gameInputClientsMeasured
        ? QStringLiteral("external-gameinput-clients")
        : QStringLiteral("none");
    verdict.detail = facts.evidence.gameInputClientsMeasured
        ? QStringLiteral(
              "exclusive policy in force; isolation is externally verified for "
              "GameInput clients only, and the game's own input path is unconfirmed")
        : QStringLiteral(
              "exclusive policy in force; no external measurement exists for any "
              "client class yet");
    return verdict;
}

QString Verdict::toLogString() const
{
    return QStringLiteral("isolation=%1 evidence=%2 real-game=%3")
        .arg(name,
             evidenceTier,
             claimsFullNative() ? QStringLiteral("confirmed")
                                : QStringLiteral("unconfirmed"));
}

Evidence recordedEvidence()
{
    Evidence evidence;
    // cpo-o06d: the receiver runs in docs/overlay.md - two rounds, wired
    // DualSense, GameInput clients only. Nothing here is extrapolated to pads or
    // input paths that were never measured.
    evidence.gameInputClientsMeasured = true;
    // Physical acceptance owns this one. GameHQ cannot observe a game's input
    // path, so it never claims one - and therefore never claims full_native.
    evidence.gamePathConfirmed = false;
    return evidence;
}

}  // namespace GameInputIsolation
