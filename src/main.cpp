// GameHQ entry point — keep thin: hand off to App.
// QApplication (not QGuiApplication) because QSystemTrayIcon needs Widgets.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QLockFile>
#include <QMessageBox>
#include <QSaveFile>
#include <QProcess>
#include <QTimer>
#include "app/App.h"
#include "app/PackagedLocalizationProbe.h"
#include "Brand.h"
#include "config/Paths.h"
#include "config/PortableProfileImporter.h"
#include "input/HidCloakMonitor.h"
#include "integration/IntegrationClient.h"
#include "localization/NativeText.h"
#include "localization/StartupLocalization.h"
#include "core/ApplicationMutex.h"
#include "core/DllSearchPolicy.h"
#include "core/PathContainment.h"
#include "core/ProcessIdentity.h"
#include "core/UpdateMaintenance.h"
#include "diagnostics/CrashDump.h"
#include "diagnostics/Logger.h"
#include "security/ReleaseTrust.h"

#include <cstring>
#include <iostream>
#include <windows.h>

namespace
{
bool isPostUpdateTokenPath(const QString& path)
{
    const QString updateRoot = QFileInfo(Paths::packageRoot()
        + QStringLiteral("/.update")).absoluteFilePath();
    const QString token = QFileInfo(path).absoluteFilePath();
    // QDir::relativeFilePath answers with an absolute path across drives or
    // shares, which a parent-prefix test accepted; compare components instead.
    return path_containment::isWithinRoot(updateRoot.toStdWString(), token.toStdWString());
}

QString updatePhase()
{
    QFile file(Paths::packageRoot() + QStringLiteral("/.update/transaction.phase"));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readLine()).trimmed();
}

bool interruptedUpdateExists()
{
    return maintenance::recoveryRequired(Paths::packageRoot().toStdWString());
}

void cleanCompletedUpdateArtifacts()
{
    std::string error;
    if (maintenance::cleanupCompleted(Paths::packageRoot().toStdWString(), error))
        qInfo() << "Previous update recovery artifacts cleaned after a second healthy start";
    else
        qWarning().noquote() << "Completed update cleanup will be retried on the next start:"
                             << QString::fromStdString(error);
}

// A start that cannot continue used to exit with no word at all. Record why,
// flush the early log buffer to disk so the reason survives, and - unless an
// automated caller (smoke test, the updater's post-update run) is waiting on
// this process - tell the user where to look. `reason` goes to the log in
// English; `message` is the localized dialog text. Every interactive caller
// runs after StartupLocalization is initialized, and NativeText falls back to
// English if the embedded catalogs could not be loaded.
int failStartup(int exitCode, const QString& reason, const QString& message,
                bool interactive, QMessageBox::Icon icon = QMessageBox::Critical,
                const QString& extraLog = {})
{
    qCritical().noquote() << "Startup:" << reason << "(exit code" << exitCode << ')';
    Logger::install(Paths::logsDir());
    if (!interactive)
        return exitCode;
    QString text = message + QStringLiteral("\n\n")
        + NativeText::get(
            //: Fatal start-up dialog; one or more log file paths follow on the next lines.
            //% "For details, see:"
            QT_TRID_NOOP("gamehq.startup.fatal.details_hint"),
            "For details, see:")
        + QLatin1Char('\n')
        + QDir::toNativeSeparators(Logger::filePath(Paths::logsDir()));
    if (!extraLog.isEmpty())
        text += QLatin1Char('\n') + QDir::toNativeSeparators(extraLog);
    QMessageBox box(icon, QString::fromLatin1(Brand::Name), text, QMessageBox::Ok);
    box.setTextInteractionFlags(Qt::TextSelectableByMouse);
    box.exec();
    return exitCode;
}

