// cpo-o06d: GameHQ's external controller-input receiver (developer tool).
//
// Why this exists: GameHQ's own diagnostics can only ever report what GameHQ
// asked the GameInput runtime for. Whether another process still receives the
// pad while the overlay holds the verified foreground and the exclusive policy
// is in force is a question about a SECOND process, so it needs a second
// process to answer it. This tool is that process: it watches the controller
// from outside through GameInput, XInput and Raw Input, records phase by phase
// what actually arrived, and refuses to call an unmeasurable run an isolated one.
//
// It never ships: the target is built only with the project's test build
// (GAMEHQ_BUILD_TESTS), it lives outside every packaging payload, and it only
// observes — no injection, no hooks, no drivers, no virtual devices.
//
// Usage:
//   GameHQInputReceiver.exe --log <file> [--duration <seconds>]
//                           [--providers gameinput,xinput,rawinput]
//                           [--phase-file <file>] [--label <text>]
//                           [--heartbeat-ms <n>] [--verdict-phases a,b,c]
//                           [--stop-on-stdin] [--self-check]
//
// The phase file is how a harness names the phases without the receiver having
// to know anything about them: every new non-empty line is a phase marker, read
// while the run is in progress. tests/tst_overlaynative.cpp drives it that way.
#include "ReceiverModel.h"
#include "ReceiverProviders.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{

using GameReceiver::LogRecord;
using GameReceiver::Provider;
using GameReceiver::ReceiverProvider;
using GameReceiver::ReceiverRun;
using GameReceiver::Session;
using GameReceiver::Verdict;
using GameReceiver::VerdictInputs;

constexpr const char* kToolName = "GameHQInputReceiver";
constexpr const char* kToolVersion = "1";

std::string utf8(const std::wstring& text)
{
    if (text.empty())
        return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0)
        return std::string();
    std::string result(size_t(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), int(text.size()), result.data(), needed, nullptr,
                        nullptr);
    return result;
}

std::string executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring wide(path);
    const size_t slash = wide.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        wide = wide.substr(0, slash);
    return utf8(wide);
}

struct Options
{
    std::string logPath;
    double durationSeconds = 0.0;
    std::vector<Provider> providers;
    std::string phaseFile;
    std::string label;
    std::vector<GameReceiver::RawUsage> rawUsages;
    int heartbeatMs = 1000;
    std::string baselinePhase = "baseline";
    std::string exclusivePhase = "exclusive";
    std::string restoredPhase = "restored";
    bool stopOnStdin = false;
    bool selfCheck = false;
};

