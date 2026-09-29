#pragma once

#include <QLibrary>
#include <QString>

#include <memory>

namespace telegram
{

// The entry points of TDLib's JSON interface, as documented in td_json_client.h.
struct TdJsonApi
{
    using CreateClientId = int (*)();
    using Send = void (*)(int clientId, const char* request);
    using Receive = const char* (*)(double timeoutSeconds);
    using Execute = const char* (*)(const char* request);
    using SetLogCallback = void (*)(int maxVerbosity, void (*callback)(int, const char*));

    CreateClientId createClientId = nullptr;
    Send send = nullptr;
    Receive receive = nullptr;
    Execute execute = nullptr;
    SetLogCallback setLogMessageCallback = nullptr;   // optional
    bool isValid() const { return createClientId && send && receive && execute; }
};

// Optional, lazily loaded TDLib runtime. Nothing is loaded until load() is
// called, and only Telegram Integrated ever calls it: Telegram Desktop sharing
// never touches this class. The DLL must sit at the pinned relative location,
// hash to the pinned SHA-256 and report the pinned TDLib version; anything else
// is refused with a stable status so the UI can explain it.
class TdRuntime
{
public:
    enum class Status {
        NotLoaded,      // load() not called yet
        Ready,
        Unpinned,       // no expected hash is recorded: refuse rather than trust
        NotInstalled,   // the optional runtime is not present
        HashMismatch,   // file differs from the pinned build
        LoadFailed,     // present but the OS could not load it
        Incompatible,   // loaded but missing entry points or wrong version
    };

    // An empty `expectedSha256` means Unpinned.
    TdRuntime(QString dllPath, QString expectedSha256, QString expectedVersion);
    ~TdRuntime();
    TdRuntime(const TdRuntime&) = delete;
    TdRuntime& operator=(const TdRuntime&) = delete;

    // The production locator: <applicationDir>/runtime/tdlib/tdjson.dll with
    // the compiled-in pin.
    static std::unique_ptr<TdRuntime> forInstalledApp(const QString& applicationDir);

    // Idempotent: a failure is remembered, not retried silently.
    Status load();
    Status status() const { return m_status; }
    bool isReady() const { return m_status == Status::Ready; }
    const TdJsonApi& api() const { return m_api; }
    // Version the library reported (only after a successful load).
    QString reportedVersion() const { return m_reportedVersion; }
    QString dllPath() const { return m_dllPath; }

    static QString statusCode(Status status);   // stable, log/UI-safe
    static QString sha256OfFile(const QString& path);

private:
    QString m_dllPath;
    QString m_expectedSha256;
    QString m_expectedVersion;
    Status m_status = Status::NotLoaded;
    TdJsonApi m_api;
    QString m_reportedVersion;
    std::unique_ptr<QLibrary> m_library;
};

} // namespace telegram
