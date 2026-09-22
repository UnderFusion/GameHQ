// cpo-o06d: implementation of the receiver's pure half. See ReceiverModel.h for
// the contract; this file must stay free of Windows and Qt so the unit tests can
// exercise it without a device.
#include "ReceiverModel.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace GameReceiver
{

namespace
{

const char* const kProviderNames[kProviderCount] = { "gameinput", "xinput", "rawinput" };

// One name per normalized bit, in the same order as the enum, so buttonNames()
// is stable and the diff reads in increasing-bit order.
struct ButtonName
{
    unsigned bit;
    const char* name;
};

const ButtonName kButtonNames[] = {
    { Button::DpadUp, "DpadUp" },           { Button::DpadDown, "DpadDown" },
    { Button::DpadLeft, "DpadLeft" },       { Button::DpadRight, "DpadRight" },
    { Button::A, "A" },                     { Button::B, "B" },
    { Button::X, "X" },                     { Button::Y, "Y" },
    { Button::LeftShoulder, "L1" },         { Button::RightShoulder, "R1" },
    { Button::LeftThumb, "L3" },            { Button::RightThumb, "R3" },
    { Button::Menu, "Menu" },               { Button::View, "View" },
    { Button::Guide, "Guide" },             { Button::Share, "Share" },
    { Button::PaddleLeft1, "PaddleL1" },    { Button::PaddleLeft2, "PaddleL2" },
    { Button::PaddleRight1, "PaddleR1" },   { Button::PaddleRight2, "PaddleR2" },
    { Button::Misc, "Misc" },               { Button::Touchpad, "Touchpad" },
};

const char* const kAxisNames[kAxisCount] = { "lx", "ly", "rx", "ry", "lt", "rt" };

// Values we are willing to print bare; everything else gets quoted so the
// space-separated grammar stays parseable.
bool isBareValue(const std::string& value)
{
    if (value.empty())
        return false;
    for (char ch : value) {
        const bool allowed = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == '/' || ch == ':'
            || ch == '+' || ch == '-' || ch == '#' || ch == '@' || ch == '*';
        if (!allowed)
            return false;
    }
    return true;
}

std::string format(const char* pattern, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, pattern);
    const int written = std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    if (written <= 0)
        return std::string();
    return std::string(buffer, static_cast<size_t>(written) < sizeof(buffer)
                                    ? static_cast<size_t>(written)
                                    : sizeof(buffer) - 1);
}

std::string formatAxisValue(float value)
{
    return format("%+.2f", static_cast<double>(value));
}

}  // namespace

const char* providerName(Provider provider)
{
    const int index = static_cast<int>(provider);
    if (index < 0 || index >= kProviderCount)
        return "unknown";
    return kProviderNames[index];
}

bool providerFromName(const std::string& name, Provider& out)
{
    for (int index = 0; index < kProviderCount; ++index) {
        if (name == kProviderNames[index]) {
            out = static_cast<Provider>(index);
            return true;
        }
    }
    return false;
}

const char* buttonName(unsigned singleBit)
{
    for (const ButtonName& entry : kButtonNames) {
        if (entry.bit == singleBit)
            return entry.name;
    }
    return "?";
}

std::string buttonNames(unsigned mask)
{
    std::string result;
    for (const ButtonName& entry : kButtonNames) {
        if ((mask & entry.bit) == 0)
            continue;
        if (!result.empty())
            result += ' ';
        result += '+';
        result += entry.name;
    }
    return result;
}

const char* axisName(int index)
{
    if (index < 0 || index >= kAxisCount)
        return "?";
    return kAxisNames[index];
}

unsigned hatSwitchToButtons(long hatValue)
{
    // HID hat switch: 0 = up, clockwise in 45-degree steps, 8 = up-left, -1 or
    // anything out of range = centred.
    if (hatValue < 0 || hatValue > 8)
        return 0;
    unsigned mask = 0;
    if (hatValue == 0 || hatValue == 1 || hatValue == 7)
        mask |= Button::DpadUp;
    if (hatValue >= 1 && hatValue <= 3)
        mask |= Button::DpadRight;
    if (hatValue >= 3 && hatValue <= 5)
        mask |= Button::DpadDown;
    if (hatValue >= 5 && hatValue <= 7)
        mask |= Button::DpadLeft;
    return mask;
}

TransitionDetector::TransitionDetector(float epsilon) : m_epsilon(epsilon) {}

