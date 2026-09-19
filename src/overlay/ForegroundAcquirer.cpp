#include "overlay/ForegroundAcquirer.h"

#include "input/InputDiagnostics.h"
#include "overlay/ForegroundApi.h"

#include <QDebug>
#include <QTimer>

constexpr int ForegroundAcquirer::kRetryDelaysMs[2];

ForegroundAcquirer::ForegroundAcquirer(QObject* parent)
    : ForegroundAcquirer(ForegroundApi::createSystem(), parent)
{
}

ForegroundAcquirer::ForegroundAcquirer(ForegroundApi* api, QObject* parent)
    : QObject(parent)
    , m_api(api)
{
}

ForegroundAcquirer::~ForegroundAcquirer() = default;

void ForegroundAcquirer::acquire(void* target, const QString& phase)
{
    ++m_generation;   // orphan any retry still scheduled for the previous call
    m_target = target;
    m_phase = phase;
    m_attempts = 0;
    attempt();
}

void ForegroundAcquirer::cancel()
{
    ++m_generation;
}

void ForegroundAcquirer::attempt()
{
    const int attemptGeneration = m_generation;
    ++m_attempts;
    const bool reported = m_api->forceForeground(m_target);
    // Native focus calls can dispatch callbacks that cancel this acquisition.
    if (attemptGeneration != m_generation)
        return;
    // What SetForegroundWindow *reported* is not the truth — re-read where the
    // foreground actually is. Windows can claim success without moving it.
    const bool acquired = m_api->foregroundWindow() == m_target;
    if (!acquired && m_attempts < kMaxAttempts) {
        const int generation = m_generation;
        QTimer::singleShot(kRetryDelaysMs[m_attempts - 1], this, [this, generation] {
            if (generation == m_generation)
                attempt();
        });
        return;
    }

    m_lastAcquired = acquired;
    qInfo().noquote() << QStringLiteral("Overlay: %1 foreground %2 after %3 attempt(s)"
                                        " (SetForegroundWindow said %4)")
                             .arg(m_phase,
                                  acquired ? QStringLiteral("acquired")
                                           : QStringLiteral("NOT acquired"),
                                  QString::number(m_attempts),
                                  reported ? QStringLiteral("ok") : QStringLiteral("denied"));
    InputDiagnostics::instance().noteForeground(m_phase, acquired);
    emit finished(m_phase, m_target, acquired, m_attempts);
}
