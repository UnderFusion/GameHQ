#pragma once

#include "updates/ReleaseInfo.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <optional>

class GitHubReleaseSource;
class UpdateDownloader;

// QML-facing update-check state machine. Owns a GitHubReleaseSource and
// turns its raw results into a state a settings page can bind to directly.
// Package download and SHA-256 verification are implemented here; installation
// is handed to the separate safe updater helper. A failed check never regresses a
// known-good result: it only ever falls back to UpToDate/UpdateAvailable.
class UpdateService : public QObject
{
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    // QML-friendly mirror of state(): "Idle", "Checking", "UpToDate",
    // "UpdateAvailable", "Downloading", "ReadyToInstall", "Installing", "Failed".
    Q_PROPERTY(QString stateName READ stateName NOTIFY stateChanged)
    Q_PROPERTY(QString installedVersion READ installedVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY releaseChanged)
    Q_PROPERTY(QString releaseName READ releaseName NOTIFY releaseChanged)
    // Raw GitHub body text is discovery metadata, not a verified localized
    // presentation source. Keep these compatibility properties empty so it
    // cannot bypass the compiled release-note bundle policy.
    Q_PROPERTY(QString notes READ notes NOTIFY releaseChanged)
    Q_PROPERTY(QVariantList noteBlocks READ noteBlocks NOTIFY releaseChanged)
    Q_PROPERTY(QString releaseUrl READ releaseUrl NOTIFY releaseChanged)
    Q_PROPERTY(qint64 size READ size NOTIFY releaseChanged)
    Q_PROPERTY(QDateTime publishedAt READ publishedAt NOTIFY releaseChanged)
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorChanged)
    Q_PROPERTY(QDateTime lastChecked READ lastChecked NOTIFY lastCheckedChanged)
    // True when the current Failed state came from a background/manual check
    // (no download was attempted yet) rather than from a download or install
    // preflight failure — lets QML offer "Check again" vs "Retry download".
    Q_PROPERTY(bool failedDuringCheck READ failedDuringCheck NOTIFY stateChanged)

public:
    enum class State
    {
        Idle,
        Checking,
        UpToDate,
        UpdateAvailable,
        Downloading,
        ReadyToInstall,
        PreparingForUpdate,
        Quiescent,
        Installing,
        Failed,
    };
    Q_ENUM(State)

    // owner/repo select the GitHub repo to poll; installedVersion should be
    // the running app's VERSION-file string (e.g. "0.6.7").
    // trustStatePath holds the anti-rollback release sequence and must live in
    // the user data root, not inside the replaceable program files.
    UpdateService(QString owner, QString repo, QString installedVersion, QString stagingRoot,
                  QString trustStatePath,
                  QObject *parent = nullptr);

    State state() const { return m_state; }
    QString stateName() const;
    QString installedVersion() const { return m_installedVersion; }
    QString latestVersion() const { return m_release ? m_release->version : QString(); }
    QString releaseName() const { return m_release ? m_release->name : QString(); }
    QString notes() const { return {}; }
    QVariantList noteBlocks() const;
    QString releaseUrl() const { return m_release ? m_release->webUrl : QString(); }
    qint64 size() const { return m_release ? m_release->zipSize : 0; }
    QDateTime publishedAt() const { return m_release ? m_release->publishedAt : QDateTime(); }
    int progress() const { return m_progress; }
    QString errorText() const { return m_errorText; }
    QDateTime lastChecked() const { return m_lastChecked; }
    bool failedDuringCheck() const { return m_failedDuringCheck; }

    // Config-key persistence (updates.skipped_version, internal.updates.etag,
    // ...) is owned by the caller; these let it prime/read this instance.
    void primeCachedEtag(const QString &etag) { m_etag = etag; }
    void primeSkippedVersion(const QString &version) { m_skippedVersion = version; }
    // Without this the persisted timestamp was never read back, so the "at most
    // one automatic check a day" gate started over on every launch.
    void primeLastChecked(const QDateTime &when) { m_lastChecked = when; }
    // A rate-limit cooldown must survive a restart, or quitting and reopening
    // GameHQ becomes a way to hammer an exhausted budget.
    void primeNextAllowedCheck(const QDateTime &when) { m_nextAllowedCheck = when; }
    QDateTime nextAllowedCheck() const { return m_nextAllowedCheck; }
    QString cachedEtag() const { return m_etag; }
    // The ETag actually worth sending: an ETag with no cached release behind it
    // only buys a 304 that resolves to "up to date".
    QString conditionalEtag() const;

    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void downloadUpdate();
    Q_INVOKABLE void cancelDownload();
    Q_INVOKABLE void installAndRestart();
    Q_INVOKABLE void skipVersion();
    Q_INVOKABLE void openReleasePage() const;
    void markQuiescent();
    void markInstalling();
    void cancelPreparation(const QString &reason);

Q_SIGNALS:
    void stateChanged();
    void releaseChanged();
    void progressChanged();
    void errorChanged();
    void lastCheckedChanged();
    // Caller should persist this to updates.skipped_version.
    void versionSkipped(const QString &version);
    // Caller should persist this to internal.updates.etag.
    void etagUpdated(const QString &etag);
    void nextAllowedCheckChanged(const QDateTime &when);
    void prepareForUpdateRequested();
    void quiescenceReached();
    void installApproved(const VerifiedUpdate &verified);

private:
    void setState(State state);
    void setNextAllowedCheck(const QDateTime &when);
    QString cooldownMessage() const;
    bool releaseIsNewerThanInstalled(const ReleaseInfo &release) const;
    State fallbackAfterCheck() const;
    void applyRelease(const ReleaseInfo &release);
    void onSucceeded(const ReleaseInfo &release, const QString &etag);
    void onUnchanged(const QString &etag);
    void onNotFound();
    void onRateLimited(qint64 resetEpochSeconds);
    void onFailed(const QString &errorText);

    GitHubReleaseSource *m_source;
    UpdateDownloader *m_downloader;
    QString m_installedVersion;
    QString m_skippedVersion;
    State m_state = State::Idle;
    std::optional<ReleaseInfo> m_release;
    int m_progress = 0;
    QString m_errorText;
    QDateTime m_lastChecked;
    QString m_etag;
    VerifiedUpdate m_verified;
    QString m_packageRoot;
    QElapsedTimer m_lastCheckRequest;
    bool m_revalidatingInstall = false;
    bool m_failedDuringCheck = false;
    // Guards the one unconditional re-request after a 304 arrived with no
    // cached release, so a server that keeps answering 304 cannot loop.
    bool m_retriedWithoutCache = false;
    QDateTime m_nextAllowedCheck;   // UTC; invalid = no cooldown
};
