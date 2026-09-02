#include "updates/UpdateInstaller.h"
#include "updates/UpdateDownloader.h"
#include "core/UpdaterHandshake.h"
#include "localization/NativeText.h"

#include <QCoreApplication>
#include <QDir>
#include <QCryptographicHash>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QRegularExpression>

#include <limits>

#include <windows.h>

namespace UpdateInstaller
{
bool prepareTransaction(const QString &packageRoot, const QString &dataDir,
                        const VerifiedUpdate &verified, QString &transactionPath,
                        QString &error)
{
    error.clear();
    if (!verified.isValid()) {
        error = NativeText::get(
            //: Update installer handoff validation failure.
            //% "The verified update metadata is incomplete."
            QT_TRID_NOOP("gamehq.error.update.install.metadata_incomplete"),
            "The verified update metadata is incomplete.");
        return false;
    }
    const QString version = verified.version;
    const QByteArray sha256 = verified.packageSha256;
    const QString root = QDir::cleanPath(QFileInfo(packageRoot).absoluteFilePath());
    const QString package = QDir::cleanPath(QFileInfo(verified.packagePath).absoluteFilePath());
    const QString downloads = QDir(root).filePath(QStringLiteral(".update/downloads"));
    const auto insideDownloads = [&downloads](const QString &candidate) {
        const QString relative = QDir(downloads).relativeFilePath(
            QDir::cleanPath(QFileInfo(candidate).absoluteFilePath()));
        return relative != QStringLiteral("..") && !relative.startsWith(QStringLiteral("../"));
    };
    // The manifest and signature travel with the package so the helper can
    // repeat the whole verification without trusting anything this process
    // wrote into the transaction.
    const QString manifest = QDir::cleanPath(QFileInfo(verified.manifestPath).absoluteFilePath());
    const QString signature = QDir::cleanPath(QFileInfo(verified.signaturePath).absoluteFilePath());
    if (!insideDownloads(package) || !insideDownloads(manifest) || !insideDownloads(signature)) {
        error = NativeText::get(
            //: Update installer handoff validation failure.
            //% "The verified update package is outside GameHQ's staging directory."
            QT_TRID_NOOP("gamehq.error.update.install.package_outside_staging"),
            "The verified update package is outside GameHQ's staging directory.");
        return false;
    }
    if (!QRegularExpression(QStringLiteral(R"(^\d+\.\d+\.\d+$)")).match(version).hasMatch()
        || sha256.size() != QCryptographicHash::hashLength(QCryptographicHash::Sha256)) {
        error = NativeText::get(
            //: Update installer handoff validation failure.
            //% "The verified update metadata is invalid."
            QT_TRID_NOOP("gamehq.error.update.install.metadata_invalid"),
            "The verified update metadata is invalid.");
        return false;
    }
    if (QFileInfo(package).fileName() != verified.artifactName
        || QFileInfo(package).size() != verified.artifactSize) {
        error = NativeText::get(
            //: Update installer handoff integrity failure.
            //% "The staged package no longer matches the signed manifest."
            QT_TRID_NOOP("gamehq.error.update.install.manifest_mismatch"),
            "The staged package no longer matches the signed manifest.");
        return false;
    }
    QByteArray actual;
    QString verifyError;
    if (!UpdateDownloader::verifyFile(package, sha256, actual, verifyError)) {
        error = NativeText::get(
            //: Update installer handoff integrity failure; %1 is the unchanged verification detail.
            //% "The update package changed before installation: %1"
            QT_TRID_NOOP("gamehq.error.update.install.package_changed"),
            "The update package changed before installation: %1").arg(verifyError);
        return false;
    }
    const QString update = QDir(root).filePath(QStringLiteral(".update"));
    if (!QDir().mkpath(update)) {
        error = NativeText::get(
            //: Update installer handoff filesystem failure.
            //% "GameHQ could not create the update transaction directory."
            QT_TRID_NOOP("gamehq.error.update.install.transaction_directory_failed"),
            "GameHQ could not create the update transaction directory.");
        return false;
    }
    // Pin the transaction to this exact process, not just to its id: Windows
    // reuses process ids, so the helper needs the creation time to prove it is
    // waiting on the application that actually authorised the update.
    FILETIME created{};
    FILETIME exited{};
    FILETIME kernel{};
    FILETIME user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        error = NativeText::get(
            //: Update installer handoff process validation failure.
            //% "GameHQ could not identify its own process for the update."
            QT_TRID_NOOP("gamehq.error.update.install.process_identity_failed"),
            "GameHQ could not identify its own process for the update.");
        return false;
    }
    const quint64 creationTime = (static_cast<quint64>(created.dwHighDateTime) << 32)
        | created.dwLowDateTime;
    if (creationTime == 0 || creationTime > static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        error = NativeText::get(
            //% "GameHQ could not identify its own process for the update."
            QT_TRID_NOOP("gamehq.error.update.install.process_identity_failed"),
            "GameHQ could not identify its own process for the update.");
        return false;
    }

