#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QVariantMap>
#include <QtGlobal>

// Per-window payload-read budget for the "press your button now" probe.
//
// The probe is explicitly requested, lasts three seconds and only ever stores
// changed-bit summaries, so its cost is worth spending on completeness: up to
// kReadsPerSlice reads per kSliceMs — 10 000 reports a second — are read in
// full. A 4 kHz pad (12 000 reports per window) and an 8 kHz pad (24 000) are
// therefore covered report for report, and a 20 ms tap cannot fall into a gap.
//
// Above that rate — several flooding devices at once — the budget switches to
// *evenly strided* sampling: every Nth report, with N recomputed each slice
// from the previous slice's observed rate. Never "read the first N then go
// blind", which is what leaves a tap unseen. The per-slice ceiling is the only
// bound, deliberately: a separate whole-window cap would be spent before the
// end of the window and blind the probe exactly where the user is most likely
// to press. It bounds the window by construction anyway — 10 000 reads per
// second of probe, so 30 000 for the standard three seconds. Any skipped
// report sets sampled(), which the summary surfaces, so a probe that sampled
// never reads as one that saw everything.
class ProbeReadBudget
{
public:
    static constexpr qint64 kSliceMs = 100;
    static constexpr int kReadsPerSlice = 1000;   // 10 000 reports/s read in full

    void reset()
    {
        m_slice = -1;
        m_eventsInSlice = 0;
        m_readsInSlice = 0;
        m_stride = 1;
        m_reads = 0;
        m_sampled = false;
    }

    // True when this payload may be read.
    bool allow(qint64 nowMs)
    {
        const qint64 slice = nowMs / kSliceMs;
        if (slice != m_slice) {
            // The rate this device family showed in the slice that just ended
            // decides how much of the next one can be read. One slice of lag
            // is fine: report rates do not change within a probe window.
            m_stride = m_eventsInSlice > kReadsPerSlice
                ? (m_eventsInSlice + kReadsPerSlice - 1) / kReadsPerSlice
                : 1;
            m_slice = slice;
            m_eventsInSlice = 0;
            m_readsInSlice = 0;
        }
        const int index = m_eventsInSlice++;
        if (m_stride > 1 && (index % m_stride) != 0) {
            m_sampled = true;
            return false;
        }
        // Ceiling for the one slice the stride cannot yet know about: the very
        // first of a window, where no previous rate has been observed. Only a
        // flood above 10 000 reports/s ever reaches it, and only in the first
        // 100 ms — before a user could have reacted to "press your button now".
        if (m_readsInSlice >= kReadsPerSlice) {
            m_sampled = true;
            return false;
        }
        ++m_readsInSlice;
        ++m_reads;
        return true;
    }

    // True once any report was skipped — never true while coverage was full.
    bool sampled() const { return m_sampled; }
    int reads() const { return m_reads; }

private:
    qint64 m_slice = -1;
    int m_eventsInSlice = 0;
    int m_readsInSlice = 0;
    int m_stride = 1;
    int m_reads = 0;
    bool m_sampled = false;
};

// Process-wide sink for the input-stack facts a bug report needs: which
// backend was active and why it changed, what devices Windows offered and how
// they were classified, aggregated event rates, the last logical controls
// delivered, overlay foreground acquisition results, HidHide cloak state, and
// whether the previous session died. AppController appends exportText() to the
// copied diagnostic summary, so one click gives a paste-ready block.
//
// Everything here is bounded: fixed-size rings, no per-event allocation once a
// device is known, and the button probe hard-caps its recorded events. Nothing
// privacy-sensitive leaves this class — device paths are reduced to VID/PID
// plus a short stable hash (no serials, no instance ids), and raw report bytes
// are only ever summarized as changed-bit positions inside the explicit probe
// window.
class InputDiagnostics
{
public:
    static InputDiagnostics& instance();

    InputDiagnostics();