void printUsage()
{
    std::cout << kToolName
              << " - watch the controller from outside GameHQ (developer tool)\n"
                 "  --log <file>            where to write the receipt (required)\n"
                 "  --duration <seconds>    bounded run; 0 (default) runs until Ctrl+C\n"
                 "  --providers <csv>       gameinput,xinput,rawinput (default: all three)\n"
                 "  --phase-file <file>     each new non-empty line names the next phase\n"
                 "  --label <text>          free-form label stored in the log header\n"
                 "  --raw-usages p:u,...    Raw Input top-level collections (default "
                 "01:04,01:05,01:08)\n"
                 "  --heartbeat-ms <n>      activity heartbeat cadence (default 1000)\n"
                 "  --verdict-phases a,b,c  phases the verdict compares (baseline,exclusive,"
                 "restored)\n"
                 "  --stop-on-stdin         stop when a line arrives on stdin\n"
                 "  --self-check            initialise the providers, report, exit\n";
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error)
{
    options.providers = { Provider::GameInput, Provider::XInput, Provider::RawInput };
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        auto value = [&](const char* name, std::string& out) {
            if (index + 1 >= argc) {
                error = std::string("missing value for ") + name;
                return false;
            }
            out = argv[++index];
            return true;
        };
        std::string text;
        if (argument == "--log") {
            if (!value("--log", options.logPath))
                return false;
        } else if (argument == "--duration") {
            if (!value("--duration", text))
                return false;
            options.durationSeconds = std::atof(text.c_str());
        } else if (argument == "--providers") {
            if (!value("--providers", text))
                return false;
            options.providers.clear();
            std::stringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ',')) {
                Provider provider;
                if (!GameReceiver::providerFromName(item, provider)) {
                    error = "unknown provider '" + item + "'";
                    return false;
                }
                options.providers.push_back(provider);
            }
            if (options.providers.empty()) {
                error = "--providers needs at least one name";
                return false;
            }
        } else if (argument == "--phase-file") {
            if (!value("--phase-file", options.phaseFile))
                return false;
        } else if (argument == "--label") {
            if (!value("--label", options.label))
                return false;
        } else if (argument == "--raw-usages") {
            if (!value("--raw-usages", text))
                return false;
            options.rawUsages.clear();
            std::stringstream stream(text);
            std::string item;
            while (std::getline(stream, item, ',')) {
                const size_t colon = item.find(':');
                if (colon == std::string::npos) {
                    error = "raw usage '" + item + "' is not page:usage";
                    return false;
                }
                GameReceiver::RawUsage usage;
                usage.page = unsigned(std::strtoul(item.substr(0, colon).c_str(), nullptr, 16));
                usage.usage = unsigned(std::strtoul(item.substr(colon + 1).c_str(), nullptr, 16));
                options.rawUsages.push_back(usage);
            }
            if (options.rawUsages.empty()) {
                error = "--raw-usages needs at least one page:usage pair";
                return false;
            }
        } else if (argument == "--heartbeat-ms") {
            if (!value("--heartbeat-ms", text))
                return false;
            options.heartbeatMs = std::max(100, std::atoi(text.c_str()));
        } else if (argument == "--verdict-phases") {
            if (!value("--verdict-phases", text))
                return false;
            std::stringstream stream(text);
            std::vector<std::string> names;
            std::string item;
            while (std::getline(stream, item, ','))
                names.push_back(item);
            if (names.size() != 3) {
                error = "--verdict-phases needs exactly three names";
                return false;
            }
            options.baselinePhase = names[0];
            options.exclusivePhase = names[1];
            options.restoredPhase = names[2];
        } else if (argument == "--stop-on-stdin") {
            options.stopOnStdin = true;
        } else if (argument == "--self-check") {
            options.selfCheck = true;
        } else if (argument == "--help" || argument == "-h") {
            printUsage();
            std::exit(0);
        } else {
            error = "unknown argument '" + argument + "'";
            return false;
        }
    }
    if (options.logPath.empty()) {
        error = "--log is required";
        return false;
    }
    return true;
}

// The run: one monotonic clock, one thread-safe log file and one serialized
// session, so an event's phase is decided by its timestamp and never by which
// provider thread happened to wake first.
class ReceiverApp final : public ReceiverRun
{
public:
    explicit ReceiverApp(const Options& options) : m_options(options) {}

    ~ReceiverApp() override { closeLog(); }

    bool openLog(std::string& error)
    {
        m_log = std::fopen(m_options.logPath.c_str(), "wb");
        if (!m_log) {
            error = "cannot open the log file '" + m_options.logPath + "'";
            return false;
        }
        return true;
    }

