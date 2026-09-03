#include "app/PackagedLocalizationProbe.h"

#include "app/ReleaseNotes.h"
#include "localization/LocaleRegistry.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTranslator>
#include <QVariantMap>

namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

const QStringList &productionLocales()
{
    static const QStringList locales{
        QStringLiteral("en-US"), QStringLiteral("zh-Hans"), QStringLiteral("ru-RU"),
        QStringLiteral("es-ES"), QStringLiteral("pt-BR"), QStringLiteral("de-DE"),
        QStringLiteral("ja-JP"), QStringLiteral("fr-FR"), QStringLiteral("pl-PL"),
        QStringLiteral("ko-KR"), QStringLiteral("zh-Hant"), QStringLiteral("tr-TR"),
        QStringLiteral("th-TH"), QStringLiteral("es-419"), QStringLiteral("uk-UA"),
        QStringLiteral("it-IT")};
    return locales;
}

bool inspectBundleMetadata(const QString &locale, int *fallbackDocuments, QString *error)
{
    QFile file(QStringLiteral(":/release-notes/generated/release-notes.%1.json").arg(locale));
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, QStringLiteral("Missing embedded release-note bundle for %1.").arg(locale));

    QJsonParseError parseError{};
    const QJsonDocument parsed = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !parsed.isObject())
        return fail(error, QStringLiteral("Invalid embedded release-note JSON for %1.").arg(locale));
    const QJsonObject meta = parsed.object().value(QStringLiteral("_meta")).toObject();
    const QJsonArray documents = meta.value(QStringLiteral("documents")).toArray();
    if (meta.value(QStringLiteral("requested_locale")).toString() != locale
        || meta.value(QStringLiteral("source_locale")).toString() != QStringLiteral("en-US")
        || documents.isEmpty()) {
        return fail(error, QStringLiteral("Release-note presentation metadata mismatch for %1.")
                               .arg(locale));
    }

    int fallbacks = 0;
    for (const QJsonValue &value : documents) {
        const QJsonObject document = value.toObject();
        const bool fallback = document.value(QStringLiteral("fallback")).toBool();
        const QString resolved = document.value(QStringLiteral("resolved_locale")).toString();
        if (fallback) {
            ++fallbacks;
            if (resolved != QStringLiteral("en-US"))
                return fail(error, QStringLiteral("Non-English fallback metadata for %1.").arg(locale));
        } else if (resolved != locale) {
            return fail(error, QStringLiteral("Mixed-document locale metadata for %1.").arg(locale));
        }
    }
    *fallbackDocuments = fallbacks;
    return true;
}

} // namespace

bool PackagedLocalizationProbe::run(const QString &reportPath, QString *error)
{
    LocaleRegistry registry(false);
    QString registryError;
    if (!registry.load(QStringLiteral(":/i18n/locales.json"), &registryError))
        return fail(error, QStringLiteral("Embedded locale manifest failed: %1").arg(registryError));

    const QVariantList available = registry.availableLanguages();
    if (available.size() != productionLocales().size())
        return fail(error, QStringLiteral("Expected exactly sixteen packaged production locales."));

    QStringList actual;
    QJsonArray localeEvidence;
    const QSet<QString> representatives{
        QStringLiteral("en-US"), QStringLiteral("pl-PL"), QStringLiteral("zh-Hant"),
        QStringLiteral("th-TH")};
    for (const QVariant &value : available)
        actual.append(value.toMap().value(QStringLiteral("tag")).toString());
    if (actual != productionLocales())
        return fail(error, QStringLiteral("Packaged production-locale order or membership differs."));

    for (const QString &locale : productionLocales()) {
        const QString catalog = registry.catalogName(locale);
        const QString catalogPath = QStringLiteral(":/i18n/%1.qm").arg(catalog);
        QFile catalogFile(catalogPath);
        if (catalog.isEmpty() || !catalogFile.open(QIODevice::ReadOnly) || catalogFile.size() <= 0)
            return fail(error, QStringLiteral("Missing embedded Qt catalog for %1: %2")
                                   .arg(locale, catalogPath));
        const qint64 catalogBytes = catalogFile.size();
        catalogFile.close();
        QTranslator translator;
        if (!translator.load(catalogPath))
            return fail(error, QStringLiteral("Invalid embedded Qt catalog for %1.").arg(locale));

        QString notesError;
        const ReleaseNotes notes = ReleaseNotes::loadBundled(locale, registry, &notesError);
        if (!notes.isValid() || notes.locale() != locale || notes.releases().isEmpty())
            return fail(error, QStringLiteral("Embedded release notes failed for %1: %2")
                                   .arg(locale, notesError));
        int fallbackDocuments = 0;
        if (!inspectBundleMetadata(locale, &fallbackDocuments, error))
            return false;

        localeEvidence.append(QJsonObject{
            {QStringLiteral("locale"), locale},
            {QStringLiteral("catalog"), catalog},
            {QStringLiteral("catalog_bytes"), catalogBytes},
            {QStringLiteral("release_count"), notes.releases().size()},
            {QStringLiteral("fallback_documents"), fallbackDocuments},
            {QStringLiteral("representative_smoke"), representatives.contains(locale)}});
    }

    for (const QString &forbidden : {QStringLiteral("gamehq_en_XA"),
                                     QStringLiteral("gamehq_ar_XB"),
                                     QStringLiteral("gamehq_cs_CZ")}) {
        if (QFile::exists(QStringLiteral(":/i18n/%1.qm").arg(forbidden)))
            return fail(error, QStringLiteral("Development/reserve catalog was packaged: %1").arg(forbidden));
    }
    for (const QString &forbidden : {QStringLiteral("en-XA"), QStringLiteral("ar-XB"),
                                     QStringLiteral("cs-CZ")}) {
        if (QFile::exists(QStringLiteral(":/release-notes/generated/release-notes.%1.json")
                              .arg(forbidden))) {
            return fail(error, QStringLiteral("Development/reserve release notes were packaged: %1")
                                   .arg(forbidden));
        }
    }

    const QJsonObject report{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("production_locale_count"), productionLocales().size()},
        {QStringLiteral("catalog_count"), productionLocales().size()},
        {QStringLiteral("release_note_bundle_count"), productionLocales().size()},
        {QStringLiteral("source_locale"), registry.sourceLanguage()},
        {QStringLiteral("update_authorization_input"), false},
        {QStringLiteral("locales"), localeEvidence}};
    QSaveFile output(reportPath);
    if (!output.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("Cannot create localization report: %1")
                               .arg(output.errorString()));
    output.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
    if (!output.commit())
        return fail(error, QStringLiteral("Cannot commit localization report: %1")
                               .arg(output.errorString()));
    if (error)
        error->clear();
    return true;
}
