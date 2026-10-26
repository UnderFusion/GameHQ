#include "app/App.h"
#include "capture/FramePumpService.h"
#include "capture/ReplayBufferState.h"
#include "capture/ScreenshotService.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "localization/NativeText.h"
#include "notify/NotificationCenter.h"
#include "sound/SoundEngine.h"
#include "ui/AppController.h"

#include <QDateTime>

namespace {

// Replay buffer rejections tell the user what to do next, so the toast shows
// them in the interface language. The English text stays the signal payload so
// logs and diagnostics keep one grammar; other technical reasons pass through.
QString localizedReplayReason(const QString& reason)
{
    if (reason == ReplayBufferState::saveRejection(ReplayBufferState::Starting))
        return NativeText::get(
            //: Replay failure reason: the replay buffer has just been started.
            //% "Replay buffer is starting; try saving again in a few seconds"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_starting"),
            "Replay buffer is starting; try saving again in a few seconds");
    if (reason == ReplayBufferState::saveRejection(ReplayBufferState::Recording))
        return NativeText::get(
            //: Replay failure reason: recording runs but no usable footage exists yet.
            //% "Replay buffer is collecting footage; try saving again in a few seconds"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_collecting"),
            "Replay buffer is collecting footage; try saving again in a few seconds");
    if (reason == ReplayBufferState::saveRejection(ReplayBufferState::Failed))
        return NativeText::get(
            //: Replay failure reason: the replay buffer stopped with an error.
            //% "Replay buffer failed; no clip was saved"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_buffer_failed"),
            "Replay buffer failed; no clip was saved");
    if (reason == ReplayBufferState::audioFailure())
        return NativeText::get(
            //: Replay failure reason: the Desktop audio source is selected but the default Windows output cannot be captured.
            //% "Desktop audio could not be recorded, so no clip was saved. Check the Windows sound output or change the audio source in Replay settings"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_desktop_audio"),
            "Desktop audio could not be recorded, so no clip was saved. Check the Windows sound output or change the audio source in Replay settings");
    if (reason == ReplayBufferState::gameAudioFailure())
        return NativeText::get(
            //: Replay failure reason: the Game audio source is selected but Windows could not capture the game's own audio.
            //% "Game audio could not be recorded, so no clip was saved. Choose Desktop audio or Off as the audio source in Replay settings"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_game_audio"),
            "Game audio could not be recorded, so no clip was saved. Choose Desktop audio or Off as the audio source in Replay settings");
    if (reason == ReplayBufferState::gameAudioUnsupported())
        return NativeText::get(
            //: Replay failure reason: the Game audio source is selected on a Windows version without per-app audio capture.
            //% "Game audio needs Windows 11, so no clip was saved. Choose Desktop audio or Off as the audio source in Replay settings"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_game_audio_unsupported"),
            "Game audio needs Windows 11, so no clip was saved. Choose Desktop audio or Off as the audio source in Replay settings");
    if (reason == ReplayBufferState::saveRejection(ReplayBufferState::Stopped))
        return NativeText::get(
            //: Replay failure reason: the replay buffer is turned off.
            //% "Replay buffer is not running"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_not_running"),
            "Replay buffer is not running");
    if (reason == QStringLiteral("Replay buffer is empty"))
        return NativeText::get(
            //: Replay failure reason: the replay buffer holds no footage yet.
            //% "Replay buffer is empty"
            QT_TRID_NOOP("gamehq.notification.replay_failed.reason_empty"),
            "Replay buffer is empty");
    return reason;
}

} // namespace