bool waitForParentProcess(const QStringList& arguments, QString& error)
{
    const qsizetype at = arguments.indexOf(QStringLiteral("--wait-for-pid"));
    if (at < 0)
        return true;
    if (at + 1 >= arguments.size()) {
        error = NativeText::get(
            //: Portable-profile import error shown before the main application starts.
            //% "The portable import parent-process argument is incomplete."
            QT_TRID_NOOP("gamehq.startup.portable_import.parent_argument_incomplete"),
            "The portable import parent-process argument is incomplete.");
        return false;
    }
    // The token carries the parent's creation time, so a recycled process id
    // cannot make this wait on a stranger, and a parent we are not allowed to
    // inspect is reported instead of assumed gone.
    switch (ProcessIdentity::waitForExit(arguments.at(at + 1), 60000)) {
    case ProcessIdentity::WaitOutcome::Exited:
        return true;
    case ProcessIdentity::WaitOutcome::StillRunning:
        error = NativeText::get(
            //: Portable-profile import error shown before the main application starts.
            //% "The running GameHQ instance did not close in time."
            QT_TRID_NOOP("gamehq.startup.portable_import.parent_still_running"),
            "The running GameHQ instance did not close in time.");
        return false;
    case ProcessIdentity::WaitOutcome::Unverifiable:
        error = NativeText::get(
            //: Portable-profile import error shown before the main application starts.
            //% "GameHQ could not confirm that the previous instance has closed."
            QT_TRID_NOOP("gamehq.startup.portable_import.parent_unverifiable"),
            "GameHQ could not confirm that the previous instance has closed.");
        return false;
    case ProcessIdentity::WaitOutcome::Malformed:
        error = NativeText::get(
            //: Portable-profile import error shown before the main application starts.
            //% "The portable import parent-process identifier is invalid."
            QT_TRID_NOOP("gamehq.startup.portable_import.parent_identifier_invalid"),
            "The portable import parent-process identifier is invalid.");
        return false;
    }
    error = NativeText::get(
        //% "The portable import parent-process identifier is invalid."
        QT_TRID_NOOP("gamehq.startup.portable_import.parent_identifier_invalid"),
        "The portable import parent-process identifier is invalid.");
    return false;
}
}

