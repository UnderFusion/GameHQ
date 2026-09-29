#include "app/StartupManager.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>
#include <QStringList>
#include <QDebug>

namespace
{
const QString kRunKey = QStringLiteral(
    "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run");
const QString kValueName = QStringLiteral("GameHQ");
const QStringList kLegacyValueNames = {QStringLiteral("SavePlay"), QStringLiteral("PlayHQ")};
}

QString StartupManager::executablePath()
{
    const QFileInfo realExecutable(QCoreApplication::applicationFilePath());
    const QDir realDir = realExecutable.absoluteDir();
    const QDir packageRoot(realDir.absoluteFilePath(QStringLiteral("..")));
    const QString launcher = packageRoot.absoluteFilePath(QStringLiteral("GameHQ.exe"));

    // Packaged installed and portable layouts both use the root launcher. It
    // owns updater promotion/recovery and must not be bypassed by autostart.
    if (realDir.dirName().compare(QStringLiteral("app"), Qt::CaseInsensitive) == 0
        && QFileInfo(launcher).isFile()) {
        return QDir::cleanPath(launcher);
    }
    return QDir::cleanPath(realExecutable.absoluteFilePath());
}

bool StartupManager::commandTargets(const QString& command, const QString& executable)
{
    QString target = command.trimmed();
    if (target.startsWith(QLatin1Char('"'))) {
        const qsizetype end = target.indexOf(QLatin1Char('"'), 1);
        target = end > 0 ? target.mid(1, end - 1) : target.mid(1);
    } else {
        const qsizetype exe = target.indexOf(QStringLiteral(".exe"), 0, Qt::CaseInsensitive);
        if (exe > 0)
            target = target.left(exe + 4);
    }
    if (target.isEmpty() || executable.isEmpty())
        return false;
    return QDir::cleanPath(QDir::fromNativeSeparators(target))
               .compare(QDir::cleanPath(QDir::fromNativeSeparators(executable)),
                        Qt::CaseInsensitive) == 0;
}

bool StartupManager::syncOnLaunch(bool enabled) const
{
    if (enabled) {
        QSettings runKey(kRunKey, QSettings::NativeFormat);
        const QString existing = runKey.value(kValueName).toString();
        const QString self = executablePath();
        if (!existing.isEmpty() && !commandTargets(existing, self)) {
            const QString other = existing.trimmed().startsWith(QLatin1Char('"'))
                ? existing.trimmed().section(QLatin1Char('"'), 1, 1)
                : existing.trimmed();
            if (QFileInfo(other).isFile()) {
                qInfo() << "Startup: enabled here, but the Run entry launches another"
                        << "GameHQ copy; left unchanged (toggle it in Settings to move it)";
                return true;
            }
        }
    }
    return setEnabled(enabled);
}

bool StartupManager::setEnabled(bool enabled) const
{
    QSettings runKey(kRunKey, QSettings::NativeFormat);
    if (enabled) {
        const QString command = QStringLiteral("\"%1\"")
            .arg(QDir::toNativeSeparators(executablePath()));
        runKey.setValue(kValueName, command);
        for (const QString& legacyName : kLegacyValueNames)
            runKey.remove(legacyName);
    } else {
        const QString existing = runKey.value(kValueName).toString();
        if (!existing.isEmpty() && !commandTargets(existing, executablePath())) {
            // Another GameHQ copy (usually the installed one) owns autostart.
            qInfo() << "Startup: disabled here; the Run entry belongs to another copy"
                    << "and is left unchanged";
            for (const QString& legacyName : kLegacyValueNames)
                runKey.remove(legacyName);
            runKey.sync();
            return runKey.status() == QSettings::NoError;
        }
        runKey.remove(kValueName);
        for (const QString& legacyName : kLegacyValueNames)
            runKey.remove(legacyName);
    }
    runKey.sync();
    if (runKey.status() != QSettings::NoError) {
        qWarning() << "Startup: could not" << (enabled ? "register" : "remove")
                   << "the per-user Run entry";
        return false;
    }
    qInfo() << "Startup:" << (enabled ? "enabled" : "disabled");
    return true;
}
