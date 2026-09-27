#include "games/GameDetector.h"
#include "games/SteamAppLookup.h"
#include "integration/ExternalGameContext.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>

#include <windows.h>
#include <tlhelp32.h>
#include <winver.h>

#include <atomic>
#include <unordered_map>

namespace
{
std::atomic<const integration::ExternalGameContext *> g_externalContext{nullptr};

bool isDescendantProcess(quint32 childPid, quint32 ancestorPid)
{
    if (childPid == 0 || ancestorPid == 0 || childPid == ancestorPid)
        return false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return false;
    std::unordered_map<DWORD, DWORD> parents;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            parents.emplace(entry.th32ProcessID, entry.th32ParentProcessID);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    DWORD current = childPid;
    for (int depth = 0; depth < 16; ++depth) {
        const auto it = parents.find(current);
        if (it == parents.end() || it->second == 0 || it->second == current)
            return false;
        if (it->second == ancestorPid)
            return true;
        current = it->second;
    }
    return false;
}
// Shell / system surfaces that are foreground-able but never a "game".
bool isShellProcess(const QString& exeLower)
{
    static const QStringList kShell = {
        QStringLiteral("explorer.exe"),
        QStringLiteral("searchhost.exe"),
        QStringLiteral("searchapp.exe"),
        QStringLiteral("shellexperiencehost.exe"),
        QStringLiteral("startmenuexperiencehost.exe"),
        QStringLiteral("applicationframehost.exe"),
        QStringLiteral("textinputhost.exe"),
        QStringLiteral("dwm.exe"),
        QStringLiteral("lockapp.exe"),
        QStringLiteral("gamehq.exe"),   // never screenshot ourselves / the overlay
        // The Snipping Tool's screen-clip layer covers the whole monitor, so the
        // fullscreen test below took it for a game and armed the replay buffer on
        // the desktop (observed 2026-07-17).
        QStringLiteral("snippingtool.exe"),
        QStringLiteral("screenclippinghost.exe"),
        QStringLiteral("screensketch.exe"),
    };
    return kShell.contains(exeLower);
}

// Overlay surfaces: fullscreen, but never a game — the class fix behind the
// per-process list above, which only ever catches the names we already know.
//
// A game's own render window cannot carry these. A flip-model D3D swapchain
// cannot present to a WS_EX_LAYERED window (layered windows go through DWM
// redirection), and no game's main window is click-through, a tool palette, or
// refuses activation. Overlays and HUDs are exactly what these mark.
bool isOverlayWindow(HWND hwnd)
{
    const LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    return (ex & WS_EX_LAYERED) || (ex & WS_EX_TRANSPARENT)
        || (ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_NOACTIVATE);
}

// Generic executable base names shared by many games (mostly Unreal Engine
// projects that ship as "Client-Win64-Shipping.exe" etc.). For these the real
// title is the project folder, not the exe, so we fall back to the path.
bool isGenericExeBase(const QString& baseLower)
{
    static const QStringList kGeneric = {
        QStringLiteral("ue4game"),  QStringLiteral("ue5game"),
        QStringLiteral("game"),     QStringLiteral("client"),
        QStringLiteral("server"),   QStringLiteral("launcher"),
        QStringLiteral("shipping"), QStringLiteral("gamelaunchhelper"),
    };
    return kGeneric.contains(baseLower);
}

// Strip Unreal/platform/config decorations: "-Win64-Shipping", "-WinGDK-Test",
// "-Shipping", trailing "-Win64" etc. A separator must precede the token so we
// never chop a real word glued to the title (e.g. a game named "…Final").
// Case-insensitive, applied until nothing more matches so stacked suffixes
// ("-Win64-Shipping") all come off.
QString stripBuildSuffixes(QString name)
{
    static const QRegularExpression re(
        QStringLiteral("[-_. ]+(Win64|Win32|WinGDK|x64|x86|"
                       "Shipping|Development|DebugGame|Debug|Test)$"),
        QRegularExpression::CaseInsensitiveOption);
    QString prev;
    do {
        prev = name;
        name.replace(re, QString());
    } while (name != prev && !name.isEmpty());
    return name;
}

// Insert spaces at camelCase / letter-digit boundaries so run-together titles
// read naturally: "Cyberpunk2077" -> "Cyberpunk 2077", "DOOMEternal" -> "DOOM
// Eternal". Acronyms like "BBQ" are left intact.
QString splitWords(QString name)
{
    static const QRegularExpression lowerUpper(QStringLiteral("([a-z0-9])([A-Z])"));
    static const QRegularExpression acronymWord(QStringLiteral("([A-Z]+)([A-Z][a-z])"));
    static const QRegularExpression letterDigit(QStringLiteral("([A-Za-z])([0-9])"));
    name.replace(acronymWord, QStringLiteral("\\1 \\2"));
    name.replace(lowerUpper, QStringLiteral("\\1 \\2"));
    name.replace(letterDigit, QStringLiteral("\\1 \\2"));
    return name;
}

// Full image path -> human-readable game title. `fullPath` may be empty (only
// the exe name known), in which case the generic-name folder fallback is skipped.
QString prettifyGameName(const QString& exe, const QString& fullPath = QString())
{
    QString base = exe;
    if (base.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
        base.chop(4);
    base = stripBuildSuffixes(base);

    // Generic UE exe ("Client-Win64-Shipping") -> use the project folder, which
    // is the directory just above "Binaries" (…/<Game>/Binaries/Win64/x.exe).
    if (!fullPath.isEmpty() && isGenericExeBase(base.toLower())) {
        const QStringList segs =
            QDir::fromNativeSeparators(fullPath).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        for (int i = segs.size() - 1; i > 0; --i) {
            if (segs.at(i).compare(QStringLiteral("Binaries"), Qt::CaseInsensitive) == 0) {
                base = segs.at(i - 1);
                break;
            }
        }
        // No Binaries folder? fall back to the exe's own parent directory.
        if (isGenericExeBase(base.toLower()) && segs.size() >= 2)
            base = segs.at(segs.size() - 2);
    }

    // Separators -> spaces, then split run-together words, then tidy whitespace.
    base.replace(QRegularExpression(QStringLiteral("[-_.]+")), QStringLiteral(" "));
    base = splitWords(base);
    base = base.simplified();

    return base.isEmpty() ? QStringLiteral("Unknown Game") : base;
}

// Raw window caption. Games usually set this to their marketing title, so it is
// often the ONLY place the real name appears when the exe is an engine codename
// (e.g. "BBQ-Win64-Shipping.exe" for "The First Berserker: Khazan").
QString readWindowTitle(HWND hwnd)
{
    wchar_t buf[512] = {};
    const int n = GetWindowTextW(hwnd, buf, 512);
    return (n > 0) ? QString::fromWCharArray(buf, n).simplified() : QString();
}

// Read one StringFileInfo field ("ProductName", "FileDescription") from the
// exe's version resource, using whatever language/codepage the file ships.
QString readVersionString(const QString& fullPath, const wchar_t* field)
{
    if (fullPath.isEmpty())
        return {};
    const std::wstring path = fullPath.toStdWString();

    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (size == 0)
        return {};

    QByteArray blob(static_cast<int>(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, blob.data()))
        return {};

    struct LangCp { WORD lang; WORD cp; };
    LangCp* xlate = nullptr;
    UINT xlateBytes = 0;
    if (!VerQueryValueW(blob.constData(), L"\\VarFileInfo\\Translation",
                        reinterpret_cast<void**>(&xlate), &xlateBytes)
        || xlateBytes < sizeof(LangCp)) {
        return {};
    }

    const UINT count = xlateBytes / sizeof(LangCp);
    for (UINT i = 0; i < count; ++i) {
        wchar_t sub[128];
        swprintf(sub, 128, L"\\StringFileInfo\\%04x%04x\\%ls",
                 xlate[i].lang, xlate[i].cp, field);
        wchar_t* val = nullptr;
        UINT valLen = 0;
        if (VerQueryValueW(blob.constData(), sub, reinterpret_cast<void**>(&val), &valLen)
            && valLen > 1) {
            return QString::fromWCharArray(val, valLen - 1).simplified();
        }
    }
    return {};
}

// Steam stores the real store name in each library's `appmanifest_<appid>.acf`
// — the true marketing title even when the exe is an engine codename
// ("BBQ"/"KZ" → the real title). SteamAppLookup owns the manifest scan.
QString steamTitleForPath(const QString& fullPath)
{
    return SteamAppLookup::forExecutable(fullPath).name;
}

// Is `cand` a usable human-facing title (vs. junk, a build artifact, or the bare
// exe/codename)? Used to decide whether a richer source beats the exe fallback.
bool isPlausibleTitle(const QString& cand, const QString& exeBase)
{
    if (cand.isEmpty() || cand.size() < 2 || cand.size() > 80)
        return false;
    if (!cand.contains(QRegularExpression(QStringLiteral("[A-Za-z]"))))
        return false;                                   // needs at least one letter
    // Reject leftover build decorations ("…-Win64-Shipping", "UE4Game", etc.).
    static const QRegularExpression junk(
        QStringLiteral("Win64|Win32|WinGDK|Shipping|Development|DebugGame"),
        QRegularExpression::CaseInsensitiveOption);
    if (cand.contains(junk))
        return false;
    // Reject the literal exe name / codename — that is what we are trying to beat.
    if (cand.compare(exeBase, Qt::CaseInsensitive) == 0)
        return false;
    return true;
}

// The title sources that cost disk I/O: a Steam library scan plus two version
// resource reads. All three describe the executable file, so they cannot change
// while a process lives — unlike the window caption, which games rewrite freely.
struct TitleSources
{
    QString fromExe;
    QString steamName;
    QString productName;
    QString fileDesc;
};

// Memoized so the 1.5 s autoTick does not re-read the disk on the GUI thread
// every tick. Keyed by pid AND path: a recycled pid or a swapped executable
// misses the cache and re-resolves rather than serving a stale title.
TitleSources titleSourcesFor(unsigned long pid, const QString& exe,
                             const QString& fullPath, const QString& winTitle)
{
    static QMutex mutex;
    static TitleSources cached;
    static unsigned long cachedPid = 0;
    static QString cachedPath;
    static bool primed = false;

    QMutexLocker lock(&mutex);
    if (primed && pid == cachedPid && fullPath == cachedPath)
        return cached;

    cached.fromExe     = prettifyGameName(exe, fullPath);
    cached.steamName   = steamTitleForPath(fullPath);
    cached.productName = readVersionString(fullPath, L"ProductName");
    cached.fileDesc    = readVersionString(fullPath, L"FileDescription");
    cachedPid  = pid;
    cachedPath = fullPath;
    primed     = true;

    qInfo().noquote() << "GameDetector title candidates for" << exe
                      << "| pid:" << pid
                      << "| steam:" << (cached.steamName.isEmpty() ? QStringLiteral("<none>") : cached.steamName)
                      << "| window:" << (winTitle.isEmpty() ? QStringLiteral("<none>") : winTitle)
                      << "| ProductName:" << (cached.productName.isEmpty() ? QStringLiteral("<none>") : cached.productName)
                      << "| FileDescription:" << (cached.fileDesc.isEmpty() ? QStringLiteral("<none>") : cached.fileDesc)
                      << "| fromExe:" << cached.fromExe
                      << "| path:" << (fullPath.isEmpty() ? QStringLiteral("<none>") : fullPath);
    return cached;
}

// Pick the best display title across all available sources, preferring
// human-facing metadata (window caption → ProductName → FileDescription) over
// the exe/codename fallback.
QString resolveTitle(unsigned long pid, const QString& exe,
                     const QString& fullPath, const QString& winTitle)
{
    QString exeBase = exe;
    if (exeBase.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive))
        exeBase.chop(4);

    const TitleSources src = titleSourcesFor(pid, exe, fullPath, winTitle);

    // Steam's manifest name is the authoritative store title — trust it first.
    if (isPlausibleTitle(src.steamName, exeBase))
        return src.steamName;

    // winTitle stays live: a game that sets its caption late must still win.
    const QString candidates[] = { winTitle, src.productName, src.fileDesc };
    for (const QString& c : candidates) {
        if (isPlausibleTitle(c, exeBase))
            return c;
    }
    return src.fromExe;
}
} // namespace

