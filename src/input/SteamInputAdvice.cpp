#include "input/SteamInputAdvice.h"

#include "input/ActionCatalog.h"
#include "input/ControlId.h"

#include <QRegularExpression>

QStringList SteamInputAdvice::advisedControls()
{
    return { ControlId::Capture, ControlId::Guide };
}

QStringList SteamInputAdvice::boundControls(const QVector<BindingResolver::Binding>& table)
{
    const QStringList advised = advisedControls();
    QStringList bound;
    for (const BindingResolver::Binding& binding : table) {
        if (binding.unbound)
            continue;
        const ActionCatalog::Action* action = ActionCatalog::find(binding.actionId);
        if (!action || action->scope != ActionCatalog::Scope::Global)
            continue;
        for (const QString& control : binding.trigger().controls) {
            if (advised.contains(control) && !bound.contains(control))
                bound << control;
        }
    }
    // Stable display order, whatever order the table stores rows in.
    QStringList ordered;
    for (const QString& control : advised) {
        if (bound.contains(control))
            ordered << control;
    }
    return ordered;
}

QString SteamInputAdvice::serializeAck(const QStringList& controls)
{
    QStringList sorted = controls;
    sorted.removeDuplicates();
    sorted.sort();
    return sorted.join(QLatin1Char(','));
}

QStringList SteamInputAdvice::parseAck(const QString& stored)
{
    return stored.split(QLatin1Char(','), Qt::SkipEmptyParts);
}

QStringList SteamInputAdvice::pendingControls(const QStringList& bound, const QString& storedAck)
{
    if (storedAck.trimmed() == QLatin1String(kDismissedAll))
        return {};
    const QStringList acked = parseAck(storedAck);
    QStringList pending;
    for (const QString& control : bound) {
        if (!acked.contains(control))
            pending << control;
    }
    return pending;
}

QString SteamInputAdvice::gameLayoutUrl(const QString& appId)
{
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    if (!digits.match(appId).hasMatch())
        return {};
    return QStringLiteral("steam://controllerconfig/") + appId;
}

QString SteamInputAdvice::controllerSettingsUrl()
{
    return QStringLiteral("steam://settings/controller");
}
