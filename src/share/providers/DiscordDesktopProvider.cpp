#include "share/providers/DiscordDesktopProvider.h"

#include "localization/NativeText.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSettings>
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

// Stable, PTB and Canary use the "Update.exe" launcher; newer builds may
// register the app executable (Discord*.exe) directly.
bool isDiscordLauncher(const QString& path)
{
    const QFileInfo info(path);
    if (!info.isFile())
        return false;
    const QString name = info.fileName();
    return name.compare(QLatin1String("Update.exe"), Qt::CaseInsensitive) == 0
        || name.startsWith(QLatin1String("Discord"), Qt::CaseInsensitive);
}
} // namespace

DiscordDesktopProvider::DiscordDesktopProvider(Locator locator, Launcher launcher,
                                               ClipboardWriter clipboard, QObject* parent)
    : Provider(parent)
    , m_locator(std::move(locator))
    , m_launcher(std::move(launcher))
    , m_clipboard(std::move(clipboard))
{
}

DiscordDesktopProvider::Installation DiscordDesktopProvider::parseHandlerCommand(const QString& command)
{
    Installation out;
    const QStringList args = splitCommandLine(command.trimmed());
    if (args.isEmpty())
        return out;
    out.executable = QDir::cleanPath(QDir::fromNativeSeparators(args.first()));
    // Keep only "--processStart <exe>": it names the app the updater starts.
    // The URL placeholder after --process-start-args must not be passed on.
    for (int i = 1; i + 1 < args.size(); ++i) {
        if (args.at(i) == QLatin1String("--processStart")) {
            out.launchArgs << args.at(i) << args.at(i + 1);
            break;
        }
    }
    return out;
}

DiscordDesktopProvider::Installation DiscordDesktopProvider::locateInstalled()
{
    const QStringList roots = {
        QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\discord\\shell\\open\\command"),
        QStringLiteral("HKEY_LOCAL_MACHINE\\Software\\Classes\\discord\\shell\\open\\command"),
    };
    for (const QString& key : roots) {
        const QSettings reg(key, QSettings::NativeFormat);
        const QString command = reg.value(QStringLiteral("Default")).toString();
        if (command.isEmpty())
            continue;
        Installation found = parseHandlerCommand(command);
        if (isDiscordLauncher(found.executable))
            return found;
    }
    // Default per-user installer location (under %LOCALAPPDATA%).
    const QString local = QDir::fromNativeSeparators(qEnvironmentVariable("LOCALAPPDATA"));
    if (!local.isEmpty()) {
        const QString update = local + QStringLiteral("/Discord/Update.exe");
        if (isDiscordLauncher(update))
            return { update, { QStringLiteral("--processStart"), QStringLiteral("Discord.exe") } };
    }
    return {};
}

bool DiscordDesktopProvider::startDetached(const QString& program, const QStringList& args)
{
    // Lets an already-running Discord raise itself: only the foreground
    // process may grant that.
    AllowSetForegroundWindow(ASFW_ANY);
    return QProcess::startDetached(program, args);
}

DiscordDesktopProvider::Installation DiscordDesktopProvider::installation() const
{
    return m_locator ? m_locator() : Installation();
}

Availability DiscordDesktopProvider::availability() const
{
    return installation().isValid() ? Availability::Available : Availability::NotInstalled;
}

QString DiscordDesktopProvider::availabilityReason() const
{
    if (availability() == Availability::Available)
        return {};
    return NativeText::get(
        //: Share destination is disabled because the Discord desktop app was not found.
        //% "Discord Desktop isn't installed."
        QT_TRID_NOOP("gamehq.share.discord_desktop.not_installed"),
        "Discord Desktop isn't installed.");
}

QString DiscordDesktopProvider::privacyNotice() const
{
    return NativeText::get(
        //: Shown under the Discord share destination. GameHQ only copies the file and opens Discord.
        //% "Copies the file and opens Discord so you can paste it. GameHQ never signs in to Discord."
        QT_TRID_NOOP("gamehq.share.discord_desktop.privacy"),
        "Copies the file and opens Discord so you can paste it. GameHQ never signs in to Discord.");
}

void DiscordDesktopProvider::requestTargets(const QString& queryId, const Request& request,
                                            const QString& query)
{
    Q_UNUSED(request);
    Q_UNUSED(query);
    // Discord owns conversation selection; the one "target" is the app itself.
    Target t;
    t.id = QStringLiteral("discord-desktop");
    t.kind = TargetKind::External;
    t.displayName = displayName();
    emit targetsReady(queryId, { t }, {});
}

void DiscordDesktopProvider::start(const Job& job, const Request& request, const Target& target)
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
    // Without the clipboard there is nothing for the user to paste.
    if (!m_clipboard || !m_clipboard(request)) {
        r.outcome = Outcome::Failed;
        r.errorCode = QStringLiteral("clipboard_unavailable");
        emit jobFinished(r);
        return;
    }
    const bool started = m_launcher
        && m_launcher(QDir::toNativeSeparators(install.executable), install.launchArgs);
    if (started) {
        qInfo() << "Share: copied the capture and opened Discord Desktop";
        r.outcome = Outcome::HandedOff;
        r.detail = QStringLiteral("paste");   // stable code: the user must paste
    } else {
        // The file is still on the clipboard, so the user can paste it into
        // an already-open Discord.
        qWarning() << "Share: Discord Desktop could not be started";
        r.outcome = Outcome::Copied;
        r.errorCode = QStringLiteral("launch_failed");
    }
    emit jobFinished(r);
}

} // namespace share
