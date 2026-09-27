#include "games/SteamAppLookup.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>

SteamApp SteamAppLookup::forExecutable(const QString& executablePath)
{
    if (executablePath.isEmpty())
        return {};
    const QString unix = QDir::fromNativeSeparators(executablePath);
    const int commonIdx = unix.indexOf(QStringLiteral("/steamapps/common/"),
                                       0, Qt::CaseInsensitive);
    if (commonIdx < 0)
        return {};

    const QString steamappsDir = unix.left(commonIdx) + QStringLiteral("/steamapps");
    const QString afterCommon = unix.mid(commonIdx + int(qstrlen("/steamapps/common/")));
    const QString installDir = afterCommon.section(QLatin1Char('/'), 0, 0);
    if (installDir.isEmpty())
        return {};

    static const QRegularExpression reFile(QStringLiteral("^appmanifest_(\\d+)\\.acf$"),
                                           QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression reInstall(
        QStringLiteral("\"installdir\"\\s*\"([^\"]*)\""));
    static const QRegularExpression reName(
        QStringLiteral("\"name\"\\s*\"([^\"]*)\""));

    const QStringList manifests =
        QDir(steamappsDir).entryList({ QStringLiteral("appmanifest_*.acf") }, QDir::Files);
    for (const QString& m : manifests) {
        const auto fileMatch = reFile.match(m);
        if (!fileMatch.hasMatch())
            continue;
        QFile f(steamappsDir + QLatin1Char('/') + m);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString acf = QString::fromUtf8(f.readAll());
        const auto mi = reInstall.match(acf);
        if (!mi.hasMatch() || mi.captured(1).compare(installDir, Qt::CaseInsensitive) != 0)
            continue;
        SteamApp app;
        app.appId = fileMatch.captured(1);
        const auto mn = reName.match(acf);
        if (mn.hasMatch())
            app.name = mn.captured(1).trimmed();
        return app;
    }
    return {};
}