int main(int argc, char* argv[])
{
    dll_search::restrictSearchPath();

    // Parse this packaging probe from the raw Windows command line. GUI-
    // subsystem CRT wrappers are not consistent about populating narrow argv
    // before QApplication exists.
    const std::wstring rawCommand = GetCommandLineW();
    if (rawCommand.find(L"--assert-version") != std::wstring::npos) {
        #define GAMEHQ_WIDEN_IMPL(value) L##value
        #define GAMEHQ_WIDEN(value) GAMEHQ_WIDEN_IMPL(value)
        const std::wstring expected = GAMEHQ_WIDEN(GAMEHQ_VERSION);
        #undef GAMEHQ_WIDEN
        #undef GAMEHQ_WIDEN_IMPL
        return rawCommand.find(L"--assert-version " + expected) != std::wstring::npos ? 0 : 2;
    }
    if (rawCommand.find(L"--release-trust-self-test") != std::wstring::npos) {
        std::string error;
        if (!release_trust::runBuiltInSelfTest(error)) {
            std::cerr << "Release trust self-test failed: " << error << '\n';
            return 6;
        }
        return 0;
    }
    if (rawCommand.find(L"--localization-assets-self-test") != std::wstring::npos) {
        QCoreApplication probeApp(argc, argv);
        const QStringList arguments = probeApp.arguments();
        const qsizetype option = arguments.indexOf(QStringLiteral("--localization-assets-self-test"));
        if (option < 0 || option + 1 >= arguments.size())
            return 7;
        QString error;
        if (!PackagedLocalizationProbe::run(arguments.at(option + 1), &error)) {
            std::cerr << "Packaged localization self-test failed: "
                      << error.toUtf8().constData() << '\n';
            return 8;
        }
        return 0;
    }
    // Elevated helper mode (no GUI, no single-instance lock): the Settings
    // "Fix automatically" button relaunches this exe with "runas" so the
    // HidHide whitelist IOCTL runs with admin rights (docs/controller-input.md).
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--hidhide-allow-self") == 0)
            return HidCloakMonitor::applyWhitelistSelfElevated();
    }

    // Capture every message from here on. The file is opened only once this
    // process owns the instance lock (or must exit): portable import replaces
    // the data folder that holds the log, and a rejected second instance must
    // not write into the running instance's log.
    Logger::beginBuffering();

    QApplication qtApp(argc, argv);
    QApplication::setApplicationName(QString::fromLatin1(Brand::Name));
    QApplication::setOrganizationName(QString::fromLatin1(Brand::Name));
    QApplication::setApplicationVersion(QStringLiteral(GAMEHQ_VERSION));
    QApplication::setQuitOnLastWindowClosed(false); // tray-resident by design
    // Default icon for every window → title bar + taskbar (tray sets its own too).
    // The .ico carries pre-rendered 16–256 px frames, so every surface gets a
    // crisp raster instead of relying on the SVG icon engine at odd sizes.
    QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/gamehq.ico")));

    // Portable-profile import can fail before App loads the persisted language.
    // Resolve the system locale here so every native startup dialog is translated;
    // NativeText still supplies English if the embedded catalogs cannot be read.
    StartupLocalization startupLocalization;
    QString startupLocalizationError;
    if (!startupLocalization.initialize(&startupLocalizationError))
        qWarning().noquote() << "Startup localization unavailable:" << startupLocalizationError;

    bool postUpdateValidation = false;
    QString postUpdateVersion;
    QString postUpdateToken;
    const QStringList arguments = qtApp.arguments();
    const bool smokeTest = arguments.contains(QStringLiteral("--smoke-test"));
    const qsizetype importAt = arguments.indexOf(QStringLiteral("--import-portable"));
    if (importAt >= 0) {
        QString importError;
        if (importAt + 1 >= arguments.size())
            importError = NativeText::get(
                //: Portable-profile import error shown before the main application starts.
                //% "The portable import source folder is missing."
                QT_TRID_NOOP("gamehq.startup.portable_import.source_missing"),
                "The portable import source folder is missing.");
        else if (Paths::isPortable())
            importError = NativeText::get(
                //: Portable-profile import error shown before the main application starts.
                //% "Run portable import from an installed copy of GameHQ."
                QT_TRID_NOOP("gamehq.startup.portable_import.installed_copy_required"),
                "Run portable import from an installed copy of GameHQ.");
        else if (waitForParentProcess(arguments, importError)) {
            // The import replaces the whole data folder, so nothing else may
            // hold it. The parent has provably exited by now, so take the same
            // lock a normal start takes and keep it for the whole import - this
            // used to run before any instance gate at all.
            QLockFile importLock(QDir::tempPath() + QStringLiteral("/gamehq.lock"));
            if (!importLock.tryLock(1000)) {
                importError = NativeText::get(
                    //: Portable-profile import error shown before the main application starts.
                    //% "Another GameHQ instance is running, so the portable import cannot start."
                    QT_TRID_NOOP("gamehq.startup.portable_import.instance_running"),
                    "Another GameHQ instance is running, so the portable import cannot start.");
            } else {
                PortableProfileImporter::Result result;
                PortableProfileImporter::Options options {
                    arguments.at(importAt + 1), Paths::dataDir(),
                    PortableProfileImporter::FailurePoint::None
                };
                PortableProfileImporter::importProfile(options, result, importError);
            }
        }
        if (!importError.isEmpty()) {
            if (arguments.contains(QStringLiteral("--import-portable-only"))) {
                qCritical().noquote() << importError;
                return 7;
            }
            QMessageBox::critical(
                nullptr,
                NativeText::get(
                    //: Title of the native portable-profile import failure dialog.
                    //% "Portable import failed"
                    QT_TRID_NOOP("gamehq.startup.portable_import.failed_title"),
                    "Portable import failed"),
                importError);
            return 7;
        }
        if (arguments.contains(QStringLiteral("--import-portable-only")))
            return 0;
    }
    const qsizetype postUpdateAt = arguments.indexOf(QStringLiteral("--post-update"));
    if (postUpdateAt >= 0) {
        // Only the updater passes --post-update and it judges the outcome
        // itself, so these exits are logged but never block on a dialog.
        if (postUpdateAt + 2 >= arguments.size())
            return failStartup(2, QStringLiteral("The post-update arguments are incomplete."),
                               {}, false);
        postUpdateVersion = arguments.at(postUpdateAt + 1);
        postUpdateToken = arguments.at(postUpdateAt + 2);
        if (postUpdateVersion != QStringLiteral(GAMEHQ_VERSION)
            || !isPostUpdateTokenPath(postUpdateToken))
            return failStartup(2, QStringLiteral("The post-update arguments do not match this build."),
                               {}, false);
        postUpdateValidation = true;
    }

    if (!postUpdateValidation && interruptedUpdateExists()) {
        const QString helper = Paths::packageRoot() + QStringLiteral("/GameHQUpdater.exe");
        const QString transaction = Paths::packageRoot() + QStringLiteral("/.update/transaction.json");
        const QString updaterLog = Paths::packageRoot() + QStringLiteral("/.update/updater.log");
        const QString recoveryFailed = QStringLiteral(
            "GameHQ could not finish recovering from an interrupted update, so it cannot start.");
        const QString recoveryFailedMessage = NativeText::get(
            //: Fatal start-up dialog shown when an interrupted update cannot be recovered.
            //% "GameHQ could not finish recovering from an interrupted update, so it cannot start."
            QT_TRID_NOOP("gamehq.startup.fatal.update_recovery_failed"),
            "GameHQ could not finish recovering from an interrupted update, so it cannot start.");
        if (!QFileInfo(helper).isExecutable() || !QFileInfo(transaction).isFile()) {
            qCritical() << "Startup: interrupted update found but the updater helper or its"
                           " transaction is missing";
            return failStartup(3, recoveryFailed, recoveryFailedMessage, !smokeTest,
                               QMessageBox::Critical, updaterLog);
        }
        // Recovery restarts the previous version itself. Run before taking the
        // app lock so that restarted process is not rejected as a duplicate.
        const int recovery = QProcess::execute(helper, { QStringLiteral("--recover"), transaction });
        if (recovery == 0)
            return 0;
        qCritical() << "Startup: update recovery helper exited with" << recovery;
        return failStartup(3, recoveryFailed, recoveryFailedMessage, !smokeTest,
                           QMessageBox::Critical, updaterLog);
    }
    const QString phaseAtStart = updatePhase();
    const bool completedUpdateAtStart = !postUpdateValidation
        && (phaseAtStart == QStringLiteral("healthy")
            || phaseAtStart == QStringLiteral("rolled_back"));

    if (!postUpdateValidation) {
        const maintenance::Info state = maintenance::inspect(
            Paths::packageRoot().toStdWString());
        if (state.state == maintenance::State::Active)
            return failStartup(4, QStringLiteral(
                                   "A GameHQ update is being installed. Start GameHQ again "
                                   "when it has finished."),
                               NativeText::get(
                                   //: Start-up dialog shown while the updater is replacing files.
                                   //% "A GameHQ update is being installed. Start GameHQ again when it has finished."
                                   QT_TRID_NOOP("gamehq.startup.fatal.update_in_progress"),
                                   "A GameHQ update is being installed. Start GameHQ again "
                                   "when it has finished."),
                               !smokeTest, QMessageBox::Information);
    }

    // QLockFile remains the source of truth. A rejected second process only
    // uses the local channel to ask the existing UI to come forward, then exits.
    QLockFile instanceLock(QDir::tempPath() + QStringLiteral("/gamehq.lock"));
    if (!instanceLock.tryLock(100)) {
        QString forwardingError;
        if (!IntegrationClient::forwardSecondInstance(arguments,
                                                       QStringLiteral(GAMEHQ_VERSION),
                                                       forwardingError)) {
            qWarning() << "Second-instance activation was not acknowledged:"
                       << forwardingError;
        }
        return 0;
    }
    // This copy owns the instance lock, so the log file is now ours to write;
    // everything buffered since startup goes out first.
    Logger::install(Paths::logsDir());
    // From here an unhandled crash leaves a small local minidump next to the log.
    CrashDump::install(Paths::logsDir());

    // Setup and Uninstall observe this process-lifetime mutex. It is acquired
    // only after QLockFile accepts this copy, so rejected second instances do
    // not make the installer think GameHQ is still running.
    ApplicationMutex applicationMutex;
    if (!applicationMutex.acquired())
        return failStartup(5, QStringLiteral(
                               "GameHQ cannot start while another copy of GameHQ or its "
                               "installer is running."),
                           NativeText::get(
                               //: Fatal start-up dialog; another GameHQ copy or Setup/Uninstall holds the application mutex.
                               //% "GameHQ cannot start while another copy of GameHQ or its installer is running."
                               QT_TRID_NOOP("gamehq.startup.fatal.another_copy_running"),
                               "GameHQ cannot start while another copy of GameHQ or its "
                               "installer is running."),
                           !smokeTest && !postUpdateValidation);

    // The session marker says "this run never reached an orderly shutdown".
    // It goes only after App and everything it owns are destroyed, so a crash
    // anywhere in teardown still shows up as a crash on the next launch.
    const auto finish = [](int exitCode) {
        QFile::remove(Paths::dataDir() + QStringLiteral("/session.marker"));
        return exitCode;
    };
    int exitCode = 0;
    bool initFailed = false;
    {
        App app;
        app.setPostUpdateValidation(postUpdateValidation);
        // A failed init still tears App down before the marker goes.
        if (!app.init()) {
            exitCode = 1;
            initFailed = true;
        } else {
            // Local/CI packaged-layout acceptance: exercise normal initialization,
            // enter the Qt event loop, then take the same orderly teardown path as the
            // tray Exit action. The session marker and log prove destruction ran.
            if (smokeTest)
                QTimer::singleShot(2000, &qtApp, &QCoreApplication::quit);

            if (postUpdateValidation) {
                QTimer::singleShot(7000, &app, [&app, postUpdateVersion, postUpdateToken] {
                    // Persist the one-time greeting before publishing health: a token
                    // means all post-update bookkeeping is durable too.
                    app.recordPostUpdateSuccess(postUpdateVersion);
                    QSaveFile token(postUpdateToken);
                    const QByteArray contents = postUpdateVersion.toUtf8() + '\n';
                    if (!token.open(QIODevice::WriteOnly)
                        || token.write(contents) != contents.size()
                        || !token.commit()) {
                        qCritical() << "Post-update validation could not publish its health token";
                        return;
                    }
                    qInfo() << "Post-update validation succeeded for" << postUpdateVersion;
                    app.completePostUpdateValidation();
                });
            } else if (completedUpdateAtStart) {
                // Retain rollback material through the first validation run. Only a
                // subsequent start that also survives seven seconds may discard it.
                QTimer::singleShot(7000, &app, [] {
                    cleanCompletedUpdateArtifacts();
                });
            }

            exitCode = qtApp.exec();
        }
    }
    // Reported only after App and its windows are gone; init() has already
    // logged the specific reason.
    if (initFailed)
        failStartup(exitCode, QStringLiteral("GameHQ could not start."),
                    NativeText::get(
                        //: Fatal start-up dialog shown when application initialization fails.
                        //% "GameHQ could not start."
                        QT_TRID_NOOP("gamehq.startup.fatal.init_failed"),
                        "GameHQ could not start."),
                    !smokeTest && !postUpdateValidation);
    return finish(exitCode);
}
