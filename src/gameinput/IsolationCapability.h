#pragma once

#include <QString>

// cpo-o06f: what GameHQ may claim about controller isolation, and what it must
// not claim.
//
// The question is deliberately narrow. Since cpo-o06c the overlay asks GameInput
// for the exclusive-foreground policy while it is interactive, and cpo-o06d
// measured from *outside* this process what that policy does to another
// GameInput client: delivery stopped while the policy was in force and resumed
// after release, twice, on a wired DualSense. That is evidence for one class of
// clients. It is not evidence that every game stops seeing the pad - no process
// can observe which input path the game in front of it uses - and the document
// that carries the measured cases is docs/overlay.md.
//
// The three levels are deliberately asymmetric:
//
//   unavailable  no mechanism is in force for this run, so nothing is narrowed.
//   scoped       the mechanism is in force and external evidence exists for a
//                NAMED client class only; the game's own path stays unconfirmed.
//   full-native  the mechanism is in force AND the tested game/pad/configuration
//                was confirmed externally. An API call can never set that.
//
// FullNative is unreachable from in-process facts by construction: this process
// cannot confirm the game's input path, so production code never sets
// `gamePathConfirmed`. tests/tst_isolationcapability.cpp pins that, exhaustively.
namespace GameInputIsolation
{

enum class Coverage
{
    Unavailable,
    Scoped,
    FullNative,
};

// What has been measured OUTSIDE the running app. Every field is named after the
// evidence that sets it; nothing here is inferred from an API call.
struct Evidence
{
    // tools/input-receiver: a separate GameInput client's delivery stopped for
    // the whole exclusive phase and resumed after release, reproduced twice on a
    // wired DualSense (docs/overlay.md, "Controller isolation").
    bool gameInputClientsMeasured = false;
    // A recorded physical acceptance result for one game + pad + configuration.
    // Only a tester's checklist can set this; GameHQ cannot observe it.
    bool gamePathConfirmed = false;
};

struct Facts
{
    // The exclusive-foreground policy as GameHQ actually applied it - the mode
    // GameInput is running under, never the fact that a request was made.
    bool policyInForce = false;
    Evidence evidence;
};

struct Verdict
{
    Coverage coverage = Coverage::Unavailable;
    QString name;          // "unavailable" | "scoped" | "full-native"
    QString evidenceTier;  // "none" | "external-gameinput-clients" | "external-game"
    QString detail;        // one sentence; safe to log or show
    bool claimsFullNative() const { return coverage == Coverage::FullNative; }
    // "isolation=scoped evidence=external-gameinput-clients real-game=unconfirmed"
    QString toLogString() const;
};

Verdict classify(const Facts& facts);

// The evidence this build actually has on file. Kept in one place so a claim can
// only widen by changing this function *and* the document beside it.
Evidence recordedEvidence();

}  // namespace GameInputIsolation