    transactionPath = QDir(update).filePath(QStringLiteral("transaction.json"));
    const QJsonObject object {
        { QStringLiteral("schemaVersion"), 2 },
        { QStringLiteral("productId"), QStringLiteral("underfusion.gamehq") },
        { QStringLiteral("expectedVersion"), version },
        { QStringLiteral("expectedSha256"), QString::fromLatin1(sha256.toHex()) },
        { QStringLiteral("packageRoot"), QDir::toNativeSeparators(root) },
        { QStringLiteral("packagePath"), QDir::toNativeSeparators(package) },
        { QStringLiteral("stagingDir"), QDir::toNativeSeparators(QDir(update).filePath(QStringLiteral("staging"))) },
        { QStringLiteral("backupDir"), QDir::toNativeSeparators(QDir(update).filePath(QStringLiteral("backup"))) },
        { QStringLiteral("restartExecutable"), QDir::toNativeSeparators(QDir(root).filePath(QStringLiteral("GameHQ.exe"))) },
        { QStringLiteral("healthTokenPath"), QDir::toNativeSeparators(QDir(update).filePath(QStringLiteral("healthy.token"))) },
        { QStringLiteral("dataDir"), QDir::toNativeSeparators(QFileInfo(dataDir).absoluteFilePath()) },
        { QStringLiteral("dataSnapshotDir"), QDir::toNativeSeparators(QDir(update).filePath(QStringLiteral("data-snapshot"))) },
        { QStringLiteral("callerPid"), static_cast<qint64>(QCoreApplication::applicationPid()) },
        { QStringLiteral("callerCreationTime"), static_cast<qint64>(creationTime) },
        { QStringLiteral("manifestPath"), QDir::toNativeSeparators(manifest) },
        { QStringLiteral("signaturePath"), QDir::toNativeSeparators(signature) },
        { QStringLiteral("manifestSha256"), verified.manifestSha256 },
        { QStringLiteral("releaseSignature"), verified.signature },
        { QStringLiteral("releaseKeyId"), verified.keyId },
        { QStringLiteral("releaseSequence"), static_cast<qint64>(verified.releaseSequence) },
        { QStringLiteral("artifactName"), verified.artifactName },
        { QStringLiteral("artifactSize"), verified.artifactSize },
        { QStringLiteral("artifactSha256"), verified.artifactSha256 },
        { QStringLiteral("phase"), QStringLiteral("download_verified") }
    };
    QSaveFile output(transactionPath);
    const QByteArray json = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(json) != json.size() || !output.commit()) {
        error = NativeText::get(
            //: Update installer handoff filesystem failure.
            //% "GameHQ could not publish the update transaction."
            QT_TRID_NOOP("gamehq.error.update.install.transaction_publish_failed"),
            "GameHQ could not publish the update transaction.");
        return false;
    }
    return true;
}

bool launchPrepared(const QString &packageRoot, const QString &transactionPath,
                    QString &error)
{
    const QString root = QFileInfo(packageRoot).absoluteFilePath();
    const QString helper = QDir(root).filePath(QStringLiteral("GameHQUpdater.exe"));
    if (!QFileInfo(helper).isFile() || !QFileInfo(helper).isExecutable()) {
        error = NativeText::get(
            //: Update installer handoff executable failure; %1 is the unchanged executable name.
            //% "%1 is missing or cannot run."
            QT_TRID_NOOP("gamehq.error.update.install.helper_missing"),
            "%1 is missing or cannot run.").arg(QStringLiteral("GameHQUpdater.exe"));
        return false;
    }
    // Create the READY event before the helper starts so its SetEvent can
    // never race ahead of us; only quit the app once the helper has validated
    // the transaction (docs/updater.md "Handoff").
    const std::wstring readyName =
        handshake::readyEventNameFor(transactionPath.toStdWString());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
    if (!ready) {
        error = NativeText::get(
            //: Update installer handoff process failure.
            //% "GameHQ could not prepare the updater handshake."
            QT_TRID_NOOP("gamehq.error.update.install.handshake_failed"),
            "GameHQ could not prepare the updater handshake.");
        return false;
    }
    qint64 processId = 0;
    if (!QProcess::startDetached(helper,
                                 { QStringLiteral("--apply"), transactionPath },
                                 root, &processId) || processId <= 0) {
        CloseHandle(ready);
        error = NativeText::get(
            //: Update installer handoff process failure.
            //% "GameHQ could not start the updater helper."
            QT_TRID_NOOP("gamehq.error.update.install.helper_start_failed"),
            "GameHQ could not start the updater helper.");
        return false;
    }
    HANDLE helperProcess = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(processId));
    HANDLE waitees[2] = { ready, helperProcess };
    const DWORD count = helperProcess ? 2 : 1;
    const DWORD result = WaitForMultipleObjects(count, waitees, FALSE, 15000);
    if (helperProcess)
        CloseHandle(helperProcess);
    CloseHandle(ready);
    if (result == WAIT_OBJECT_0)
        return true;
    error = result == WAIT_OBJECT_0 + 1
        ? NativeText::get(
              //: Update installer handoff process failure.
              //% "The updater helper rejected the update before it became ready."
              QT_TRID_NOOP("gamehq.error.update.install.helper_rejected"),
              "The updater helper rejected the update before it became ready.")
        : NativeText::get(
              //: Update installer handoff timeout.
              //% "The updater helper did not confirm it is ready in time."
              QT_TRID_NOOP("gamehq.error.update.install.helper_timeout"),
              "The updater helper did not confirm it is ready in time.");
    return false;
}
}