// Part of App::init(): capture services and their sound/toast feedback.
void App::wireCaptureFeedback()
{
    // Ordering contract for everything below: the service lambdas capture `this`
    // and dereference members that are constructed further down (m_sounds at the
    // SoundEngine block, m_notify at the NotificationCenter block). That is safe
    // only because nothing here emits before init() returns — the first capture
    // needs the pad/hotkeys, which m_input->start() arms last. Keep it that way:
    // if a service ever fires a signal from its own constructor, construct
    // m_sounds/m_notify before the connect that uses them.

    // Every capture outcome goes through here, so a request that was answered
    // with a receipt card replaces that card and one that was not still gets a
    // card of its own. update() fails when the receipt is already gone -
    // dismissed by the user, or pushed out of the visible stack - and an
    // outcome nobody sees is the bug this whole path exists to prevent.
    const auto postCaptureOutcome = [this](quint64 operationId, const QString& title,
            const QString& body, const QString& image, const QString& kind,
            const QDateTime& when, bool video) {
        if (operationId && m_notify->update(operationId, title, body, image, kind, when, video))
            return;
        m_notify->post(title, body, image, kind, when, video);
    };
    // "The encoder finished" and "the library has it" are two different claims.
    // A rejected insert leaves a real file on disk that no gallery will ever
    // show, so the toast names which of the two happened instead of saying
    // "saved" and sending the user after a thumbnail that does not exist.
    const auto commitOutcomeTitle = [](CaptureCommitOutcome outcome, const QString& savedTitle,
                                       const QString& failedTitle) {
        if (outcome == CaptureCommitOutcome::Indexed)
            return savedTitle;
        if (outcome == CaptureCommitOutcome::Failed)
            return failedTitle;
        return NativeText::get(
            //: Shown when a capture file was written but could not be added to the library.
            //% "Saved to disk, not in library"
            QT_TRID_NOOP("gamehq.notification.capture_media_only.title"),
            "Saved to disk, not in library");
    };
    const auto commitOutcomeBody = [](CaptureCommitOutcome outcome, const QString& savedBody) {
        if (outcome == CaptureCommitOutcome::Indexed)
            return savedBody;
        if (outcome == CaptureCommitOutcome::MediaOnly) {
            return NativeText::get(
                //: Body shown when a capture file exists on disk but has no library entry.
                //% "The file is in your captures folder, but it could not be added to the library."
                QT_TRID_NOOP("gamehq.notification.capture_media_only.body"),
                "The file is in your captures folder, but it could not be added to the library.");
        }
        return NativeText::get(
            //: Body shown when a capture is neither in the library nor on disk.
            //% "It was not added to the library and the file is not on disk."
            QT_TRID_NOOP("gamehq.notification.capture_lost.body"),
            "It was not added to the library and the file is not on disk.");
    };
    // One title for every screenshot dead end: a refused request, a failed grab
    // and a capture that never reached the library all read the same way.
    const auto screenshotFailedTitle = [] {
        return NativeText::get(
            //: Title of the notification shown when a screenshot could not be taken or kept.
            //% "Screenshot failed"
            QT_TRID_NOOP("gamehq.notification.screenshot_failed.title"),
            "Screenshot failed");
    };
    const auto replayFailedTitle = [] {
        return NativeText::get(
            //: Title of the notification shown when saving a replay clip fails.
            //% "Replay failed"
            QT_TRID_NOOP("gamehq.notification.replay_failed.title"),
            "Replay failed");
    };
    // Screenshot capture (0.4): GDI grab of the foreground game → DB/gallery.
    m_screenshots = std::make_unique<ScreenshotService>(m_config.get(),
                                                        m_locations.get());
    // Let the QML bridge save clip frames (Share on a focused clip) through the
    // same screenshot pipeline — same folder, format, sound and toast.
    m_controller->setScreenshotService(m_screenshots.get());
    // Shutter plays the instant the pixels are grabbed (before the async encode),
    // so the feedback is immediate even though the PNG lands a moment later.
    connect(m_screenshots.get(), &ScreenshotService::grabbed, this,
            [this] {
                if (m_config->value(ConfigKeys::CaptureScreenshotSound, true).toBool())
                    m_sounds->play(QStringLiteral("screenshot"));
            });
    connect(m_screenshots.get(), &ScreenshotService::captured, this,
            [this, postCaptureOutcome, commitOutcomeTitle, commitOutcomeBody,
             screenshotFailedTitle](const QString& path, const QString& game,
                                    const QString& exePath, quint64 operationId) {
                const CaptureCommitOutcome outcome =
                    m_controller->commitCapture(path, QStringLiteral("screenshot"), game, exePath);
                const bool indexed = outcome == CaptureCommitOutcome::Indexed;
                if (!indexed)
                    m_sounds->play(QStringLiteral("error"));
                if (!m_config->value(ConfigKeys::NotificationsEnabled, true).toBool())
                    return;
                // A commit that failed is reported even when screenshot toasts
                // are off: that switch mutes successes, it does not agree to
                // leave a capture invisible.
                if (indexed && !m_config->value(ConfigKeys::CaptureScreenshotNotify, true).toBool())
                    return;
                const QString savedTitle = NativeText::get(
                    //: Title of the notification shown after a screenshot is saved.
                    //% "Screenshot saved"
                    QT_TRID_NOOP("gamehq.notification.screenshot_saved.title"),
                    "Screenshot saved");
                postCaptureOutcome(operationId,
                    commitOutcomeTitle(outcome, savedTitle, screenshotFailedTitle()),
                    commitOutcomeBody(outcome, game),
                    indexed ? path : QString(),
                    indexed ? QStringLiteral("success") : QStringLiteral("error"),
                    QDateTime::currentDateTime(), false);
            });
    // Skipped and failed are one story for the user: the press produced no
    // screenshot, and the reason belongs on screen. Neither is gated on the
    // screenshot toast toggle, which mutes successes, not dead ends.
    const auto reportScreenshotFailure = [this, postCaptureOutcome, screenshotFailedTitle](
            const QString& why, quint64 operationId) {
        m_sounds->play(QStringLiteral("error"));
        if (!m_config->value(ConfigKeys::NotificationsEnabled, true).toBool())
            return;
        postCaptureOutcome(operationId, screenshotFailedTitle(), why, QString(),
                           QStringLiteral("error"), QDateTime::currentDateTime(), false);
    };
    connect(m_screenshots.get(), &ScreenshotService::skipped, this,
            [reportScreenshotFailure](const QString& why, quint64 operationId) {
                qInfo() << "Screenshot: skipped —" << why;
                reportScreenshotFailure(why, operationId);
            });
    connect(m_screenshots.get(), &ScreenshotService::failed, this,
            [reportScreenshotFailure](const QString& why, quint64 operationId) {
                qWarning() << "Screenshot: failed —" << why;
                reportScreenshotFailure(why, operationId);
            });

    // WGC replay buffer (0.5): records continuously while a game is foreground
    // (replay.auto, always-on; Settings → Replay is the master switch). Share-hold
    // (or Ctrl+Shift+E) saves the last N seconds as one clip (below).
    m_framePump = std::make_unique<FramePumpService>(m_config.get(), m_locations.get());
    // Both services acknowledge on this thread before any admission gate or work.
    const auto acknowledgeCapture = [this](const CaptureRequest& request, CaptureRequest::Kind kind) {
        const bool replay = kind == CaptureRequest::Kind::Replay;
        const bool sound = m_config->value(replay ? ConfigKeys::ReplayClipSound
                                                 : ConfigKeys::CaptureScreenshotSound, true).toBool();
        const bool toast = m_config->value(ConfigKeys::NotificationsEnabled, true).toBool()
            && m_config->value(ConfigKeys::NotificationsCaptureReceived, true).toBool()
            && m_config->value(replay ? ConfigKeys::ReplayClipNotify
                                     : ConfigKeys::CaptureScreenshotNotify, true).toBool();
        if (sound)
            m_sounds->play(QStringLiteral("capture_accepted"));
        if (toast) {
            const QString title = replay
                ? NativeText::get(
                    //: A replay save request was received; saving may still fail.
                    //% "Replay save requested"
                    QT_TRID_NOOP("gamehq.notification.replay_requested.title"),
                    "Replay save requested")
                : NativeText::get(
                    //: A screenshot request was received; capture may still fail.
                    //% "Capture request received"
                    QT_TRID_NOOP("gamehq.notification.capture_requested.title"),
                    "Capture request received");
            m_notify->post(request.id, title, {}, {}, QStringLiteral("info"));
        }
        qInfo().noquote() << QStringLiteral(
            "CaptureFeedback[%1]: kind=%2 submitted elapsedMs=%3 sound=%4 toast=%5")
            .arg(request.tag(), replay ? QStringLiteral("replay") : QStringLiteral("screenshot"))
            .arg(request.ageMs()).arg(sound).arg(toast);
    };
    connect(m_screenshots.get(), &ScreenshotService::requestAccepted, this, acknowledgeCapture);
    connect(m_framePump.get(), &FramePumpService::requestAccepted, this, acknowledgeCapture);
    connect(m_screenshots.get(), &ScreenshotService::hdrCaptureRequested,
            m_framePump.get(), &FramePumpService::captureHdrScreenshot);
    connect(m_framePump.get(), &FramePumpService::hdrScreenshotReady,
            m_screenshots.get(), &ScreenshotService::saveImage);
    connect(m_framePump.get(), &FramePumpService::hdrScreenshotFailed,
            m_screenshots.get(), &ScreenshotService::reportHdrCaptureFailure);
    connect(m_framePump.get(), &FramePumpService::failed, this,
            [this](const QString& why) { qWarning() << "FramePump: failed —" << why; });
    // The ring freezes immediately, but do not say "saved" until the final MP4
    // exists and the gallery commit path runs. Otherwise a remux failure sounds
    // successful while no thumbnail appears.
    connect(m_framePump.get(), &FramePumpService::clipSaving, this,
            [](const QString&, const QString&, const QString&) {
            });
    // Remux finished: file + thumbnail ready → add it to the gallery and notify.
    connect(m_framePump.get(), &FramePumpService::clipSaved, this,
            [this, postCaptureOutcome, commitOutcomeTitle, commitOutcomeBody, replayFailedTitle](
                   const QString& path, const QString& game, const QString& thumb,
                   const QString& exePath, quint64 operationId) {
                const CaptureCommitOutcome outcome =
                    m_controller->commitClip(path, game, thumb, exePath);
                const bool indexed = outcome == CaptureCommitOutcome::Indexed;
                if (!indexed)
                    m_sounds->play(QStringLiteral("error"));
                else if (m_config->value(ConfigKeys::ReplayClipSound, true).toBool())
                    m_sounds->play(QStringLiteral("replay_saved"));
                if (!m_config->value(ConfigKeys::NotificationsEnabled, true).toBool())
                    return;
                if (indexed && !m_config->value(ConfigKeys::ReplayClipNotify, true).toBool())
                    return;
                const QString savedTitle = NativeText::get(
                    //: Title of the notification shown after a replay clip is saved.
                    //% "Replay saved"
                    QT_TRID_NOOP("gamehq.notification.replay_saved.title"),
                    "Replay saved");
                postCaptureOutcome(operationId,
                    commitOutcomeTitle(outcome, savedTitle, replayFailedTitle()),
                    commitOutcomeBody(outcome, game),
                    indexed ? thumb : QString(),
                    indexed ? QStringLiteral("success") : QStringLiteral("error"),
                    QDateTime::currentDateTime(), indexed);
            });
    connect(m_framePump.get(), &FramePumpService::clipFailed, this,
            [this, postCaptureOutcome, replayFailedTitle](
                    const QString& game, const QString& reason, quint64 operationId) {
                m_sounds->play(QStringLiteral("error"));
                if (m_config->value(ConfigKeys::NotificationsEnabled, true).toBool()) {
                    const QString body = reason.isEmpty()
                        ? game
                        : NativeText::get(
                              //: Replay failure detail; %1 is the unchanged technical reason.
                              //% "Reason: %1"
                              QT_TRID_NOOP("gamehq.notification.replay_failed.reason"),
                              "Reason: %1").arg(localizedReplayReason(reason));
                    postCaptureOutcome(operationId, replayFailedTitle(), body, QString(),
                                       QStringLiteral("error"), QDateTime::currentDateTime(), false);
                }
            });
    connect(m_framePump.get(), &FramePumpService::foregroundGameDetected,
            m_controller.get(), &AppController::updateForegroundGame);
    connect(m_framePump.get(), &FramePumpService::bufferStateChanged,
            m_controller.get(), &AppController::updateReplayBufferState);
    // A delete that kept the row used to be completely silent: the capture just
    // stayed put with no explanation. Say it out loud. A failed QFile::remove is
    // not proof of a lock — permissions, read-only files and antivirus hold-offs
    // land here too — so the wording offers the likely cause without claiming it.
    connect(m_controller.get(), &AppController::captureDeletionFailed, this,
            [this](int count) {
                m_sounds->play(QStringLiteral("error"));
                if (!m_config->value(ConfigKeys::NotificationsEnabled, true).toBool())
                    return;
                const QString body = count > 1
                    ? NativeText::get(
                          //: Body of the notification shown when several captures could not be
                          //: deleted. The cause is not known for certain, so the text stays
                          //: tentative and names the action most likely to help.
                          //% "Some files couldn't be deleted. They may be in use by other programs. Close any programs using them and try again."
                          QT_TRID_NOOP("gamehq.notification.capture_delete_failed.body_many"),
                          "Some files couldn't be deleted. They may be in use by other programs. Close any programs using them and try again.")
                    : NativeText::get(
                          //: Body of the notification shown when a capture could not be deleted.
                          //: The cause is not known for certain, so the text stays tentative and
                          //: names the action most likely to help.
                          //% "The file couldn't be deleted. It may be in use by another program. Close any program using it and try again."
                          QT_TRID_NOOP("gamehq.notification.capture_delete_failed.body_one"),
                          "The file couldn't be deleted. It may be in use by another program. Close any program using it and try again.");
                m_notify->post(
                    NativeText::get(
                        //: Title of the notification shown when a capture could not be deleted.
                        //% "Couldn't delete"
                        QT_TRID_NOOP("gamehq.notification.capture_delete_failed.title"),
                        "Couldn't delete"),
                    body, QString(), QStringLiteral("error"), QDateTime::currentDateTime(), false);
            });

    // Delete never touches files in imported or watched folders. Without a word
    // the capture would just stay put, which reads as a failed delete.
    connect(m_controller.get(), &AppController::captureDeletionKeptImported, this,
            [this](int count) {
                m_sounds->play(QStringLiteral("error"));
                if (!m_config->value(ConfigKeys::NotificationsEnabled, true).toBool())
                    return;
                const QString body = count > 1
                    ? NativeText::get(
                          //: Body of the notification shown when a delete reached several
                          //: captures that live in imported folders. GameHQ never deletes
                          //: originals there, so those files stay on disk and in the library.
                          //% "Some files are in watched folders. GameHQ doesn't delete originals there, so they were left untouched."
                          QT_TRID_NOOP("gamehq.notification.capture_delete_kept_imported.body_many"),
                          "Some files are in watched folders. GameHQ doesn't delete originals there, so they were left untouched.")
                    : NativeText::get(
                          //: Body of the notification shown when a delete reached a capture
                          //: that lives in an imported folder. GameHQ never deletes originals
                          //: there, so the file stays on disk and in the library.
                          //% "This file is in a watched folder. GameHQ doesn't delete originals there, so it was left untouched."
                          QT_TRID_NOOP("gamehq.notification.capture_delete_kept_imported.body_one"),
                          "This file is in a watched folder. GameHQ doesn't delete originals there, so it was left untouched.");
                m_notify->post(
                    NativeText::get(
                        //: Title of the notification shown when a capture could not be deleted.
                        //% "Couldn't delete"
                        QT_TRID_NOOP("gamehq.notification.capture_delete_failed.title"),
                        "Couldn't delete"),
                    body, QString(), QStringLiteral("error"), QDateTime::currentDateTime(), false);
            });

    // Settings changed a replay.* key (fps/resolution/length): re-arm a
    // running buffer so it records with the new parameters immediately.
    connect(m_controller.get(), &AppController::replaySettingsChanged,
            m_framePump.get(), &FramePumpService::restartBuffer);
    // HDR can be toggled while a game is running. Re-arm immediately so the
    // rolling buffer never combines SDR and HDR capture formats.
    connect(m_controller.get(), &AppController::hdrDisplayConfigurationChanged,
            m_framePump.get(), &FramePumpService::restartBuffer);
}