    void closeLog()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_log) {
            std::fflush(m_log);
            std::fclose(m_log);
            m_log = nullptr;
        }
    }

    double nowSeconds() const override
    {
        const auto now = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(now - m_start).count();
    }

    void log(const std::string& event,
             const std::vector<std::pair<std::string, std::string>>& fields) override
    {
        LogRecord record;
        record.seconds = nowSeconds();
        record.event = event;
        record.fields = fields;
        writeLine(GameReceiver::formatLogLine(record));
    }

    void writeLine(const std::string& line)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_log)
            return;
        std::fwrite(line.data(), 1, line.size(), m_log);
        std::fputc('\n', m_log);
        // Flushed per line on purpose: a harness waits for `event=ready` and a
        // crash must still leave the receipt readable up to the last line.
        std::fflush(m_log);
    }

    void noteArrival(Provider provider, double seconds) override
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_session.noteArrival(provider, seconds);
    }

    void noteTransition(Provider provider, double seconds) override
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_session.noteTransition(provider, seconds);
    }

    void noteSystemButton(Provider provider, double seconds) override
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_session.noteSystemButton(provider, seconds);
    }

    void notePresence(Provider provider, double seconds) override
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_session.notePresence(provider, seconds);
    }

    void markPhase(const std::string& name, double seconds)
    {
        {
            std::lock_guard<std::mutex> lock(m_sessionMutex);
            m_session.markPhase(name, seconds);
        }
        LogRecord record;
        record.seconds = seconds;
        record.event = "phase";
        record.fields = { { "name", name } };
        writeLine(GameReceiver::formatLogLine(record));
    }

    void closeSession(double seconds)
    {
        std::lock_guard<std::mutex> lock(m_sessionMutex);
        m_session.closeAt(seconds);
    }

    std::mutex& sessionMutex() { return m_sessionMutex; }

    const Session& session() const { return m_session; }

    const std::vector<GameReceiver::RawUsage>& rawUsages() const { return m_options.rawUsages; }

    std::uint64_t processedPhaseLines() const
    {
        return m_processedPhaseLines.load(std::memory_order_acquire);
    }

    void setProcessedPhaseLines(std::uint64_t count)
    {
        m_processedPhaseLines.store(count, std::memory_order_release);
    }

    bool headerWritten() const { return m_headerWritten; }
    void setHeaderWritten() { m_headerWritten = true; }

private:
    Options m_options;
    std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
    std::FILE* m_log = nullptr;
    std::mutex m_mutex;
    std::mutex m_sessionMutex;
    Session m_session;
    std::atomic<std::uint64_t> m_processedPhaseLines{ 0 };
    bool m_headerWritten = false;
};

std::vector<std::pair<std::string, std::string>> providerFields(Provider provider)
{
    return { { "provider", GameReceiver::providerName(provider) } };
}

// Reads any new phase markers. Returns the names applied, in order.
std::vector<std::string> pollPhaseFile(const Options& options, ReceiverApp& app,
                                       std::uint64_t& processedLines)
{
    std::vector<std::string> applied;
    if (options.phaseFile.empty())
        return applied;
    std::ifstream file(options.phaseFile);
    if (!file)
        return applied;
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' '))
            line.pop_back();
        lines.push_back(line);
    }
    if (lines.size() <= processedLines)
        return applied;
    for (std::uint64_t index = processedLines; index < lines.size(); ++index) {
        const std::string& marker = lines[size_t(index)];
        if (marker.empty() || marker[0] == '#')
            continue;
        app.markPhase(marker, app.nowSeconds());
        applied.push_back(marker);
    }
    processedLines = lines.size();
    app.setProcessedPhaseLines(processedLines);
    return applied;
}

std::unique_ptr<ReceiverProvider> makeProvider(Provider provider, ReceiverRun& run)
{
    switch (provider) {
    case Provider::GameInput:
        return makeGameInputProvider(run);
    case Provider::XInput:
        return makeXInputProvider(run);
    case Provider::RawInput:
        // The receiver's own default (game controls) when the caller does not
        // ask for a specific collection.
        return makeRawInputProvider(run, std::vector<GameReceiver::RawUsage>());
    }
    return nullptr;
}

int runSelfCheck(ReceiverApp& app, const Options& options)
{
    for (Provider provider : options.providers) {
        std::unique_ptr<ReceiverProvider> instance =
            provider == Provider::RawInput && !app.rawUsages().empty()
            ? makeRawInputProvider(app, app.rawUsages())
            : makeProvider(provider, app);
        std::string error;
        const bool started = instance->start(error);
        LogRecord record;
        record.seconds = app.nowSeconds();
        record.event = "provider";
        record.fields = providerFields(provider);
        record.fields.emplace_back("state", started ? "ready" : "unavailable");
        if (started)
            record.fields.emplace_back("detail", instance->statusDetail());
        else
            record.fields.emplace_back("reason", error);
        app.writeLine(GameReceiver::formatLogLine(record));
    }
    app.writeLine(GameReceiver::formatLogLine(
        LogRecord{ app.nowSeconds(), "", "exit", { { "reason", "self-check" } } }));
    return 0;
}

