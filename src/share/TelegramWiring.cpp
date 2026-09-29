#include "share/TelegramWiring.h"

#include "share/ShareService.h"
#include "share/providers/TelegramDesktopProvider.h"

#if GAMEHQ_TELEGRAM_INTEGRATED_BUILD
#include "share/ShareSecurity.h"
#include "share/providers/TelegramIntegratedProvider.h"
#include "telegram/TdJsonTransport.h"
#include "telegram/TdRuntime.h"
#include "telegram/TelegramAccount.h"

#include <QLocale>
#endif

namespace share
{

TelegramWiring registerTelegramProviders(Service* service, const QString& applicationDir,
                                         const QString& dataDir)
{
    TelegramWiring wiring;
    service->registry()->add(new TelegramDesktopProvider(service));

#if GAMEHQ_TELEGRAM_INTEGRATED_BUILD
    // Optional TDLib runtime: located now, hashed and loaded only when the
    // user connects.
    std::shared_ptr<telegram::TdRuntime> runtimeOwner = telegram::TdRuntime::forInstalledApp(applicationDir);
    telegram::TdRuntime* runtime = runtimeOwner.get();
    wiring.runtime = runtimeOwner;
    telegram::Account::Config accountConfig{
        SecretStore(),
        dataDir + QStringLiteral("/share/sessions"),
        [runtime]() -> std::unique_ptr<telegram::TdTransport> {
            if (runtime->load() != telegram::TdRuntime::Status::Ready)
                return nullptr;
            return std::make_unique<telegram::TdJsonTransport>(runtime);
        },
        [runtime] { return telegram::TdRuntime::statusCode(runtime->quickStatus()); },
        QStringLiteral(GAMEHQ_VERSION),
        QLocale::system().name().left(2),
        5 * 60 * 1000 };
    auto account = std::make_shared<telegram::Account>(std::move(accountConfig));
    wiring.account = account;
    wiring.accountObject = account.get();
    service->registry()->add(new TelegramIntegratedProvider(account.get(), runtime, service));
#else
    Q_UNUSED(applicationDir);
    Q_UNUSED(dataDir);
#endif
    return wiring;
}

} // namespace share
