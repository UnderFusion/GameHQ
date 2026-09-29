#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace share
{
class Service;

// Compile-time product gate for Telegram Integrated (plan t13/t25). The owner
// deferred it: standard GameHQ builds never register it, never show its
// account UI and never touch the TDLib runtime, whatever a config file says.
// A developer can build it in with -DGAMEHQ_TELEGRAM_INTEGRATED=ON; there is
// deliberately no user setting for it.
#ifndef GAMEHQ_TELEGRAM_INTEGRATED_BUILD
#define GAMEHQ_TELEGRAM_INTEGRATED_BUILD 0
#endif
inline constexpr bool kTelegramIntegratedCompiledIn = GAMEHQ_TELEGRAM_INTEGRATED_BUILD != 0;

// What registerTelegramProviders() created for Telegram Integrated. Everything
// is null in every standard build. Type-erased on purpose so standard builds
// need none of the TDLib code. Keep this alive exactly as long as the Service
// that holds the provider; `account` is declared last so it goes first.
struct TelegramWiring
{
    std::shared_ptr<void> runtime;
    std::shared_ptr<void> account;
    QObject* accountObject = nullptr;   // for QML; null when not built in
};

// Always registers Telegram Desktop (the supported path, independent of
// TDLib). Registers Telegram Integrated only in a developer build.
TelegramWiring registerTelegramProviders(Service* service, const QString& applicationDir,
                                         const QString& dataDir);

} // namespace share
