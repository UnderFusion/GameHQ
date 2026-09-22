// cpo-o06d: the three observation paths of the external receiver.
//
// All three answer the same question from a process GameHQ does not control:
// "did the pad still reach a second app while the overlay held the verified
// foreground and GameHQ had requested GameInput's exclusive-foreground policy?"
//
//   * GameInput  — another GameInput client, deliberately asking the runtime for
//                  BACKGROUND input, so a gap during the exclusive phase means
//                  the policy was applied to us rather than us merely losing the
//                  foreground. This is the measurement that matters.
//   * XInput     — the API most games actually use. It cannot be affected by
//                  GameInput's policy; it defines the compatibility boundary.
//   * Raw Input  — a RIDEV_INPUTSINK registration, i.e. the strongest background
//                  observation Windows offers. If reports keep arriving here
//                  while GameInput goes quiet, that is an honest boundary, not a
//                  failure of the GameInput experiment.
//
// Nothing here injects, hooks, hides or modifies anything: the receiver is a
// plain input consumer, and it is developer-only (never part of the installer).
#pragma once

#include "ReceiverModel.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace GameReceiver
{

// What a provider needs from the run itself: the monotonic clock the log and
// the session share, the counters, and the log sink.
class ReceiverRun
{
public:
    virtual ~ReceiverRun() = default;
    virtual double nowSeconds() const = 0;
    virtual void log(const std::string& event,
                     const std::vector<std::pair<std::string, std::string>>& fields) = 0;

    // Session updates go through the run, not through the Session directly:
    // GameInput delivers on a runtime thread, XInput and Raw Input have their
    // own, and a phase boundary must not be attributed by whichever thread
    // happened to wake last. These four are serialized by the run.
    virtual void noteArrival(Provider provider, double seconds) = 0;
    virtual void noteTransition(Provider provider, double seconds) = 0;
    virtual void noteSystemButton(Provider provider, double seconds) = 0;
    virtual void notePresence(Provider provider, double seconds) = 0;
};

// Counters a provider has accumulated, handed to the heartbeat ticker.
struct ProviderCounters
{
    long long arrivals = 0;
    long long transitions = 0;
    long long systemButtons = 0;
    long long presence = 0;
};

class ReceiverProvider
{
public:
    virtual ~ReceiverProvider() = default;
    virtual const char* name() const = 0;

    // false means this path is unavailable on this machine (no runtime, no
    // hid.dll, ...). The reason is logged and the other paths keep running:
    // one absent API must not turn into an unmeasurable run.
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    // "device=054c:0ce6" / "decode=hid" / "policy=background-input", for the
    // provider log line.
    virtual std::string statusDetail() const = 0;
    // Counters since the previous call (the heartbeat ticker owns the cadence).
    virtual ProviderCounters takeCounters() = 0;
};

std::unique_ptr<ReceiverProvider> makeGameInputProvider(ReceiverRun& run);
std::unique_ptr<ReceiverProvider> makeXInputProvider(ReceiverRun& run);
// The Raw Input registration: (usage page, usage) pairs, top-level collections
// only. Empty means the game-control default (page 0x01, usages 0x04/0x05/0x08).
struct RawUsage
{
    unsigned page = 0;
    unsigned usage = 0;
};

std::unique_ptr<ReceiverProvider> makeRawInputProvider(ReceiverRun& run,
                                                       const std::vector<RawUsage>& usages);

// The foreground window as the receiver sees it, attached to every log line
// that carries a policy-relevant moment.
struct ForegroundInfo
{
    void* window = nullptr;
    unsigned long processId = 0;
    std::string title;
};

ForegroundInfo foregroundSnapshot();
std::vector<std::pair<std::string, std::string>> foregroundFields(const ForegroundInfo& info,
                                                                  bool selfIsForeground);

}  // namespace GameReceiver
