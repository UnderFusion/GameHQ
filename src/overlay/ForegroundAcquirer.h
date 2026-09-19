#pragma once

#include <QObject>
#include <QString>

#include <memory>

class ForegroundApi;

// Moves the OS foreground to a target window and tells the truth about
// whether it worked. One synchronous attempt, then at most two timer-based
// retries (50 ms, 150 ms) — SetForegroundWindow denials are often transient
// (the shell finishing its own transition), but a persistent retry loop
// would fight the shell, so the budget is fixed and small. There is no
// busy-wait; between attempts the GUI thread runs normally.
class ForegroundAcquirer : public QObject
{
    Q_OBJECT
public:
    explicit ForegroundAcquirer(QObject* parent = nullptr);
    // Test seam: takes ownership of `api`.
    explicit ForegroundAcquirer(ForegroundApi* api, QObject* parent = nullptr);
    ~ForegroundAcquirer() override;

    // Begin acquiring for `phase` ("overlay show" / "overlay hide"). A new
    // call supersedes any retries still pending from the previous one.
    void acquire(void* target, const QString& phase);

    // Stop queued retries when the window closes or focus moves elsewhere.
    void cancel();

    // Result of the most recently *finished* acquisition.
    bool lastAcquired() const { return m_lastAcquired; }

    static constexpr int kMaxAttempts = 3;   // 1 attempt + 2 retries
    static constexpr int kRetryDelaysMs[2] = {50, 150};

signals:
    // attempts = how many were actually made (1..3).
    void finished(const QString& phase, void* target, bool acquired, int attempts);

private:
    void attempt();

    std::unique_ptr<ForegroundApi> m_api;
    QString m_phase;
    void* m_target = nullptr;
    int m_attempts = 0;
    int m_generation = 0;    // cancels superseded pending retries
    bool m_lastAcquired = false;
};
