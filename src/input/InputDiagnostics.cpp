#include "input/InputDiagnostics.h"

#include <QRegularExpression>
#include <QCryptographicHash>
#include <QSet>

InputDiagnostics& InputDiagnostics::instance()
{
    static InputDiagnostics diagnostics;
    return diagnostics;
}

InputDiagnostics::InputDiagnostics()
{
    m_clock.start();
}

void InputDiagnostics::setPreviousSessionCrashed(bool crashed)
{
    m_previousSessionCrashed = crashed;
}

void InputDiagnostics::noteBackendSwitch(const QString& backend, const QString& reason)
{
    m_activeBackend = backend;
    push(m_switches, kMaxSwitches, m_clock.elapsed(),
         QStringLiteral("%1 (%2)").arg(backend, reason));
}

void InputDiagnostics::noteDevice(const QString& identity, const QString& description,
                                  const QString& verdict)
{
    if (!m_devices.contains(identity))
        m_deviceOrder.append(identity);
    DeviceInfo& info = m_devices[identity];
    info.description = description;
    info.verdict = verdict;
}

void InputDiagnostics::noteRate(const QString& identity, quint32 eventsPerSecond)
{
    if (!m_devices.contains(identity))
        m_deviceOrder.append(identity);
    DeviceInfo& info = m_devices[identity];
    info.eventsPerSecond = eventsPerSecond;
    info.sawRate = true;
}

void InputDiagnostics::noteControl(const QString& controlId, const QString& backend)
{
    m_activeBackend = backend;
    push(m_controls, kMaxControls, m_clock.elapsed(),
         QStringLiteral("%1 (%2)").arg(controlId, backend));
}

void InputDiagnostics::noteRejectedBinding(const QString& subject, const QString& reason)
{
    push(m_rejectedBindings, kMaxRejectedBindings, m_clock.elapsed(),
         QStringLiteral("%1: %2").arg(subject, reason));
}

void InputDiagnostics::setGestureTiming(const QString& description)
{
    m_gestureTiming = description;
}

void InputDiagnostics::notePattern(const QString& detail)
{
    push(m_patterns, kMaxPatterns, m_clock.elapsed(), detail);
}

void InputDiagnostics::setBoundPatterns(const QStringList& patterns)
{
    m_boundPatterns = patterns;
}

void InputDiagnostics::setGuideObserved(bool observed)
{
    m_guideObserved = observed;
}

void InputDiagnostics::noteForeground(const QString& phase, bool acquired)
{
    push(m_foreground, kMaxForeground, m_clock.elapsed(),
         QStringLiteral("%1 %2").arg(phase,
                                     acquired ? QStringLiteral("acquired")
                                              : QStringLiteral("FAILED")));
}

void InputDiagnostics::setCloakStatus(const QStringList& hiddenPads, bool hidHidePresent)
{
    m_hiddenPads = hiddenPads;
    m_hidHidePresent = hidHidePresent;
}

// ---------------------------------------------------------------- cpo-x01

void InputDiagnostics::setControllerRouting(const QString& servingProvider,
                                            const QString& switchReason,
                                            const QStringList& arbitrationLines)
{
    m_servingProvider = servingProvider;
    m_switchReason = switchReason;
    m_arbitrationLines = arbitrationLines;
}

void InputDiagnostics::setMappingState(
    const QString& runningGameKey,
    const QVector<MappingChainSnapshot>& chains,
    const QVector<StaleAssignmentSnapshot>& staleAssignments)
{
    const bool first = !m_mappingStateSeen;
    if (first || runningGameKey != m_gameKey) {
        // One transition entry per change - a session start, an exit, or a
        // move to another game. Hashed here so no ring ever holds a path.
        push(m_gameSessions, kMaxGameSessions, m_clock.elapsed(),
             runningGameKey.isEmpty()
                 ? QStringLiteral("none")
                 : QStringLiteral("present ") + hashedId(runningGameKey));
    }
    m_mappingStateSeen = true;
    m_gameKey = runningGameKey;
    m_mappingChains = chains;
    m_staleAssignments = staleAssignments;
}

void InputDiagnostics::noteOverlayShow(bool foregroundPreserved)
{
    m_overlayStateSeen = true;
    m_overlayVisible = true;
    m_overlayShowSeen = true;
    m_overlayForegroundPreserved = foregroundPreserved;
    push(m_overlayTransitions, kMaxOverlayTransitions, m_clock.elapsed(),
         foregroundPreserved
             ? QStringLiteral("overlay show | game foreground preserved")
             : QStringLiteral("overlay show | foreground CHANGED (game lost foreground)"));
}

