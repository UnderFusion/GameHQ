#include "share/providers/TelegramDesktopProvider.h"

#include "localization/NativeText.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QDebug>

#include <windows.h>
#include <shellapi.h>

namespace share
{

namespace
{
QStringList splitCommandLine(const QString& command)
{
    QStringList out;
    // CommandLineToArgvW("") answers with this process's own path.
    if (command.trimmed().isEmpty())
        return out;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(reinterpret_cast<LPCWSTR>(command.utf16()), &argc);
    if (!argv)
        return out;
    for (int i = 0; i < argc; ++i)
        out.append(QString::fromWCharArray(argv[i]));
    LocalFree(argv);
    return out;
}

bool isTelegramExecutable(const QString& path)
{
    const QFileInfo info(path);
    return info.isFile()
        && info.fileName().compare(QLatin1String("Telegram.exe"), Qt::CaseInsensitive) == 0;
}
} // namespace

TelegramDesktopProvider::TelegramDesktopProvider(QObject* parent)
    : TelegramDesktopProvider(&TelegramDesktopProvider::locateInstalled,
                              [](const QString& program, const QStringList& args) {
                                  return QProcess::startDetached(program, args);
                              },
                              parent)
{
}

TelegramDesktopProvider::TelegramDesktopProvider(Locator locator, Launcher launcher, QObject* parent)
    : Provider(parent)
    , m_locator(std::move(locator))
    , m_launcher(std::move(launcher))
{
}

TelegramDesktopProvider::Installation TelegramDesktopProvider::parseHandlerCommand(const QString& command)
{
    Installation out;
    const QStringList args = splitCommandLine(command.trimmed());
    if (args.isEmpty())
        return out;
    out.executable = QDir::cleanPath(QDir::fromNativeSeparators(args.first()));
    // Keep only the profile selector: it decides which Telegram account data
    // (and running instance) receives the file. Everything after "--" is the
    // URL placeholder, which a hand-off must not pass on.
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--"))
            break;
        if (args.at(i) == QLatin1String("-workdir") && i + 1 < args.size()) {
            out.profileArgs << args.at(i) << args.at(i + 1);
            ++i;
        }
    }
    return out;
}

TelegramDesktopProvider::Installation TelegramDesktopProvider::locateInstalled()
{
    const QStringList roots = {
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\tg\\shell\\open\\command"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Classes\\tg\\shell\\open\\command"),
    };
    for (const QString& key : roots) {
        const QSettings reg(key, QSettings::NativeFormat);
        const QString command = reg.value(QStringLiteral("Default")).toString();
        if (command.isEmpty())
            continue;
        Installation found = parseHandlerCommand(command);
        if (isTelegramExecutable(found.executable))
            return found;
    }
    // Default per-user installer location (%APPDATA%\Telegram Desktop).
    const QString appData = QDir::fromNativeSeparators(qEnvironmentVariable("APPDATA"));
    if (!appData.isEmpty()) {
        const QString exe = appData + QStringLiteral("/Telegram Desktop/Telegram.exe");
        if (isTelegramExecutable(exe))
            return { exe, {} };
    }
    return {};
}

TelegramDesktopProvider::Installation TelegramDesktopProvider::installation() const
{
    return m_locator ? m_locator() : Installation();
}

Availability TelegramDesktopProvider::availability() const
{
    return installation().isValid() ? Availability::Available : Availability::NotInstalled;
}

QString TelegramDesktopProvider::availabilityReason() const
{
    if (availability() == Availability::Available)
        return {};
    return NativeText::get(
        //: Share destination is disabled because the Telegram desktop app was not found.
        //% "Telegram Desktop isn't installed."
        QT_TRID_NOOP("gamehq.share.telegram_desktop.not_installed"),
        "Telegram Desktop isn't installed.");
}

QString TelegramDesktopProvider::privacyNotice() const
{
    return NativeText::get(
        //: Shown under the Telegram share destination. GameHQ only opens Telegram with the file.
        //% "Opens Telegram to choose the chat. GameHQ never signs in to Telegram."
        QT_TRID_NOOP("gamehq.share.telegram_desktop.privacy"),
        "Opens Telegram to choose the chat. GameHQ never signs in to Telegram.");
}

void TelegramDesktopProvider::requestTargets(const QString& queryId, const Request& request,
                                             const QString& query)
{
    Q_UNUSED(request);
    Q_UNUSED(query);
    // Telegram owns recipient selection; the one "target" is the app itself.
    Target t;
    t.id = QStringLiteral("telegram-desktop");
    t.kind = TargetKind::External;
    t.displayName = displayName();
    emit targetsReady(queryId, { t }, {});
}

void TelegramDesktopProvider::start(const Job& job, const Request& request, const Target& target)
{
    Q_UNUSED(target);
    Result r;
    r.jobId = job.id;
    const Installation install = installation();
    if (!install.isValid()) {
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("not_installed");
        emit jobFinished(r);
        return;
    }
    // Let an already-running Telegram bring its "choose a chat" box to the
    // front: Windows only lets the foreground process grant that.
    AllowSetForegroundWindow(ASFW_ANY);
    QStringList args = install.profileArgs;
    args << QStringLiteral("-sendpath") << QDir::toNativeSeparators(request.filePath());
    const bool started = m_launcher && m_launcher(QDir::toNativeSeparators(install.executable), args);
    if (!started) {
        qWarning() << "Share: Telegram Desktop could not be started";
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("launch_failed");
    } else {
        qInfo() << "Share: handed the capture to Telegram Desktop";
        r.outcome = Outcome::HandedOff;
    }
    emit jobFinished(r);
}

} // namespace share
