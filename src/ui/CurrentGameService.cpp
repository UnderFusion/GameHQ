#include "ui/CurrentGameService.h"
#include "core/GameIdentity.h"
#include "games/GameDetector.h"
#include "storage/CaptureDatabase.h"

#include <QFileInfo>
#include <QSet>

#include <windows.h>
#include <tlhelp32.h>

namespace
{
QSet<QString> runningExecutablePaths()
{
    QSet<QString> paths;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return paths;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!proc)
            continue;

        wchar_t buf[MAX_PATH] = {};
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, buf, &size))
            paths.insert(GameIdentity::executableKey(QString::fromWCharArray(buf, size)));
        CloseHandle(proc);
    }

    CloseHandle(snapshot);
    return paths;
}
} // namespace

CurrentGameService::CurrentGameService(CaptureDatabase* db)
    : m_db(db)
{
}

bool CurrentGameService::syncToForegroundGame()
{
    const ForegroundGame foreground = GameDetector::current();
    return update(foreground.valid && !foreground.isExcludedProcess ? foreground.gameName : QString(),
                  foreground.valid && !foreground.isExcludedProcess ? foreground.executablePath : QString());
}

bool CurrentGameService::update(const QString& gameName, const QString& executablePath)
{
    m_lastUpdateChangedGameMetadata = false;
    int gameId = -1;
    bool hasCaptures = false;
    bool matchedCapturedGame = false;
    const QString key = GameIdentity::key(gameName);

    if (!gameName.isEmpty() && !key.isEmpty()) {
        m_foregroundMisses = 0;
        const auto entries = m_db->listGames();
        for (const GameEntry& game : entries) {
            if (GameIdentity::key(game.name) == key) {
                gameId = game.id;
                matchedCapturedGame = true;
                break;
            }
        }
        if (gameId < 0)
            gameId = runningCapturedGameFallback();
    } else if (m_currentGameId >= 0 && ++m_foregroundMisses < kClearAfterMisses) {
        gameId = m_currentGameId;
    } else {
        gameId = runningCapturedGameFallback();
        if (gameId >= 0)
            m_foregroundMisses = 0;
    }

    if (gameId >= 0)
        hasCaptures = m_db->hasCapturesForGame(gameId);

    const bool stateChanged = m_currentGameId != gameId
        || m_currentGameHasCaptures != hasCaptures;
    if (stateChanged) {
        m_currentGameId = gameId;
        m_currentGameHasCaptures = hasCaptures;
    }

    if (!gameName.isEmpty() && (matchedCapturedGame || gameId < 0))
        m_lastUpdateChangedGameMetadata = m_db->rememberGameExecutable(gameName, executablePath);

    return stateChanged;
}

int CurrentGameService::runningCapturedGameFallback() const
{
    const QSet<QString> running = runningExecutablePaths();
    if (running.isEmpty())
        return -1;

    int fallbackId = -1;
    const auto entries = m_db->listGames();
    for (const GameEntry& game : entries) {
        if (game.executablePath.isEmpty())
            continue;

        const QString path = GameIdentity::executableKey(game.executablePath);
        if (path.isEmpty() || !running.contains(path))
            continue;

        if (game.id == m_currentGameId)
            return game.id;
        if (fallbackId < 0)
            fallbackId = game.id;
    }
    return fallbackId;
}
