#pragma once

#include "share/ShareProvider.h"

#include <QStringList>

#include <functional>

namespace share
{

// Telegram Desktop hand-off (plan t12). Privacy-first: GameHQ never signs in
// to Telegram. It starts the installed Telegram Desktop with its documented
// `-sendpath <file>` option, which opens Telegram's own "choose a chat" box
// for exactly the chosen capture; the user picks the recipient and sends in
// Telegram. The outcome is therefore always `handed_off`, never `sent`.
class TelegramDesktopProvider : public Provider
{
    Q_OBJECT
public:
    // Where Telegram Desktop is and how its registered handler starts it.
    struct Installation
    {
        QString executable;         // absolute path to Telegram.exe
        QStringList profileArgs;    // e.g. {"-workdir", "<dir>"} for portable setups
        bool isValid() const { return !executable.isEmpty(); }
    };
    using Locator = std::function<Installation()>;
    // Starts a detached process; returns false if it could not start.
    using Launcher = std::function<bool(const QString& program, const QStringList& args)>;

    explicit TelegramDesktopProvider(QObject* parent = nullptr);
    TelegramDesktopProvider(Locator locator, Launcher launcher, QObject* parent = nullptr);

    QString id() const override { return QStringLiteral("telegram.desktop"); }
    QString displayName() const override { return QStringLiteral("Telegram"); }
    QString iconSource() const override { return QStringLiteral("\uE724"); }   // Segoe Fluent: Send
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

    // Parses a registered `tg` URL handler command line, e.g.
    //   "C:\...\Telegram.exe" -workdir "D:\TG" -- "%1"
    // into the executable and the profile arguments worth keeping.
    static Installation parseHandlerCommand(const QString& command);
    // Production locator: HKCU/HKLM `tg` handler, then the default per-user
    // install folder. Only accepts an existing Telegram.exe.
    static Installation locateInstalled();

private:
    Installation installation() const;

    Locator m_locator;
    Launcher m_launcher;
};

} // namespace share