std::atomic_bool g_stop{ false };

BOOL WINAPI consoleHandler(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_stop.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

std::vector<std::pair<std::string, std::string>> foregroundLogFields()
{
    const GameReceiver::ForegroundInfo info = GameReceiver::foregroundSnapshot();
    const bool self = info.processId == GetCurrentProcessId();
    return GameReceiver::foregroundFields(info, self);
}

}  // namespace

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error)) {
        std::cerr << "error: " << error << "\n";
        printUsage();
        return 2;
    }

    ReceiverApp app(options);
    if (!app.openLog(error)) {
        std::cerr << "error: " << error << "\n";
        return 2;
    }

    if (!options.selfCheck) {
        const auto header = [&] {
            std::vector<std::pair<std::string, std::string>> fields = {
                { "tool", kToolName },
                { "version", kToolVersion },
                { "pid", std::to_string(GetCurrentProcessId()) },
                { "label", options.label },
                { "exe_dir", executableDirectory() },
                { "duration", std::to_string(options.durationSeconds) },
                { "note",
                  "developer-only external observer; no injection, no hooks, no virtual "
                  "devices" },
            };
            return fields;
        }();
        LogRecord start;
        start.seconds = app.nowSeconds();
        start.event = "start";
        start.fields = header;
        app.writeLine(GameReceiver::formatLogLine(start));
    } else {
        LogRecord start;
        start.seconds = app.nowSeconds();
        start.event = "start";
        start.fields = { { "tool", kToolName },
                         { "version", kToolVersion },
                         { "mode", "self-check" } };
        app.writeLine(GameReceiver::formatLogLine(start));
    }

    if (options.selfCheck) {
        const int result = runSelfCheck(app, options);
        app.closeLog();
        return result;
    }

    SetConsoleCtrlHandler(consoleHandler, TRUE);

    std::vector<std::unique_ptr<ReceiverProvider>> providers;
    for (Provider provider : options.providers) {
        std::unique_ptr<ReceiverProvider> instance =
            provider == Provider::RawInput && !app.rawUsages().empty()
            ? makeRawInputProvider(app, app.rawUsages())
            : makeProvider(provider, app);
        std::string startError;
        const bool started = instance->start(startError);
        LogRecord record;
        record.seconds = app.nowSeconds();
        record.event = "provider";
        record.fields = providerFields(provider);
        record.fields.emplace_back("state", started ? "ready" : "unavailable");
        if (started) {
            record.fields.emplace_back("detail", instance->statusDetail());
        } else {
            record.fields.emplace_back("reason", startError);
        }
        const auto foreground = foregroundLogFields();
        record.fields.insert(record.fields.end(), foreground.begin(), foreground.end());
        app.writeLine(GameReceiver::formatLogLine(record));
        if (started)
            providers.push_back(std::move(instance));
        else
            instance->stop();
    }

    {
        LogRecord ready;
        ready.seconds = app.nowSeconds();
        ready.event = "ready";
        ready.fields = { { "providers", std::to_string(providers.size()) } };
        const auto readyForeground = foregroundLogFields();
        ready.fields.insert(ready.fields.end(), readyForeground.begin(), readyForeground.end());
        app.writeLine(GameReceiver::formatLogLine(ready));
    }

    std::thread stdinWatcher;
    if (options.stopOnStdin) {
        stdinWatcher = std::thread([] {
            std::string line;
            while (std::getline(std::cin, line))
                g_stop.store(true, std::memory_order_release);
            g_stop.store(true, std::memory_order_release);
        });
    }

    std::uint64_t processedPhaseLines = 0;
    auto heartbeatAt = std::chrono::steady_clock::now();
    std::string stopReason = "duration";

    for (;;) {
        if (g_stop.load(std::memory_order_acquire)) {
            stopReason = options.stopOnStdin ? "stdin" : "signal";
            break;
        }
        if (options.durationSeconds > 0.0 && app.nowSeconds() >= options.durationSeconds) {
            stopReason = "duration";
            break;
        }

        pollPhaseFile(options, app, processedPhaseLines);

        const auto now = std::chrono::steady_clock::now();
        if (now - heartbeatAt >= std::chrono::milliseconds(options.heartbeatMs)) {
            const double interval =
                std::chrono::duration<double>(now - heartbeatAt).count();
            heartbeatAt = now;
            for (const auto& provider : providers) {
                const GameReceiver::ProviderCounters counters = provider->takeCounters();
                LogRecord record;
                record.seconds = app.nowSeconds();
                record.event = "activity";
                record.fields = { { "provider", provider->name() } };
                record.fields.emplace_back("interval", std::to_string(interval));
                record.fields.emplace_back("arrivals", std::to_string(counters.arrivals));
                record.fields.emplace_back("transitions", std::to_string(counters.transitions));
                record.fields.emplace_back("systemButtons", std::to_string(counters.systemButtons));
                record.fields.emplace_back("presence", std::to_string(counters.presence));
                const auto foreground = foregroundLogFields();
                record.fields.insert(record.fields.end(), foreground.begin(), foreground.end());
                app.writeLine(GameReceiver::formatLogLine(record));
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    g_stop.store(false, std::memory_order_release);
    for (const auto& provider : providers)
        provider->stop();
    if (stdinWatcher.joinable()) {
        // The watcher only ends on EOF; the process is about to exit anyway.
        stdinWatcher.detach();
    }

    app.closeSession(app.nowSeconds());

    // The receipt's machine-readable tail: one line per phase and provider, then
    // the verdict per provider. The verdict names its inputs so a reader can see
    // what was compared instead of trusting a summary word.
    {
        std::lock_guard<std::mutex> lock(app.sessionMutex());
        const Session& session = app.session();
        for (const GameReceiver::Phase& phase : session.phases()) {
            for (int index = 0; index < GameReceiver::kProviderCount; ++index) {
                const Provider provider = static_cast<Provider>(index);
                if (std::find(options.providers.begin(), options.providers.end(), provider)
                    == options.providers.end())
                    continue;
                LogRecord record;
                record.seconds = app.nowSeconds();
                record.event = "summary";
                record.fields = GameReceiver::summaryFields(phase, provider, session.endSeconds());
                app.writeLine(GameReceiver::formatLogLine(record));
            }
        }

        for (Provider provider : options.providers) {
            VerdictInputs inputs;
            const GameReceiver::Phase* baseline = session.phaseByName(options.baselinePhase);
            const GameReceiver::Phase* exclusive = session.phaseByName(options.exclusivePhase);
            const GameReceiver::Phase* restored = session.phaseByName(options.restoredPhase);
            std::string missing;
            if (!baseline)
                missing = options.baselinePhase;
            else if (!exclusive)
                missing = options.exclusivePhase;
            else if (!restored)
                missing = options.restoredPhase;

            LogRecord record;
            record.seconds = app.nowSeconds();
            record.event = "verdict";
            if (!missing.empty()) {
                record.fields = GameReceiver::verdictFields(
                    options.baselinePhase, options.exclusivePhase, options.restoredPhase, provider,
                    inputs, Verdict::NotMeasurable,
                    "phase '" + missing + "' is not present in this run - nothing was compared");
            } else {
                const int providerIndex = static_cast<int>(provider);
                inputs.baselineArrivals = baseline->providers[providerIndex].arrivals;
                inputs.baselineRate = session.arrivalRate(*baseline, provider);
                inputs.exclusiveRate = session.arrivalRate(*exclusive, provider);
                inputs.restoredRate = session.arrivalRate(*restored, provider);
                inputs.exclusivePresence = exclusive->providers[providerIndex].presence;
                record.fields = GameReceiver::verdictFields(
                    options.baselinePhase, options.exclusivePhase, options.restoredPhase, provider,
                    inputs, GameReceiver::judge(inputs));
            }
            app.writeLine(GameReceiver::formatLogLine(record));
        }
    }

    {
        LogRecord exit;
        exit.seconds = app.nowSeconds();
        exit.event = "exit";
        exit.fields = { { "reason", stopReason } };
        app.writeLine(GameReceiver::formatLogLine(exit));
    }
    app.closeLog();
    return 0;
}
