#pragma once

#include "share/ShareProvider.h"

#include <QStringList>

#include <functional>

namespace share
{

// Discord Desktop hand-off (plan t14). Privacy-first: GameHQ never signs in to
// Discord and uses no bot, webhook or user token. Discord has no documented
// "send this file" launch option, so the reliable path is: put the chosen
// capture on the clipboard, then start/focus the installed Discord, where the
// user pastes it into a chat. The outcome is `handed_off` (result detail
// "paste") when Discord was opened, `copied` when it could not be, and never
// `sent`.
class DiscordDesktopProvider : public Provider
{
    Q_OBJECT
public:
    struct Installation
    {
        QString executable;      // Update.exe (stock installer) or Discord.exe
        QStringList launchArgs;  // e.g. {"--processStart", "Discord.exe"}
        bool isValid() const { return !executable.isEmpty(); }
    };
    using Locator = std::function<Installation()>;
    // Starts a detached process; returns false if it could not start.
    using Launcher = std::function<bool(const QString& program, const QStringList& args)>;
    // Puts the capture on the clipboard; returns false if that failed.
    using ClipboardWriter = std::function<bool(const Request& request)>;

    DiscordDesktopProvider(Locator locator, Launcher launcher, ClipboardWriter clipboard,
                           QObject* parent = nullptr);

    QString id() const override { return QStringLiteral("discord.desktop"); }
    QString displayName() const override { return QStringLiteral("Discord"); }
    QString iconSource() const override { return QStringLiteral(""); }   // Segoe Fluent: Chat
    Capabilities capabilities() const override
    {
        return Capability::Image | Capability::Video | Capability::ExternalHandoff;
    }
    Availability availability() const override;
    QString availabilityReason() const override;
    QString privacyNotice() const override;
    int jobTimeoutMs() const override { return 15000; }

    void requestTargets(const QString& queryId, const Request& request,
                        const QString& query) override;
    void start(const Job& job, const Request& request, const Target& target) override;

    // Parses the registered `discord` URL handler command line: the launcher
    // plus the "--processStart <exe>" pair that starts (or focuses) Discord.
    static Installation parseHandlerCommand(const QString& command);
    // Production locator: HKCU/HKLM `discord` handler, then the default
    // per-user install folder. Only accepts an existing Discord launcher.
    static Installation locateInstalled();
    static bool startDetached(const QString& program, const QStringList& args);

private:
    Installation installation() const;

    Locator m_locator;
    Launcher m_launcher;
    ClipboardWriter m_clipboard;
};

} // namespace share