std::string TransitionDetector::update(const PadState& state)
{
    std::string diff;
    if (!m_seen) {
        m_seen = true;
        m_buttons = state.buttons;
        for (int axis = 0; axis < kAxisCount; ++axis)
            m_axes[axis] = state.axes[axis];
        diff = "initial";
        const std::string held = buttonNames(state.buttons);
        if (!held.empty())
            diff += ' ' + held;
        return diff;
    }

    const unsigned pressed = state.buttons & ~m_buttons;
    const unsigned released = m_buttons & ~state.buttons;
    for (const ButtonName& entry : kButtonNames) {
        if ((pressed & entry.bit) != 0) {
            if (!diff.empty())
                diff += ' ';
            diff += '+';
            diff += entry.name;
        }
    }
    for (const ButtonName& entry : kButtonNames) {
        if ((released & entry.bit) != 0) {
            if (!diff.empty())
                diff += ' ';
            diff += '-';
            diff += entry.name;
        }
    }

    for (int axis = 0; axis < kAxisCount; ++axis) {
        const float value = state.axes[axis];
        if (std::fabs(value - m_axes[axis]) < m_epsilon)
            continue;
        if (!diff.empty())
            diff += ' ';
        diff += axisName(axis);
        diff += '=';
        diff += formatAxisValue(value);
    }

    m_buttons = state.buttons;
    for (int axis = 0; axis < kAxisCount; ++axis)
        m_axes[axis] = state.axes[axis];
    return diff;
}

void Session::markPhase(const std::string& name, double seconds)
{
    if (!m_phases.empty() && m_phases.back().open) {
        m_phases.back().endSeconds = seconds;
        m_phases.back().open = false;
    }
    Phase phase;
    phase.name = name;
    phase.startSeconds = seconds;
    phase.open = true;
    m_phases.push_back(phase);
    m_sessionEnd = seconds;
}

void Session::closeAt(double seconds)
{
    if (!m_phases.empty() && m_phases.back().open) {
        m_phases.back().endSeconds = seconds;
        m_phases.back().open = false;
    }
    m_sessionEnd = seconds;
}

const Phase* Session::phaseByName(const std::string& name) const
{
    for (const Phase& phase : m_phases) {
        if (phase.name == name)
            return &phase;
    }
    return nullptr;
}

double Session::durationOf(const Phase& phase) const
{
    const double end = phase.open ? m_sessionEnd : phase.endSeconds;
    return end > phase.startSeconds ? end - phase.startSeconds : 0.0;
}

double Session::arrivalRate(const Phase& phase, Provider provider) const
{
    const double duration = durationOf(phase);
    if (duration <= 0.0)
        return 0.0;
    return static_cast<double>(phase.providers[static_cast<int>(provider)].arrivals) / duration;
}

void Session::noteArrival(Provider provider, double seconds)
{
    if (m_phases.empty())
        markPhase("startup", seconds);
    Phase& phase = m_phases.back();
    ProviderPhaseStats& stats = phase.providers[static_cast<int>(provider)];
    if (stats.arrivals == 0)
        stats.firstArrivalSeconds = seconds;
    stats.lastArrivalSeconds = seconds;
    ++stats.arrivals;
    m_sessionEnd = seconds;
}

void Session::noteTransition(Provider provider, double seconds)
{
    noteArrival(provider, seconds);
    ++m_phases.back().providers[static_cast<int>(provider)].transitions;
}

void Session::noteSystemButton(Provider provider, double seconds)
{
    noteArrival(provider, seconds);
    ++m_phases.back().providers[static_cast<int>(provider)].systemButtons;
}

void Session::notePresence(Provider provider, double seconds)
{
    if (m_phases.empty())
        markPhase("startup", seconds);
    ++m_phases.back().providers[static_cast<int>(provider)].presence;
}

const char* verdictName(Verdict verdict)
{
    switch (verdict) {
    case Verdict::NotMeasurable:
        return "not-measurable";
    case Verdict::Continuous:
        return "continuous";
    case Verdict::BlockedThenResumed:
        return "blocked-then-resumed";
    case Verdict::BlockedNoResume:
        return "blocked-no-resume";
    }
    return "unknown";
}

Verdict judge(const VerdictInputs& inputs)
{
    // No baseline activity means the arrangement cannot tell "blocked" from
    // "this process never gets background input in the first place". Reported
    // as not-measurable on purpose: an unmeasurable provider must never be
    // dressed up as an isolated one.
    if (inputs.baselineArrivals < kMinBaselineArrivals || inputs.baselineRate <= 0.0)
        return Verdict::NotMeasurable;

    if (inputs.exclusiveRate >= kQuietFraction * inputs.baselineRate)
        return Verdict::Continuous;

    // Quiet during the exclusive phase. Only a return to activity afterwards
    // shows the policy was scoped rather than the device having gone away; a
    // device add/remove in that window is the other explanation and is called
    // out instead of being claimed as isolation.
    if (inputs.exclusivePresence > 0)
        return Verdict::BlockedNoResume;
    if (inputs.restoredRate >= kQuietFraction * inputs.baselineRate)
        return Verdict::BlockedThenResumed;
    return Verdict::BlockedNoResume;
}

