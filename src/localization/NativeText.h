#pragma once

#include <QCoreApplication>
#include <QString>

namespace NativeText
{
// Resolve a stable translation ID while retaining an English last-resort
// fallback for failures that can occur before the source catalog is available.
inline QString get(const char *id, const char *englishFallback)
{
    const QString translated = qtTrId(id);
    return translated == QString::fromLatin1(id)
        ? QString::fromUtf8(englishFallback)
        : translated;
}
}
