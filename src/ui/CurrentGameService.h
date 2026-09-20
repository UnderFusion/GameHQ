#pragma once

#include <QString>

class CaptureDatabase;
struct GameEntry;

class CurrentGameService
{
public:
    explicit CurrentGameService(CaptureDatabase* db);

    int currentGameId() const { return m_currentGameId; }
    bool currentGameAvailable() const { return m_currentGameHasCaptures; }
    bool lastUpdateChangedGameMetadata() const { return m_lastUpdateChangedGameMetadata; }

    // cpo-p07: the mapping game context's identity. The canonical executable key
    // of the session's game (GameIdentity::executableKey - the same
    // normalization assignment rows are written with), or empty when no game is
    // in session or the row carries no executable path.
    //
    // Deliberately read from the SESSION, never from the raw foreground window:
    // the session already survives an open overlay (an excluded process is not a
    // game) and a game that is still running, so this key cannot be retargeted
    // just because the user opened the overlay.
    QString currentGameExecutableKey() const;
    // Display name of the session's game ("" when none): the Settings label.
    QString currentGameName() const;

    bool syncToForegroundGame();
    bool update(const QString& gameName, const QString& executablePath);

private:
    static constexpr int kClearAfterMisses = 3;

    int runningCapturedGameFallback() const;
    // The session's game row (id -1 when the session has no game or the row is
    // gone). One lookup, so the key and the name can never disagree about which
    // game is in session.
    GameEntry currentGameEntry() const;

    CaptureDatabase* m_db;
    int m_currentGameId = -1;
    bool m_currentGameHasCaptures = false;
    bool m_lastUpdateChangedGameMetadata = false;
    int m_foregroundMisses = 0;
};