    void setPreviousSessionCrashed(bool crashed);
    void noteBackendSwitch(const QString& backend, const QString& reason);
    // Classification registry, keyed by "VVVV:PPPP" identity. Re-noting the
    // same identity overwrites — a replug that changes the verdict should show
    // the latest one.
    void noteDevice(const QString& identity, const QString& description,
                    const QString& verdict);
    void noteRate(const QString& identity, quint32 eventsPerSecond);
    void noteControl(const QString& controlId, const QString& backend);
    // phase: "overlay show" / "overlay hide".
    void noteForeground(const QString& phase, bool acquired);
    // A stored binding row did not survive validation and was skipped, so the
    // action fell back to its default. Silent skipping would read as "the app
    // forgot my binding"; this is what turns that into an answerable report.
    void noteRejectedBinding(const QString& subject, const QString& reason);
    // The live gesture timing, so a report about "the button feels slow" comes
    // with the numbers that decided how long it waited.
    void setGestureTiming(const QString& description);
    // Recognizer traffic: which pattern fired, and the chord lifecycle around
    // it (opened / completed / timed out). Control ids only - no report bytes,
    // no device paths.
    void notePattern(const QString& detail);
    // The effective controller assignments, in canonical serialized form, so a
    // report shows what was bound and not just what happened.
    void setBoundPatterns(const QStringList& patterns);
    // Whether the Guide/PS button has ever reached GameHQ this session. The
    // usual reason a combination never fires is that another app owns it.
    void setGuideObserved(bool observed);
    void setCloakStatus(const QStringList& hiddenPads, bool hidHidePresent);

    // "Press your screenshot button now": for `durationMs` the backends may
    // report per-event detail through noteProbeEvent. Returns immediately;
    // callers check probeActive() on their hot path (one branch when idle).
    void startProbe(int durationMs = kProbeDurationMs);
    bool probeActive() const;
    // Returns false once the event cap is reached — callers stop reporting
    // (and stop reading payloads) for the rest of the window.
    bool noteProbeEvent(const QString& identity, const QString& backend,
                        const QString& detail);
    // A backend had to skip payload reads to stay inside its budget: the
    // window still covered its full duration, but by sampling.
    void noteProbeSampled();
    QString probeSummary() const;

    // VID/PID survive, the rest of the path collapses to a short stable hash.
    static QString redactDevicePath(const QString& path);

    QString exportText() const;
    void setReplayBindings(const QString& profile, const QStringList& rows);
    QString exportBetaText(const QString& build, const QString& windowsBuild,
                           const QVariantMap& config, const QString& logTail) const;

    // ---------------------------------------------------------------- cpo-x01
    // Current-state snapshots for the one-click export: which provider serves
    // the logical controller and what its arbitration is doing, what each
    // mapping route currently serves, and the overlay/focus state. These are
    // bounded snapshots - one value per fact plus two small rings - never
    // per-event telemetry; the owning components push them at their existing
    // low-frequency transitions, and the export renders them through the same
    // allowlist and hashing rules as the rest of the package.
    struct MappingChainSnapshot {
        QString deviceGroup;   // "controller" | "keyboard" | "mouse"
        QString profile;       // raw logical profile; hashed in the export
        QString source;        // canonical source label; allowlisted in the export
        QString presetId;
        bool owned = false;
        QString fingerprint;   // served table content fingerprint
    };
    struct StaleAssignmentSnapshot {
        QString deviceGroup;
        QString targetKind;    // "game" | "controller" | "legacy_slot"
        QString targetKey;     // raw; hashed in the export
        QString presetId;
        QString reason;        // "missing_preset" | "wrong_group"
    };
    // servingProvider: the backend currently serving the logical controller
    // ("Sony controller", "GameInput shadow", ... or "none"); switchReason: why
    // that role last moved; arbitrationLines: canonical engine-built facts
    // (route identity, pending candidate, last-control ages, suppression
    // counters) - each still passed through the export allowlist.
    void setControllerRouting(const QString& servingProvider, const QString& switchReason,
                              const QStringList& arbitrationLines);
    // runningGameKey is the raw canonical key (a full executable path while the
    // game runs); the export hashes it and reports presence only, and a change
    // pushes one stamped game-session transition entry.
    void setMappingState(const QString& runningGameKey,
                         const QVector<MappingChainSnapshot>& chains,
                         const QVector<StaleAssignmentSnapshot>& staleAssignments);
    // A real overlay open/close, recorded by the production show/hide path.
    // Show carries the observed OS fact - did the game window keep the
    // foreground through presentation - and never the non-activating policy
    // flag; hide makes no foreground claim and therefore takes no value.
    //
    // cpo-o06a: `trace` is the one-line OverlayFocus record for that
    // transition (overlay/OverlayFocusTrace.h) - already sanitized by its
    // caller. It is optional so the existing bool-only contract, and the tests
    // that pin it, keep working unchanged.
    void noteOverlayShow(bool foregroundPreserved, const QString& trace = QString());
    void noteOverlayHide(const QString& trace = QString());

