#include "app/App.h"
#include "capture/FramePumpService.h"
#include "capture/ScreenshotService.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/Paths.h"
#include "core/UpdateMaintenance.h"
#include "integration/IntegrationService.h"
#include "localization/LanguageManager.h"
#include "localization/NativeText.h"
#include "updates/UpdateSchedule.h"
#include "updates/UpdateService.h"
#include "updates/UpdateInstaller.h"

#include <QDateTime>
#include <QGuiApplication>
#include <QTimer>

// Part of App::init(): the update service, its persisted check state and
// the capture quiescence handshake before an install.
void App::wireUpdates()
{
    m_updates = std::make_unique<UpdateService>(QStringLiteral("underfusion"), QStringLiteral("GameHQ"),
                                                 QStringLiteral(GAMEHQ_PRERELEASE).isEmpty()
                                                     ? QStringLiteral(GAMEHQ_VERSION)
                                                     : QStringLiteral(GAMEHQ_VERSION "-" GAMEHQ_PRERELEASE),
                                                 Paths::packageRoot() + QStringLiteral("/.update/downloads"),
                                                 Paths::dataDir() + QStringLiteral("/release-trust-state.json"),
                                                 nullptr);
    m_updates->setNotesLocale(m_languageManager->effectiveLanguage());
    connect(m_languageManager.get(), &LanguageManager::languageChanged, this, [this] {
        m_updates->setNotesLocale(m_languageManager->effectiveLanguage());
    });
    // Update-check policy (docs/updater.md "Discovery"): prime the cached ETag
    // and skip flag from config, persist whatever the service later reports,
    // and check on every launch, then hourly. Startup and manual checkNow()
    // bypass the recent-check interval, but always respect GitHub cooldowns.
    m_updates->primeCachedEtag(m_config->value(ConfigKeys::InternalUpdatesEtag, QString()).toString());
    m_updates->primeSkippedVersion(m_config->value(ConfigKeys::UpdatesSkippedVersion, QString()).toString());
    m_updates->primeLastChecked(QDateTime::fromString(
        m_config->value(ConfigKeys::InternalUpdatesLastCheckUtc, QString()).toString(),
        Qt::ISODate).toUTC());
    m_updates->primeNextAllowedCheck(QDateTime::fromString(
        m_config->value(ConfigKeys::InternalUpdatesNextAllowedCheckUtc, QString()).toString(),
        Qt::ISODate).toUTC());
    m_updates->setIncludeBetas(m_config->value(ConfigKeys::UpdatesIncludeBetas, false).toBool());
    m_updates->restoreCachedRelease();
    connect(m_config.get(), &ConfigManager::valueChanged, this,
            [this](const QString& key, const QVariant& value) {
        if (key != ConfigKeys::UpdatesIncludeBetas)
            return;
        m_updates->setIncludeBetas(value.toBool());
        // Opting in is a request to see betas now, not after the daily gate -
        // unless an update is already downloading or installing.
        const UpdateService::State state = m_updates->state();
        if (value.toBool()
            && (state == UpdateService::State::Idle || state == UpdateService::State::UpToDate
                || state == UpdateService::State::UpdateAvailable
                || state == UpdateService::State::Failed))
            m_updates->checkNow();
    });
    connect(m_updates.get(), &UpdateService::nextAllowedCheckChanged, this,
            [this](const QDateTime& when) {
        m_config->setValue(ConfigKeys::InternalUpdatesNextAllowedCheckUtc,
                            when.isValid() ? when.toUTC().toString(Qt::ISODate) : QString());
        m_config->save();
    });
    connect(m_updates.get(), &UpdateService::etagUpdated, this, [this](const QString& etag) {
        m_config->setValue(ConfigKeys::InternalUpdatesEtag, etag);
        m_config->save();
    });
    connect(m_updates.get(), &UpdateService::versionSkipped, this, [this](const QString& version) {
        m_config->setValue(ConfigKeys::UpdatesSkippedVersion, version);
        m_config->save();
    });
    connect(m_updates.get(), &UpdateService::lastCheckedChanged, this, [this] {
        m_config->setValue(ConfigKeys::InternalUpdatesLastCheckUtc,
                            m_updates->lastChecked().toUTC().toString(Qt::ISODate));
        m_config->save();
    });
    m_updatePreparationTimer = new QTimer(this);
    m_updatePreparationTimer->setSingleShot(true);
    m_updatePreparationTimer->setInterval(30000);
    auto finishUpdatePreparation = [this] {
        if (!m_updateScreenshotsReady || !m_updateReplayReady)
            return;
        m_updatePreparationTimer->stop();
        m_config->save();
        qInfo() << "Update preparation: capture services are quiescent and config is flushed";
        m_updates->markQuiescent();
    };
    connect(m_screenshots.get(), &ScreenshotService::updateReady, this, [this, finishUpdatePreparation] {
        m_updateScreenshotsReady = true;
        finishUpdatePreparation();
    });
    connect(m_framePump.get(), &FramePumpService::updateWaitingForExport, this, [] {
        qInfo() << "Update preparation: waiting for the current replay export to finish";
    });
    connect(m_framePump.get(), &FramePumpService::updateReady, this, [this, finishUpdatePreparation] {
        m_updateReplayReady = true;
        finishUpdatePreparation();
    });
    connect(m_updates.get(), &UpdateService::prepareForUpdateRequested, this, [this] {
        m_updateScreenshotsReady = false;
        m_updateReplayReady = false;
        m_updatePreparationTimer->start();
        m_screenshots->prepareForUpdate();
        m_framePump->prepareForUpdate();
    });
    connect(m_updates.get(), &UpdateService::installApproved, this,
            [this](const VerifiedUpdate &verified) {
        QString transactionPath;
        QString error;
        if (!UpdateInstaller::prepareTransaction(Paths::packageRoot(), Paths::dataDir(),
                                                  verified, transactionPath, error)) {
            m_screenshots->cancelUpdatePreparation();
            m_framePump->cancelUpdatePreparation();
            m_updates->cancelPreparation(error);
            qWarning() << "Update installer handoff failed:" << error;
            return;
        }
        std::string maintenanceError;
        if (!maintenance::begin(Paths::packageRoot().toStdWString(), maintenanceError)) {
            error = QString::fromStdString(maintenanceError);
            m_screenshots->cancelUpdatePreparation();
            m_framePump->cancelUpdatePreparation();
            m_updates->cancelPreparation(
                NativeText::get(
                    //: Update failure detail; %1 is the unchanged maintenance subsystem reason.
                    //% "GameHQ could not begin update maintenance: %1"
                    QT_TRID_NOOP("gamehq.error.update.maintenance_handoff_failed"),
                    "GameHQ could not begin update maintenance: %1").arg(error));
            qWarning() << "Update maintenance handoff failed:" << error;
            return;
        }
        m_integration->broadcastMaintenance(300);
        if (!UpdateInstaller::launchPrepared(Paths::packageRoot(), transactionPath, error)) {
            maintenance::finish(Paths::packageRoot().toStdWString());
            m_integration->cancelMaintenance();
            m_screenshots->cancelUpdatePreparation();
            m_framePump->cancelUpdatePreparation();
            m_updates->cancelPreparation(error);
            qWarning() << "Update installer handoff failed:" << error;
            return;
        }
        m_updates->markInstalling();
        qInfo() << "Updater helper launched for" << verified.version << "; exiting application";
        QTimer::singleShot(0, qApp, &QCoreApplication::quit);
    });
    connect(m_updatePreparationTimer, &QTimer::timeout, this, [this] {
        m_screenshots->cancelUpdatePreparation();
        m_framePump->cancelUpdatePreparation();
        m_updates->cancelPreparation(NativeText::get(
            //: Update installation error shown when active capture work cannot stop safely.
            //% "The update was canceled because capture work did not finish safely in time."
            QT_TRID_NOOP("gamehq.error.update.capture_quiescence_timeout"),
            "The update was canceled because capture work did not finish safely in time."));
        qWarning() << "Update preparation timed out; update cancelled without stopping capture work";
    });
    auto maybeAutoCheckForUpdate = [this](UpdateSchedule::Trigger trigger) {
        const char* name = trigger == UpdateSchedule::Trigger::Startup ? "startup" : "periodic";
        const QDateTime last = QDateTime::fromString(
            m_config->value(ConfigKeys::InternalUpdatesLastCheckUtc, QString()).toString(), Qt::ISODate);
        // A launch must discover releases published since the previous check.
        // GitHub's cooldown still wins, including across application restarts.
        switch (UpdateSchedule::decideAutomaticCheck(
                    trigger, m_config->value(ConfigKeys::UpdatesCheckAutomatically, true).toBool(),
                    m_updates->busy(), last, m_updates->nextAllowedCheck(),
                    QDateTime::currentDateTimeUtc())) {
        case UpdateSchedule::AutomaticDecision::Disabled:
            qInfo() << "Update check:" << name << "skipped; automatic checks are disabled";
            return;
        case UpdateSchedule::AutomaticDecision::Busy:
            qInfo() << "Update check:" << name << "skipped; update service is busy in state"
                    << m_updates->stateName();
            return;
        case UpdateSchedule::AutomaticDecision::Cooldown:
            qInfo() << "Update check:" << name << "waiting for GitHub cooldown until"
                    << m_updates->nextAllowedCheck();
            return;
        case UpdateSchedule::AutomaticDecision::RecentCheck:
            qInfo() << "Update check:" << name << "waiting for hourly interval; last check" << last;
            return;
        case UpdateSchedule::AutomaticDecision::Request:
            break;
        }
        qInfo() << "Update check:" << name << "requesting release metadata";
        m_updates->checkNow();
    };
    QTimer::singleShot(UpdateSchedule::kStartupDelayMs, m_updates.get(),
                       [maybeAutoCheckForUpdate] { maybeAutoCheckForUpdate(UpdateSchedule::Trigger::Startup); });
    m_updateCheckTimer = new QTimer(this);
    m_updateCheckTimer->setTimerType(Qt::PreciseTimer);
    m_updateCheckTimer->setInterval(UpdateSchedule::kAutomaticIntervalSecs * 1000);
    connect(m_updateCheckTimer, &QTimer::timeout, this,
            [maybeAutoCheckForUpdate] {
        maybeAutoCheckForUpdate(UpdateSchedule::Trigger::Periodic);
    });
    // Measure the next hour from the completed startup/manual check. Otherwise
    // the timer can fire a few seconds too early and skip a whole extra hour.
    connect(m_updates.get(), &UpdateService::lastCheckedChanged, m_updateCheckTimer,
            [this] { m_updateCheckTimer->start(); });
    m_updateCheckTimer->start();
}