void InputDiagnostics::noteOverlayHide()
{
    m_overlayStateSeen = true;
    m_overlayVisible = false;
    push(m_overlayTransitions, kMaxOverlayTransitions, m_clock.elapsed(),
         QStringLiteral("overlay hide"));
}

QString InputDiagnostics::hashedId(const QString& raw)
{
    if (raw.isEmpty())
        return {};
    return QStringLiteral("sha256:") + QString::fromLatin1(
        QCryptographicHash::hash(raw.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
}

void InputDiagnostics::startProbe(int durationMs)
{
    m_probeStartedMs = m_clock.elapsed();
    m_probeDeadlineMs = m_probeStartedMs + qBound(250, durationMs, 10000);
    m_probeEvents.clear();
    m_probeOverflowed = false;
    m_probeSampled = false;
}

bool InputDiagnostics::probeActive() const
{
    return m_probeDeadlineMs >= 0 && m_clock.elapsed() < m_probeDeadlineMs;
}

bool InputDiagnostics::noteProbeEvent(const QString& identity, const QString& backend,
                                      const QString& detail)
{
    if (!probeActive())
        return false;
    if (m_probeEvents.size() >= kMaxProbeEvents) {
        m_probeOverflowed = true;
        return false;
    }
    push(m_probeEvents, kMaxProbeEvents, m_clock.elapsed() - m_probeStartedMs,
         QStringLiteral("%1 via %2: %3").arg(identity, backend, detail));
    return true;
}

void InputDiagnostics::noteProbeSampled()
{
    if (probeActive())
        m_probeSampled = true;
}

QString InputDiagnostics::probeSummary() const
{
    if (m_probeStartedMs < 0)
        return QStringLiteral("Probe: never run");
    QStringList lines;
    lines << (probeActive() ? QStringLiteral("Probe: RUNNING")
                            : QStringLiteral("Probe: finished"));
    if (m_probeEvents.isEmpty()) {
        lines << QStringLiteral("  no button change reached GameHQ during the window");
    } else {
        for (const Stamped& event : m_probeEvents)
            lines << QStringLiteral("  +%1ms %2").arg(event.ms).arg(event.text);
    }
    if (m_probeOverflowed)
        lines << QStringLiteral("  (event cap reached, further changes dropped)");
    if (m_probeSampled)
        lines << QStringLiteral("  (high report rate: reports were sampled, not read "
                                "one by one — press and hold the button if nothing "
                                "showed up)");
    return lines.join(QLatin1Char('\n'));
}

QString InputDiagnostics::redactDevicePath(const QString& path)
{
    if (path.isEmpty())
        return path;
    // Keep only what a bug report needs to identify the model. Serial numbers,
    // container ids and instance paths all collapse into the hash.
    static const QRegularExpression vidPid(
        QStringLiteral("vid_([0-9a-fA-F]{4}).*?pid_([0-9a-fA-F]{4})"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = vidPid.match(path);
    const QString identity = match.hasMatch()
        ? QStringLiteral("%1:%2").arg(match.captured(1).toUpper(),
                                      match.captured(2).toUpper())
        : QStringLiteral("????:????");
    return QStringLiteral("%1/#%2").arg(
        identity, QString::number(qHash(path), 16).right(8));
}

QString InputDiagnostics::exportText() const
{
    QStringList lines;
    lines << QStringLiteral("Input:");
    lines << QStringLiteral("  Previous session ended unexpectedly: %1")
                 .arg(m_previousSessionCrashed ? QStringLiteral("YES")
                                               : QStringLiteral("no"));
    lines << QStringLiteral("  Active backend: %1")
                 .arg(m_activeBackend.isEmpty() ? QStringLiteral("none")
                                                : m_activeBackend);

    lines << QStringLiteral("  Backend switches:");
    if (m_switches.isEmpty())
        lines << QStringLiteral("    none");
    for (const Stamped& entry : m_switches)
        lines << QStringLiteral("    %1").arg(stamp(entry));

    lines << QStringLiteral("  Devices:");
    if (m_deviceOrder.isEmpty())
        lines << QStringLiteral("    none seen");
    for (const QString& identity : m_deviceOrder) {
        const DeviceInfo& info = m_devices.value(identity);
        QString line = QStringLiteral("    %1 %2 -> %3")
                           .arg(identity, info.description, info.verdict);
        if (info.sawRate)
            line += QStringLiteral(" @ %1 events/s").arg(info.eventsPerSecond);
        lines << line;
    }

    lines << QStringLiteral("  Recent controls:");
    if (m_controls.isEmpty())
        lines << QStringLiteral("    none");
    for (const Stamped& entry : m_controls)
        lines << QStringLiteral("    %1").arg(stamp(entry));

    if (!m_gestureTiming.isEmpty())
        lines << QStringLiteral("  Gesture timing: %1").arg(m_gestureTiming);

    lines << QStringLiteral("  Guide/PS button seen this session: %1")
                 .arg(m_guideObserved ? QStringLiteral("yes")
                                      : QStringLiteral("NO (another app may own it)"));

    lines << QStringLiteral("  Recent patterns:");
    if (m_patterns.isEmpty())
        lines << QStringLiteral("    none");
    for (const Stamped& entry : m_patterns)
        lines << QStringLiteral("    %1").arg(stamp(entry));

    if (!m_boundPatterns.isEmpty()) {
        lines << QStringLiteral("  Controller assignments:");
        for (const QString& pattern : m_boundPatterns)
            lines << QStringLiteral("    %1").arg(pattern);
    }

    if (!m_rejectedBindings.isEmpty()) {
        lines << QStringLiteral("  Rejected binding rows (defaults used instead):");
        for (const Stamped& entry : m_rejectedBindings)
            lines << QStringLiteral("    %1").arg(stamp(entry));
    }

    lines << QStringLiteral("  Overlay foreground:");
    if (m_foreground.isEmpty())
        lines << QStringLiteral("    no overlay open/close this session");
    for (const Stamped& entry : m_foreground)
        lines << QStringLiteral("    %1").arg(stamp(entry));

    if (m_hiddenPads.isEmpty()) {
        lines << QStringLiteral("  Controllers hidden by a HID filter: none");
    } else {
        lines << QStringLiteral("  Controllers hidden by a HID filter (HidHide %1):")
                     .arg(m_hidHidePresent ? QStringLiteral("installed")
                                           : QStringLiteral("not detected"));
        for (const QString& pad : m_hiddenPads)
            lines << QStringLiteral("    %1").arg(pad);
    }

    lines << QStringLiteral("  %1").arg(probeSummary());
    return lines.join(QLatin1Char('\n'));
}

void InputDiagnostics::clear()
{
    m_replayProfile.clear();
    m_replayBindings.clear();
    m_previousSessionCrashed = false;
    m_switches.clear();
    m_controls.clear();
    m_foreground.clear();
    m_rejectedBindings.clear();
    m_gestureTiming.clear();
    m_patterns.clear();
    m_boundPatterns.clear();
    m_guideObserved = false;
    m_devices.clear();
    m_deviceOrder.clear();
    m_hiddenPads.clear();
    m_hidHidePresent = false;
    m_activeBackend.clear();
    m_probeDeadlineMs = -1;
    m_probeStartedMs = -1;
    m_probeEvents.clear();
    m_probeOverflowed = false;
    m_probeSampled = false;
    m_servingProvider.clear();
    m_switchReason.clear();
    m_arbitrationLines.clear();
    m_mappingStateSeen = false;
    m_gameKey.clear();
    m_mappingChains.clear();
    m_staleAssignments.clear();
    m_gameSessions.clear();
    m_overlayStateSeen = false;
    m_overlayVisible = false;
    m_overlayShowSeen = false;
    m_overlayForegroundPreserved = false;
    m_overlayTransitions.clear();
}

void InputDiagnostics::push(QVector<Stamped>& ring, int cap, qint64 ms,
                            const QString& text)
{
    if (ring.size() >= cap)
        ring.removeFirst();
    ring.append({ms, text});
}

QString InputDiagnostics::stamp(const Stamped& entry)
{
    return QStringLiteral("+%1s %2")
        .arg(QString::number(entry.ms / 1000.0, 'f', 1), entry.text);
}

void InputDiagnostics::setReplayBindings(const QString& profile, const QStringList& rows)
{
    m_replayProfile = profile;
    m_replayBindings = rows.mid(0, 8);
}

QString InputDiagnostics::exportBetaText(const QString& build, const QString& windowsBuild,
                                        const QVariantMap& config, const QString& logTail) const
{
    // This export is deliberately an allowlist, not a redacted copy of the log.
    // Unknown fields and free-form failure details never enter the package.
    const auto safe = [](const QString& value) {
        static const QRegularExpression plain(QStringLiteral("^[A-Za-z0-9_.: +(),=-]{1,240}$"));
        return plain.match(value).hasMatch() ? value : QStringLiteral("[redacted]");
    };
    const QString profile = m_replayProfile.isEmpty() ? QStringLiteral("none")
        : hashedId(m_replayProfile);
    QStringList lines{QStringLiteral("GameHQ beta diagnostics v1"),
        QStringLiteral("Build: ") + safe(build),
        QStringLiteral("Windows build: ") + safe(windowsBuild),
        QStringLiteral("Active provider: ") + (m_activeBackend.isEmpty() ? QStringLiteral("none") : safe(m_activeBackend)),
        QStringLiteral("Resolved controller profile: ") + profile};

    // cpo-x01: which provider serves the logical controller and what its
    // arbitration is doing. Provider-local facts only - this package never
    // claims two providers see the same physical device (that question is
    // cpo-c03b's and is unresolved).
    lines << QStringLiteral("Controller routing:");
    lines << QStringLiteral("  serving provider: ") +
        (m_servingProvider.isEmpty()
             ? QStringLiteral("unavailable (no provider switch observed)")
             : safe(m_servingProvider));
    if (!m_switchReason.isEmpty())
        lines << QStringLiteral("  last switch reason: ") + safe(m_switchReason);
    if (m_arbitrationLines.isEmpty()) {
        lines << QStringLiteral("  arbitration: unavailable (no routing transition observed)");
    } else {
        for (const QString& line : m_arbitrationLines)
            lines << QStringLiteral("  ") + safe(line);
    }

    // cpo-x01: what each mapping route currently serves, why, and which broken
    // assignment rows were skipped. Profiles, game keys and assignment target
    // keys are paths or identity strings, so they enter only as pseudonyms.
    lines << QStringLiteral("Mapping state:");
    lines << QStringLiteral("  game context: ") +
        (!m_mappingStateSeen
             ? QStringLiteral("unavailable (no mapping refresh observed)")
             : (m_gameKey.isEmpty() ? QStringLiteral("none")
                                    : QStringLiteral("present (key ") + hashedId(m_gameKey) + QStringLiteral(")")));
    if (m_mappingChains.isEmpty()) {
        lines << QStringLiteral("  routes: none tracked");
    } else {
        static const QStringList sources{QStringLiteral("game"), QStringLiteral("controller"),
            QStringLiteral("group_default"), QStringLiteral("builtin"),
            QStringLiteral("materialized"), QStringLiteral("migration_bridge"),
            QStringLiteral("local_legacy")};
        QStringList rendered;
        for (const MappingChainSnapshot& chain : m_mappingChains) {
            rendered << QStringLiteral("    %1%2 -> source=%3 preset=%4 owned=%5 fp=%6")
                            .arg(safe(chain.deviceGroup),
                                 chain.profile.isEmpty() ? QString()
                                                         : QStringLiteral(" profile ") + hashedId(chain.profile),
                                 sources.contains(chain.source) ? chain.source : QStringLiteral("[redacted]"),
                                 chain.presetId.isEmpty() ? QStringLiteral("none") : safe(chain.presetId),
                                 chain.owned ? QStringLiteral("yes") : QStringLiteral("no"),
                                 chain.fingerprint.isEmpty() ? QStringLiteral("none") : safe(chain.fingerprint));
        }
        rendered.sort();
        const int omitted = rendered.size() - kMaxMappingChains;
        if (omitted > 0)
            rendered = rendered.mid(0, kMaxMappingChains)
                + QStringList{QStringLiteral("    ... %1 more route(s)").arg(omitted)};
        lines << QStringLiteral("  routes:");
        lines << rendered;
    }
    if (!m_staleAssignments.isEmpty()) {
        static const QStringList reasons{QStringLiteral("missing_preset"),
                                         QStringLiteral("wrong_group")};
        static const QStringList kinds{QStringLiteral("game"), QStringLiteral("controller"),
                                       QStringLiteral("legacy_slot"), QStringLiteral("group_default")};
        QStringList stale;
        for (const StaleAssignmentSnapshot& report : m_staleAssignments) {
            stale << QStringLiteral("    %1 %2 %3 preset=%4 key=%5")
                         .arg(safe(report.deviceGroup),
                              kinds.contains(report.targetKind) ? report.targetKind : QStringLiteral("[redacted]"),
                              reasons.contains(report.reason) ? report.reason : QStringLiteral("[redacted]"),
                              report.presetId.isEmpty() ? QStringLiteral("none") : safe(report.presetId),
                              report.targetKey.isEmpty() ? QStringLiteral("none") : hashedId(report.targetKey));
        }
        stale.sort();
        const int omitted = stale.size() - kMaxStaleAssignments;
        if (omitted > 0)
            stale = stale.mid(0, kMaxStaleAssignments)
                + QStringList{QStringLiteral("    ... %1 more").arg(omitted)};
        lines << QStringLiteral("  stale assignments (skipped, next winner served):");
        lines << stale;
    }

    lines << QStringLiteral("Effective global.save_replay bindings:");
    if (m_replayBindings.isEmpty()) lines << QStringLiteral("  unavailable (no controller profile observed)");
    for (const auto& row : m_replayBindings) lines << QStringLiteral("  ") + safe(row);
    lines << QStringLiteral("Relevant configuration:");
    for (const auto& key : {"replay.auto", "replay.clip_notify", "replay.clip_sound",
                           "replay.length_seconds", "replay.manual_idle_s",
                           "input.default_hold_ms", "input.multi_tap_interval_ms"}) {
        const QVariant value = config.value(QLatin1String(key));
        bool ok = false;
        const int number = value.toInt(&ok);
        lines << QStringLiteral("  %1=%2").arg(QLatin1String(key),
                    ok ? QString::number(number) : QStringLiteral("unavailable"));
    }
    const QString mode = config.value(QStringLiteral("capture.mode")).toString();
    lines << QStringLiteral("  capture.mode=") +
        (QStringList{"always", "whitelist", "only_in_games"}.contains(mode) ? mode : QStringLiteral("unavailable"));
    lines << QStringLiteral("Controller replay trace (last 64 KiB, at most 32 events; receipt is not publication):");
    static const QRegularExpression event(QStringLiteral(
        "ReplaySave\\[([0-9]+)(?: src=([a-z]+) \\+([0-9]+)ms)?\\]: (accepted|armed|frozen|exporting|published|failed|request begin|worker resumed|remux ok)\\b"));
    const auto tailLines = logTail.right(kMaxTraceBytes).split(QLatin1Char('\n'));
    QSet<QString> controllerIds;
    for (const auto& line : tailLines) {
        const auto match = event.match(line);
        if (match.hasMatch() && match.captured(2) == QLatin1String("controller"))
            controllerIds.insert(match.captured(1));
    }
    QStringList trace;
    for (const auto& line : tailLines) {
        const auto match = event.match(line);
        if (!match.hasMatch() || !controllerIds.contains(match.captured(1))) continue;
        if (!match.captured(2).isEmpty() && match.captured(2) != QLatin1String("controller")) continue;
        QString stage = match.captured(4);
        if (stage == QLatin1String("failed")) {
            const QString detail = line.mid(match.capturedEnd());
            if (detail.startsWith(" - A replay save is already in progress")) stage += " (busy)";
            else if (detail.startsWith(" - Replay buffer")) stage += " (buffer not ready)";
            else if (detail.startsWith(" - Replay capture is paused for an update")) stage += " (update paused)";
            else stage += " (details omitted)";
        }
        trace << QStringLiteral("  request=%1 %2").arg(match.captured(1), stage);
        if (trace.size() > kMaxTraceEvents) trace.removeFirst();
    }
    lines << (trace.isEmpty() ? QStringList{QStringLiteral("  unavailable: no controller requests in readable log tail")}: trace);

    // cpo-x01: overlay/focus state. Two stamped timelines - game-session
    // transitions and overlay show/hide - are what separates "the overlay was
    // open" from "the game session actually changed".
    lines << QStringLiteral("Overlay/focus:");
    lines << QStringLiteral("  overlay visible: ") +
        (!m_overlayStateSeen ? QStringLiteral("unavailable (no overlay activity this session)")
                             : (m_overlayVisible ? QStringLiteral("yes") : QStringLiteral("no")));
    lines << QStringLiteral("  game foreground preserved on show: ") +
        (!m_overlayShowSeen
             ? QStringLiteral("unavailable (no overlay show this session)")
             : (m_overlayForegroundPreserved
                    ? QStringLiteral("yes")
                    : QStringLiteral("no (the foreground changed during show)")));
    lines << QStringLiteral("  game session transitions:");
    if (m_gameSessions.isEmpty())
        lines << QStringLiteral("    unavailable (no mapping refresh observed)");
    for (const Stamped& entry : m_gameSessions)
        lines << QStringLiteral("    %1").arg(stamp(entry));
    lines << QStringLiteral("  overlay show/hide:");
    if (m_overlayTransitions.isEmpty())
        lines << QStringLiteral("    no overlay open/close this session");
    for (const Stamped& entry : m_overlayTransitions)
        lines << QStringLiteral("    %1").arg(stamp(entry));
    return lines.join(QLatin1Char('\n'));
}