    // ---------------------------------------------------------------- cpo-o06a
    // The GameInput focus policy the runtime is actually running under, in
    // words ("background input + guide + share"). Pushed by the component that
    // applies it, so the export states the policy in force rather than the one
    // the code intended. Stays empty when GameInput is not active.
    void setGameInputFocusPolicy(const QString& description);
    QString gameInputFocusPolicy() const { return m_gameInputFocusPolicy; }
    // cpo-o06c: the focus policy has exactly one owner
    // (GameInputFocusController); every transition it makes is recorded here with
    // its reason, so a report shows the policy TIMELINE rather than only the
    // policy in force at the moment the export happened to be written. Bounded.
    void noteGameInputFocusTransition(const QString& mode, const QString& reason);
    // Read-back for the overlay trace: the provider currently serving the
    // logical controller, and its profile as the pseudonym the export uses -
    // never the raw profile string.
    QString servingProvider() const { return m_servingProvider; }
    QString controllerProfileId() const;
    static constexpr int kMaxTraceBytes = 64 * 1024;
    static constexpr int kMaxTraceEvents = 32;
    void clear();

    static constexpr int kProbeDurationMs = 3000;
    static constexpr int kMaxSwitches = 32;
    static constexpr int kMaxControls = 16;
    static constexpr int kMaxForeground = 8;
    static constexpr int kMaxRejectedBindings = 16;
    static constexpr int kMaxPatterns = 16;
    static constexpr int kMaxProbeEvents = 64;
    static constexpr int kMaxMappingChains = 8;
    static constexpr int kMaxStaleAssignments = 8;
    static constexpr int kMaxGameSessions = 8;
    static constexpr int kMaxOverlayTransitions = 8;
    // cpo-o06c: mode + reason per application of the GameInput focus policy.
    // Bounded: the policy changes at most twice per overlay open/close.
    static constexpr int kMaxGameInputFocusTransitions = 16;

private:
    struct Stamped {
        qint64 ms = 0;
        QString text;
    };
    struct DeviceInfo {
        QString description;
        QString verdict;
        quint32 eventsPerSecond = 0;
        bool sawRate = false;
    };

    static void push(QVector<Stamped>& ring, int cap, qint64 ms, const QString& text);
    static QString stamp(const Stamped& entry);
    // Privacy boundary for identity-like values (logical profiles, game keys,
    // assignment target keys): a stable short pseudonym, never the raw value.
    static QString hashedId(const QString& raw);

    QElapsedTimer m_clock;
    bool m_previousSessionCrashed = false;
    QVector<Stamped> m_switches;
    QVector<Stamped> m_controls;
    QVector<Stamped> m_foreground;
    QVector<Stamped> m_rejectedBindings;
    QString m_gestureTiming;
    QVector<Stamped> m_patterns;
    QStringList m_boundPatterns;
    QString m_replayProfile;
    QStringList m_replayBindings;
    bool m_guideObserved = false;
    QHash<QString, DeviceInfo> m_devices;
    QStringList m_deviceOrder;            // insertion order for stable export
    QStringList m_hiddenPads;
    bool m_hidHidePresent = false;
    QString m_activeBackend;
    qint64 m_probeDeadlineMs = -1;
    qint64 m_probeStartedMs = -1;
    QVector<Stamped> m_probeEvents;
    bool m_probeOverflowed = false;
    bool m_probeSampled = false;

    // ---------------------------------------------------------------- cpo-x01
    QString m_servingProvider;
    QString m_switchReason;
    QStringList m_arbitrationLines;
    bool m_mappingStateSeen = false;
    QString m_gameKey;   // raw canonical key; hashed in the export
    QVector<MappingChainSnapshot> m_mappingChains;
    QVector<StaleAssignmentSnapshot> m_staleAssignments;
    QVector<Stamped> m_gameSessions;   // hashed keys only
    bool m_overlayStateSeen = false;
    bool m_overlayVisible = false;
    bool m_overlayShowSeen = false;      // a show ran, so preservation was observed
    bool m_overlayForegroundPreserved = false;
    QVector<Stamped> m_overlayTransitions;   // overlay show/hide only
    // cpo-o06c: GameInput focus-policy transitions (mode + reason), bounded.
    QVector<Stamped> m_gameInputFocusTransitions;
    // cpo-o06a: the most recent full focus/controller record of each kind.
    // One per transition, kept as the already-formatted line.
    QString m_overlayShowTrace;
    QString m_overlayHideTrace;
    QString m_gameInputFocusPolicy;
};
