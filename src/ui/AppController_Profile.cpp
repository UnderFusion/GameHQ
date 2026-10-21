// AppController: data/log folders, portable-profile import and diagnostics.
#include "ui/AppController.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/Paths.h"
#include "core/ProcessIdentity.h"
#include "input/InputDiagnostics.h"
#include "localization/NativeText.h"
#include "ui/ShellActions.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QProcess>
#include <QSysInfo>
#include <QUrl>
#include <QVariantMap>

bool AppController::portableMode() const
{
    return Paths::isPortable();
}

QString AppController::dataRoot() const
{
    return QDir::toNativeSeparators(Paths::dataDir());
}

QString AppController::logsRoot() const
{
    return QDir::toNativeSeparators(Paths::logsDir());
}

void AppController::openDataFolder()
{
    ShellActions::openFile(Paths::dataDir());
}

void AppController::openLogsFolder()
{
    ShellActions::openFile(Paths::logsDir());
}

QString AppController::beginPortableImport(const QUrl& folderUrl)
{
    if (portableMode())
        return NativeText::get(
            //: Validation error shown in Settings before starting portable-profile import.
            //% "Portable profiles can only be imported by an installed copy of GameHQ."
            QT_TRID_NOOP("gamehq.error.portable_import.installed_copy_required"),
            "Portable profiles can only be imported by an installed copy of GameHQ.");
    if (!folderUrl.isLocalFile())
        return NativeText::get(
            //: Validation error shown in Settings before starting portable-profile import.
            //% "Select a local folder containing portable GameHQ."
            QT_TRID_NOOP("gamehq.error.portable_import.local_folder_required"),
            "Select a local folder containing portable GameHQ.");
    const QString source = QDir::cleanPath(folderUrl.toLocalFile());
    if (!QFileInfo(source + QStringLiteral("/portable.flag")).isFile()
        || !QFileInfo(source + QStringLiteral("/GameHQ.exe")).isFile()
        || !QFileInfo(source + QStringLiteral("/gamehq-data")).isDir())
        return NativeText::get(
            //: Validation error shown in Settings before starting portable-profile import.
            //% "The selected folder is not a GameHQ portable package."
            QT_TRID_NOOP("gamehq.error.portable_import.package_invalid"),
            "The selected folder is not a GameHQ portable package.");

    // Identify this process by more than its id: the importer refuses to start
    // until it can prove *this* instance has exited, and a bare id can name a
    // different program once Windows recycles it.
    const QString identity = ProcessIdentity::currentToken();
    if (identity.isEmpty())
        return NativeText::get(
            //: Portable-profile import launch failure shown in Settings.
            //% "GameHQ could not identify its own process for the import."
            QT_TRID_NOOP("gamehq.error.portable_import.process_identity_failed"),
            "GameHQ could not identify its own process for the import.");
    const QStringList arguments {
        QStringLiteral("--import-portable"), source,
        QStringLiteral("--wait-for-pid"), identity
    };
    if (!QProcess::startDetached(QCoreApplication::applicationFilePath(), arguments))
        return NativeText::get(
            //: Portable-profile import launch failure shown in Settings.
            //% "GameHQ could not start the portable import process."
            QT_TRID_NOOP("gamehq.error.portable_import.process_start_failed"),
            "GameHQ could not start the portable import process.");
    QCoreApplication::quit();
    return {};
}

void AppController::quitApplication()
{
    QCoreApplication::quit();
}

void AppController::copyDiagnosticSummary() const
{
    QVariantMap relevant;
    for (const auto& key : {ConfigKeys::CaptureMode, ConfigKeys::ReplayAuto,
            ConfigKeys::ReplayClipNotify, ConfigKeys::ReplayClipSound,
            ConfigKeys::ReplayLengthSeconds, ConfigKeys::ReplayManualIdleSeconds,
            ConfigKeys::InputDefaultHoldMs, ConfigKeys::InputMultiTapIntervalMs})
        relevant.insert(QString(key), m_config->value(key));
    QFile log(QDir(logsRoot()).filePath(QStringLiteral("gamehq.log")));
    QByteArray tail;
    if (log.open(QIODevice::ReadOnly)) {
        log.seek(qMax<qint64>(0, log.size() - InputDiagnostics::kMaxTraceBytes));
        tail = log.read(InputDiagnostics::kMaxTraceBytes);
    }
    QString build = version() + QStringLiteral(" compiled " __DATE__ " " __TIME__ " Qt " QT_VERSION_STR);
    QFile executable(QCoreApplication::applicationFilePath());
    QCryptographicHash digest(QCryptographicHash::Sha256);
    build += executable.open(QIODevice::ReadOnly) && digest.addData(&executable)
        ? QStringLiteral(" binary-sha256=") + QString::fromLatin1(digest.result().toHex())
        : QStringLiteral(" binary-sha256=unavailable");
    const QString summary = InputDiagnostics::instance().exportBetaText(
        build, QSysInfo::kernelVersion(), relevant, QString::fromUtf8(tail));
    QGuiApplication::clipboard()->setText(summary);
}