ForegroundGame GameDetector::current()
{
    ForegroundGame g;

    HWND hwnd = GetForegroundWindow();
    if (!hwnd)
        return g;
    g.hwnd = hwnd;
    g.valid = true;

    QString fullPath;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    g.pid = pid;
    if (pid) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (proc) {
            wchar_t buf[MAX_PATH] = {};
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(proc, 0, buf, &size)) {
                fullPath = QString::fromWCharArray(buf, size);
                g.executablePath = fullPath;
                g.processName = QFileInfo(fullPath).fileName();
            }
            CloseHandle(proc);
        }
    }
    g.windowTitle = readWindowTitle(hwnd);
    g.gameName = resolveTitle(g.pid, g.processName, fullPath, g.windowTitle);

    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    g.x = wr.left;
    g.y = wr.top;
    g.w = wr.right - wr.left;
    g.h = wr.bottom - wr.top;

    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(mon, &mi)) {
        const RECT& m = mi.rcMonitor;
        g.isFullscreen = wr.left <= m.left && wr.top <= m.top
                         && wr.right >= m.right && wr.bottom >= m.bottom;
    }

    g.isExcludedProcess = isShellProcess(g.processName.toLower());
    // "Covers the monitor" alone is not a game: it also matches every overlay,
    // which is how the replay buffer ended up recording the desktop.
    g.isGame = g.isFullscreen && !g.isExcludedProcess && !isOverlayWindow(hwnd);

    const integration::ExternalGameContext *context = g_externalContext.load();
    if (context && !g.isExcludedProcess && !isOverlayWindow(hwnd)) {
        const integration::ExternalGameMatch match = context->matchForeground(
            static_cast<quint32>(g.pid), g.executablePath, g.isGame,
            [](quint32 child, quint32 ancestor) {
                return isDescendantProcess(child, ancestor);
            });
        if (match.confidence != integration::MatchConfidence::None) {
            g.hasExternalIdentity = true;
            g.externalSource = match.session.sourceId;
            g.externalId = match.session.playniteGameId;
            if (!match.session.name.isEmpty())
                g.gameName = match.session.name;
            // Only an exact PID or a verified descendant permits a windowed
            // game. Directory hints may rename an already-safe fullscreen game
            // but can never turn an arbitrary foreground window into a game.
            if (match.authorizesWindowedCapture())
                g.isGame = true;
        }
    }
    return g;
}

void GameDetector::setExternalContext(const integration::ExternalGameContext *context)
{
    g_externalContext.store(context);
}

bool GameDetector::shouldCapture(const ForegroundGame& g, const QString& captureMode)
{
    if (!g.valid || g.w <= 0 || g.h <= 0)
        return false;
    if (captureMode == QStringLiteral("always"))
        return true;
    // only_in_games and (until a whitelist UI exists in 1.0) whitelist both
    // gate on the fullscreen-game heuristic.
    return g.isGame;
}
