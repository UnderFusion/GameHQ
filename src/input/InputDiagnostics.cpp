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
        : QStringLiteral("sha256:") + QString::fromLatin1(QCryptographicHash::hash(
            m_replayProfile.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
    QStringList lines{QStringLiteral("GameHQ beta diagnostics v1"),
        QStringLiteral("Build: ") + safe(build),
        QStringLiteral("Windows build: ") + safe(windowsBuild),
        QStringLiteral("Active provider: ") + (m_activeBackend.isEmpty() ? QStringLiteral("none") : safe(m_activeBackend)),
        QStringLiteral("Resolved controller profile: ") + profile,
        QStringLiteral("Effective global.save_replay bindings:")};
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
    return lines.join(QLatin1Char('\n'));
}
