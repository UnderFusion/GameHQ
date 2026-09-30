#include "core/GamePins.h"

#include "core/GameIdentity.h"

#include <QSet>

namespace GamePins {

QString key(int gameId, const QString& executablePath)
{
    if (!executablePath.trimmed().isEmpty())
        return GameIdentity::executableKey(executablePath);
    return QStringLiteral("id:%1").arg(gameId);
}

QList<int> pinnedFirst(const QStringList& rowKeys, const QStringList& pinned)
{
    const QSet<QString> pins(pinned.cbegin(), pinned.cend());
    QList<int> head;
    QList<int> tail;
    for (int i = 0; i < rowKeys.size(); ++i)
        (pins.contains(rowKeys.at(i)) ? head : tail).append(i);
    return head + tail;
}

QStringList withPin(const QStringList& pinned, const QString& key, bool pin)
{
    QStringList out = pinned;
    out.removeAll(key);
    if (pin && !key.isEmpty())
        out.append(key);
    return out;
}

}
