#include "app/App.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/Paths.h"
#include "share/ShareService.h"
#include "share/TelegramWiring.h"
#include "share/providers/ClipboardShareProvider.h"
#include "share/external/ExternalProviderHost.h"
#include "share/providers/DiscordDesktopProvider.h"
#include "share/providers/DiscordWebhookProvider.h"
#include "share/providers/TelegramDesktopProvider.h"

#include <QCoreApplication>
#include <QQmlContext>

// Part of App::init(): the Share service and its providers.
void App::wireShare()
{
    // One Share flow for gallery, lightbox and overlay; providers register
    // into its registry and never need their own QML.
    m_share = std::make_unique<share::Service>();
    m_share->setConfig(m_config.get());
    // Registration order is the order the Share dialog lists destinations.
    // Telegram Desktop always; Telegram Integrated only in a developer build
    // (compile-time gate, see share/TelegramWiring.h).
    m_telegramWiring = std::make_unique<share::TelegramWiring>(share::registerTelegramProviders(
        m_share.get(), QCoreApplication::applicationDirPath(), Paths::dataDir()));
    m_engine.rootContext()->setContextProperty(
        QStringLiteral("telegramAccount"), m_telegramWiring->accountObject);
    m_share->registry()->add(new share::DiscordDesktopProvider(
        &share::DiscordDesktopProvider::locateInstalled,
        &share::DiscordDesktopProvider::startDetached,
        &share::ClipboardShareProvider::copyCapture, m_share.get()));
    m_share->registry()->add(new share::DiscordWebhookProvider(
        std::make_unique<share::DiscordWebhookStore>(
            Paths::dataDir() + QStringLiteral("/share/discord-webhooks.json"),
            share::SecretStore(), share::DiscordWebhookStore::UrlValidator()),
        m_share.get()));
    m_share->registry()->add(new share::ClipboardShareProvider(m_share.get()));
    // Third-party providers are opt-in: with the switch off nothing listens.
    if (m_config->value(ConfigKeys::ShareExternalProviders, false).toBool()) {
        m_shareProviders = std::make_unique<share::external::ExternalProviderHost>(m_share->registry());
        m_shareProviders->setAppVersion(QStringLiteral(GAMEHQ_VERSION));
        QString providerHostError;
        if (!m_shareProviders->start(providerHostError)) {
            qWarning() << "Share provider host could not start:" << providerHostError;
            m_shareProviders.reset();
        }
    }
}
