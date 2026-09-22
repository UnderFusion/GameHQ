// cpo-o06d: the pure half of the external controller receiver.
//
// The receiver exists to answer one question from OUTSIDE GameHQ's process:
// while the overlay holds the verified foreground and GameHQ has requested
// GameInput's exclusive-foreground policy, does a second process still receive
// the pad? Nothing inside GameHQ can answer that — its own logs can only say
// what it asked for, never what the runtime delivered to somebody else.
//
// This header deliberately contains no Windows, no Qt and no gamepad API: the
// transition rules, the phase bookkeeping, the verdicts and the log grammar are
// plain C++ so they can be unit tested without a device attached
// (tests/tst_inputreceiver.cpp). The providers that talk to GameInput, XInput
// and Raw Input live in ReceiverProviders.cpp; the tool's main() lives in
// GameHQInputReceiver.cpp.
//
// The measurement contract, stated once:
//
//   * a provider is only "measurable" if its BASELINE phase saw real device
//     activity while the receiver was a background process — without that,
//     "no events during the exclusive phase" means nothing at all and is
//     reported as not-measurable, never as isolation;
//   * "activity" and "change" are counted separately: a pad that streams
//     reports while it is idle still proves the delivery path is alive, while
//     a button edge proves the payload arrived;
//   * the verdict is per provider, because GameInput's policy binds GameInput
//     clients only. A Raw Input sink that keeps receiving reports during the
//     exclusive phase is the documented compatibility boundary, not a failure
//     of the GameInput measurement.
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace GameReceiver
{

// The three observation paths the receiver offers. The order is stable and is
// what the log, the summary and the tests all iterate in.
enum class Provider
{
    GameInput = 0,
    XInput = 1,
    RawInput = 2,
};
constexpr int kProviderCount = 3;

const char* providerName(Provider provider);
bool providerFromName(const std::string& name, Provider& out);

// One normalized button layout for every provider, so a press reads the same
// whichever API delivered it. Raw Input is the exception on purpose: its
// buttons are reported as raw HID usages, because guessing a per-device button
// map would be inventing evidence.
namespace Button
{
enum Bits : unsigned
{
    DpadUp = 1u << 0,
    DpadDown = 1u << 1,
    DpadLeft = 1u << 2,
    DpadRight = 1u << 3,
    A = 1u << 4,
    B = 1u << 5,
    X = 1u << 6,
    Y = 1u << 7,
    LeftShoulder = 1u << 8,
    RightShoulder = 1u << 9,
    LeftThumb = 1u << 10,
    RightThumb = 1u << 11,
    Menu = 1u << 12,
    View = 1u << 13,
    Guide = 1u << 14,
    Share = 1u << 15,
    PaddleLeft1 = 1u << 16,
    PaddleLeft2 = 1u << 17,
    PaddleRight1 = 1u << 18,
    PaddleRight2 = 1u << 19,
    Misc = 1u << 20,
    Touchpad = 1u << 21,
};
}  // namespace Button

// "+A +Menu -DpadLeft", always in increasing-bit order so two runs with the
// same input produce identical bytes.
std::string buttonNames(unsigned mask);
// Human name of a single bit, for the per-bit diff.
const char* buttonName(unsigned singleBit);

enum AxisIndex
{
    AxisLeftX = 0,
    AxisLeftY = 1,
    AxisRightX = 2,
    AxisRightY = 3,
    AxisLeftTrigger = 4,
    AxisRightTrigger = 5,
};
constexpr int kAxisCount = 6;
const char* axisName(int index);

struct PadState
{
    unsigned buttons = 0;
    float axes[kAxisCount] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
};

// HID hat-switch value -> d-pad bits. Shared by the Raw Input path so its
// decode can be tested without a device. -1 (or any out-of-range value) means
// "centred".
unsigned hatSwitchToButtons(long hatValue);

// Meaningful-change detection with hysteresis: a button edge or an axis move
// past `epsilon` is a transition, everything else is noise. A pad streaming
// reports at 250 Hz must not turn into 250 log lines a second.
class TransitionDetector
{
public:
    explicit TransitionDetector(float epsilon = 0.12f);

    // "" when nothing noteworthy changed, "initial ..." on the first sample,
    // otherwise "+A -DpadLeft lx=+0.55".
    std::string update(const PadState& state);

    unsigned buttons() const { return m_buttons; }
    bool seen() const { return m_seen; }

private:
    bool m_seen = false;
    unsigned m_buttons = 0;
    float m_axes[kAxisCount] = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
    float m_epsilon;
};

// ---------------------------------------------------------------------------
// Session timeline. One "phase" is the window between two phase markers; the
// receiver never guesses what a phase means, the harness names them.
// ---------------------------------------------------------------------------
struct ProviderPhaseStats
{
    long long arrivals = 0;       // reports/state updates delivered to us
    long long transitions = 0;    // meaningful button/axis changes
    long long systemButtons = 0;  // Guide/Share deliveries (GameInput only)
    long long presence = 0;       // device arrival/removal notices
    double firstArrivalSeconds = 0.0;
    double lastArrivalSeconds = 0.0;
};

struct Phase
{
    std::string name;
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    ProviderPhaseStats providers[kProviderCount];
    bool open = true;
};

class Session
{
public:
    // Closes the current phase and opens `name` at `seconds`. Called for every
    // new non-empty line of the phase file.
    void markPhase(const std::string& name, double seconds);
    // Closes the last phase at the end of the run.
    void closeAt(double seconds);

    void noteArrival(Provider provider, double seconds);
    void noteTransition(Provider provider, double seconds);
    void noteSystemButton(Provider provider, double seconds);
    void notePresence(Provider provider, double seconds);

    const std::vector<Phase>& phases() const { return m_phases; }
    const Phase* phaseByName(const std::string& name) const;
    double endSeconds() const { return m_sessionEnd; }
    double durationOf(const Phase& phase) const;

    // Per-second rates. A zero-length phase reports 0 rather than infinity, so
    // a phase that never got a duration can never look "measurable".
    double arrivalRate(const Phase& phase, Provider provider) const;

private:
    std::vector<Phase> m_phases;
    double m_sessionEnd = 0.0;
};

// ---------------------------------------------------------------------------
// Verdicts. One per provider per run; the wording is the whole point of the
// tool, so it is computed here rather than eyeballed out of the log.
// ---------------------------------------------------------------------------
enum class Verdict
{
    NotMeasurable,        // baseline saw nothing: this arrangement proves nothing
    Continuous,           // activity kept flowing: not blocked for this provider
    BlockedThenResumed,   // stopped during the exclusive phase and came back
    BlockedNoResume,      // stopped and did not come back: suspect, not proof
};
const char* verdictName(Verdict verdict);

struct VerdictInputs
{
    long long baselineArrivals = 0;
    double baselineRate = 0.0;
    double exclusiveRate = 0.0;
    double restoredRate = 0.0;
    long long exclusivePresence = 0;
};

// A rate is "quiet" below kQuietFraction of the baseline rate, "active" above
// it. Kept here, not in the judge body, so the tests can pin the threshold.
constexpr double kQuietFraction = 0.2;
// Below this many baseline arrivals the run is not a measurement at all: a
// couple of stale reports cannot carry a verdict.
constexpr long long kMinBaselineArrivals = 10;

Verdict judge(const VerdictInputs& inputs);

// ---------------------------------------------------------------------------
// Log grammar: one record per line, "key=value" tokens, values quoted when they
// contain anything but [A-Za-z0-9._/:+-#@*] — so a log line can be split on
// spaces and parsed by a harness without a regex, and two identical runs
// produce identical bytes.
// ---------------------------------------------------------------------------
struct LogRecord
{
    double seconds = 0.0;
    std::string phase;
    std::string event;
    std::vector<std::pair<std::string, std::string>> fields;
};

std::string quote(const std::string& value);
std::string formatLogLine(const LogRecord& record);
bool parseLogLine(const std::string& line, LogRecord& out);
// Empty when absent.
std::string fieldOf(const LogRecord& record, const std::string& key);

// The machine-readable tail of a run: one line per phase/provider plus the
// verdict line the harness reads. Both are FIELD VECTORS, not pre-formatted
// text, so they travel through the same LogRecord grammar as every other line
// and a harness only ever has to parse one format.
std::vector<std::pair<std::string, std::string>> summaryFields(const Phase& phase,
                                                                Provider provider,
                                                                double sessionDuration);
// `noteOverride` lets the caller explain a verdict the numbers cannot (a phase
// that was never marked, for instance). Empty means "use the verdict's own
// wording".
std::vector<std::pair<std::string, std::string>> verdictFields(const std::string& baselinePhase,
                                                               const std::string& exclusivePhase,
                                                               const std::string& restoredPhase,
                                                               Provider provider,
                                                               const VerdictInputs& inputs,
                                                               Verdict verdict,
                                                               const std::string& noteOverride =
                                                                   std::string());

}  // namespace GameReceiver
