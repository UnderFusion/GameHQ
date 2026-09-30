#pragma once
#include <QList>
#include <QString>
#include <QStringList>

// Pinned games in the sidebar. A pin is stored as a stable key (the canonical
// executable, or "id:<n>" for a game GameHQ never saw running) so it survives
// renames and a library rescan. Ordering keeps the database's most-recent-first
// order inside each group: pinned games first, then the rest.
namespace GamePins {

QString key(int gameId, const QString& executablePath);

// Row indices of `rowKeys` reordered so every pinned row comes first; the
// relative order inside the pinned and unpinned groups is unchanged.
QList<int> pinnedFirst(const QStringList& rowKeys, const QStringList& pinned);

// `pinned` with `key` added (to the end) or removed; never duplicates a key.
QStringList withPin(const QStringList& pinned, const QString& key, bool pin);

}