std::string quote(const std::string& value)
{
    if (value.empty() || isBareValue(value))
        return value;
    std::string result = "\"";
    for (char ch : value) {
        if (ch == '"' || ch == '\\')
            result += '\\';
        if (ch == '\n' || ch == '\r')
            result += ' ';
        else
            result += ch;
    }
    result += '"';
    return result;
}

std::string formatLogLine(const LogRecord& record)
{
    std::string line = format("t=%.3fs", record.seconds);
    if (!record.phase.empty())
        line += " phase=" + quote(record.phase);
    line += " event=" + quote(record.event);
    for (const auto& entry : record.fields) {
        line += ' ';
        line += entry.first;
        line += '=';
        line += quote(entry.second);
    }
    return line;
}

bool parseLogLine(const std::string& line, LogRecord& out)
{
    out = LogRecord();
    size_t index = 0;
    bool sawTime = false;
    while (index < line.size()) {
        while (index < line.size() && line[index] == ' ')
            ++index;
        if (index >= line.size())
            break;
        const size_t keyStart = index;
        while (index < line.size() && line[index] != '=' && line[index] != ' ')
            ++index;
        const std::string key = line.substr(keyStart, index - keyStart);
        std::string value;
        if (index < line.size() && line[index] == '=') {
            ++index;
            if (index < line.size() && line[index] == '"') {
                ++index;
                while (index < line.size() && line[index] != '"') {
                    if (line[index] == '\\' && index + 1 < line.size())
                        ++index;
                    value += line[index];
                    ++index;
                }
                if (index < line.size())
                    ++index;  // closing quote
            } else {
                const size_t valueStart = index;
                while (index < line.size() && line[index] != ' ')
                    ++index;
                value = line.substr(valueStart, index - valueStart);
            }
        }
        if (key == "t") {
            out.seconds = std::strtod(value.c_str(), nullptr);
            sawTime = true;
        } else if (key == "phase") {
            out.phase = value;
        } else if (key == "event") {
            out.event = value;
        } else if (!key.empty()) {
            out.fields.emplace_back(key, value);
        }
    }
    if (!sawTime || out.event.empty())
        return false;
    return true;
}

std::string fieldOf(const LogRecord& record, const std::string& key)
{
    for (const auto& entry : record.fields) {
        if (entry.first == key)
            return entry.second;
    }
    return std::string();
}

std::vector<std::pair<std::string, std::string>> summaryFields(const Phase& phase,
                                                               Provider provider,
                                                               double sessionDuration)
{
    const ProviderPhaseStats& stats = phase.providers[static_cast<int>(provider)];
    const double duration = phase.open ? sessionDuration - phase.startSeconds
                                       : phase.endSeconds - phase.startSeconds;
    const double safeDuration = duration > 0.0 ? duration : 0.0;
    const double rate =
        safeDuration > 0.0 ? static_cast<double>(stats.arrivals) / safeDuration : 0.0;
    return {
        { "provider", providerName(provider) },
        { "phase", phase.name },
        { "duration", format("%.3fs", safeDuration) },
        { "arrivals", std::to_string(stats.arrivals) },
        { "transitions", std::to_string(stats.transitions) },
        { "systemButtons", std::to_string(stats.systemButtons) },
        { "presence", std::to_string(stats.presence) },
        { "rate", format("%.2f/s", rate) },
    };
}

std::vector<std::pair<std::string, std::string>> verdictFields(
    const std::string& baselinePhase, const std::string& exclusivePhase,
    const std::string& restoredPhase, Provider provider, const VerdictInputs& inputs,
    Verdict verdict, const std::string& noteOverride)
{
    std::string note;
    switch (verdict) {
    case Verdict::NotMeasurable:
        note = "baseline saw no background activity for this provider - the arrangement "
               "proves nothing here";
        break;
    case Verdict::Continuous:
        note = "activity kept arriving during the exclusive phase - not blocked for this "
               "provider";
        break;
    case Verdict::BlockedThenResumed:
        note = "arrivals stopped while the exclusive policy was in force and returned after "
               "release";
        break;
    case Verdict::BlockedNoResume:
        note = "arrivals stopped and did not return (device change or unplugged pad) - "
               "suspect, not proof";
        break;
    }
    return {
        { "provider", providerName(provider) },
        { "baselinePhase", baselinePhase },
        { "exclusivePhase", exclusivePhase },
        { "restoredPhase", restoredPhase },
        { "baselineRate", format("%.2f/s", inputs.baselineRate) },
        { "exclusiveRate", format("%.2f/s", inputs.exclusiveRate) },
        { "restoredRate", format("%.2f/s", inputs.restoredRate) },
        { "baselineArrivals", std::to_string(inputs.baselineArrivals) },
        { "presenceDuringExclusive", std::to_string(inputs.exclusivePresence) },
        { "result", verdictName(verdict) },
        { "note", noteOverride.empty() ? note : noteOverride },
    };
}

}  // namespace GameReceiver
