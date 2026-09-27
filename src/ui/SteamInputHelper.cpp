#include "ui/SteamInputHelper.h"

#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "games/SteamAppLookup.h"
#include "input/ControlId.h"
#include "input/SteamInputAdvice.h"
#include "localization/NativeText.h"

#include <QDebug>

SteamInputHelper::SteamInputHelper(ConfigManager* config, Providers providers, QObject* parent)
    : QObject(parent)
    , m_config(config)
    , m_providers(std::move(providers))
{
    if (m_config) {
        connect(m_config, &ConfigManager::valueChanged, this, [this](const QString& key) {
            if (key == ConfigKeys::InputSteamConflictHelp)
                refresh();
        });
    }
}

bool SteamInputHelper::enabled() const
{
    return !m_config || m_config->value(ConfigKeys::InputSteamConflictHelp, true).toBool();
}

bool SteamInputHelper::createShareBound() const
{
    return m_bound.contains(ControlId::Capture);
}

bool SteamInputHelper::guideBound() const
{
    return m_bound.contains(ControlId::Guide);
}

QString SteamInputHelper::ackKey(const QString& appId)
{
    return QString(ConfigKeys::InputSteamConflictAckPrefix) + appId;
}

QString SteamInputHelper::controlLabel(const QString& control)
{
    if (control == ControlId::Guide) {
        return NativeText::get(
            //: Controller button name in the Steam Input conflict help.
            //% "PS / Guide button"
            QT_TRID_NOOP("gamehq.steam_input.control.guide"),
            "PS / Guide button");
    }
    return NativeText::get(
        //: Controller button name in the Steam Input conflict help.
        //% "Create / Share button"
        QT_TRID_NOOP("gamehq.steam_input.control.capture"),
        "Create / Share button");
}

void SteamInputHelper::refresh()
{
    const QString key = m_providers.gameExecutable ? m_providers.gameExecutable() : QString();
    if (key != m_gameKey) {
        m_gameKey = key;
        // Reads only the Steam library's install manifest for this executable,
        // never Steam's controller configuration.
        const SteamApp app = SteamAppLookup::forExecutable(key);
        m_appId = app.appId;
        if (app.isValid())
            qInfo().noquote() << "Steam Input help: session game is Steam app" << app.appId;
    }
    m_gameName = m_appId.isEmpty() || !m_providers.gameName ? QString() : m_providers.gameName();

    QStringList bound;
    if (!m_appId.isEmpty() && m_providers.boundControls)
        bound = m_providers.boundControls();
    const QString ack = m_appId.isEmpty() || !m_config
        ? QString() : m_config->value(ackKey(m_appId)).toString();
    m_bound = bound;
    m_pending = SteamInputAdvice::pendingControls(bound, ack);
    emit changed();
    maybeNotify();
}

void SteamInputHelper::maybeNotify()
{
    if (!enabled() || m_pending.isEmpty() || !m_providers.notify)
        return;
    const QString token = m_appId + QLatin1Char('|') + m_pending.join(QLatin1Char(','));
    if (m_notifiedThisRun.contains(token))
        return;
    m_notifiedThisRun.insert(token);

    QStringList names;
    for (const QString& control : m_pending)
        names << controlLabel(control);
    const QString title = NativeText::get(
        //: Notification title. Deliberately tentative: GameHQ cannot see Steam's settings.
        //% "Steam Input may also use this button"
        QT_TRID_NOOP("gamehq.steam_input.notice.title"),
        "Steam Input may also use this button");
    const QString body = NativeText::get(
        //: Notification body. %1 = game name, %2 = one or two button names.
        //% "%1: Steam Input may also send %2 to the game. Review it in Settings › Input › Steam Input."
        QT_TRID_NOOP("gamehq.steam_input.notice.body"),
        "%1: Steam Input may also send %2 to the game. Review it in Settings › Input › Steam Input.")
        .arg(m_gameName, names.join(QStringLiteral(", ")));
    m_providers.notify(title, body);
}

bool SteamInputHelper::openGameLayout()
{
    const QString url = SteamInputAdvice::gameLayoutUrl(m_appId);
    if (url.isEmpty() || !m_providers.openUrl)
        return false;
    qInfo().noquote() << "Steam Input help: user opened the Steam layout for app" << m_appId;
    return m_providers.openUrl(url);
}

bool SteamInputHelper::openControllerSettings()
{
    if (!m_providers.openUrl)
        return false;
    qInfo() << "Steam Input help: user opened Steam controller settings";
    return m_providers.openUrl(SteamInputAdvice::controllerSettingsUrl());
}

void SteamInputHelper::markReviewed()
{
    if (m_appId.isEmpty() || !m_config)
        return;
    const QString stored = m_config->value(ackKey(m_appId)).toString();
    if (stored == QLatin1String(SteamInputAdvice::kDismissedAll))
        return;
    QStringList acked = SteamInputAdvice::parseAck(stored);
    acked += m_bound;
    m_config->setValue(ackKey(m_appId), SteamInputAdvice::serializeAck(acked));
    m_config->save();
    refresh();
}

void SteamInputHelper::hideForGame()
{
    if (m_appId.isEmpty() || !m_config)
        return;
    m_config->setValue(ackKey(m_appId), QString::fromLatin1(SteamInputAdvice::kDismissedAll));
    m_config->save();
    refresh();
}
