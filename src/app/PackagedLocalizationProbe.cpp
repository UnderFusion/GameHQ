#include "app/PackagedLocalizationProbe.h"

#include "app/ReleaseNotes.h"
#include "config/ConfigManager.h"
#include "localization/LanguageManager.h"
#include "localization/LanguagePreference.h"
#include "localization/LocaleRegistry.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryDir>
#include <QTranslator>
#include <QVariantMap>
#include <utility>

namespace {

class ProbeBootstrapStore final : public LanguageBootstrapStore
{
public:
    explicit ProbeBootstrapStore(QString language = {}, bool present = true)
        : m_language(std::move(language)), m_present(present)
    {
    }

    bool hasValue() const override { return m_present; }
    QString value() const override { return m_language; }
    bool removeValue() override
    {
        m_present = false;
        return true;
    }
    bool present() const { return m_present; }

private:
    QString m_language;
    bool m_present = false;
};

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

    QString sourceAbout;
    QString sourceSupport;
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

        QTemporaryDir profile;
        if (!profile.isValid())
            return fail(error, QStringLiteral("Cannot create isolated profile for %1.").arg(locale));
        const QString configPath = profile.filePath(QStringLiteral("config.json"));
        ConfigManager config(configPath);
        if (!config.load())
            return fail(error, QStringLiteral("Cannot initialize isolated profile for %1.").arg(locale));
        ProbeBootstrapStore bootstrap(locale);
        LanguagePreference preference(&config, &registry);
        const QString initialLanguage = preference.initialLanguage(bootstrap, false);
        if (initialLanguage != locale || bootstrap.present())
            return fail(error, QStringLiteral("Installer handoff consumption failed for %1.").arg(locale));

        LanguageManager manager(&registry);
        int qmlRetranslates = 0;
        manager.setQmlRetranslateCallback([&qmlRetranslates] { ++qmlRetranslates; });
        QString languageError;
        if (!manager.initialize(initialLanguage, {locale}, &languageError)
            || manager.requestedLanguage() != locale
            || manager.effectiveLanguage() != locale) {
            return fail(error, QStringLiteral("Packaged runtime language selection failed for %1: %2")
                                   .arg(locale, languageError));
        }
        const QString about = qtTrId("gamehq.navigation.about");
        const QString support = qtTrId("gamehq.navigation.support_gamehq");
        if (about.isEmpty() || support.isEmpty()
            || about.startsWith(QStringLiteral("gamehq."))
            || support.startsWith(QStringLiteral("gamehq."))) {
            return fail(error, QStringLiteral("Raw or missing sidebar footer text for %1.")
                                   .arg(locale));
        }
        if (locale == QStringLiteral("en-US")) {
            sourceAbout = about;
            sourceSupport = support;
        } else if (about == sourceAbout || support == sourceSupport) {
            return fail(error, QStringLiteral("Unexpected English sidebar footer fallback for %1.")
                                   .arg(locale));
        }

        const QDateTime sample(QDate(2026, 9, 3), QTime(14, 5), QTimeZone::UTC);
        const QString renderedDate = manager.formatDate(sample);
        const QString expectedDate = QLocale(locale).toString(
            sample.toLocalTime().date(), QLocale::ShortFormat);
        if (renderedDate != expectedDate)
            return fail(error, QStringLiteral("Locale date formatting failed for %1.").arg(locale));

        for (int cycle = 0; cycle < 3; ++cycle) {
            manager.setRequestedLanguage(QStringLiteral("en-US"));
            if (manager.effectiveLanguage() != QStringLiteral("en-US")) {
                return fail(error, QStringLiteral("Intermediate English switch failed for %1.")
                                       .arg(locale));
            }
            manager.setRequestedLanguage(locale);
            if (manager.effectiveLanguage() != locale
                || qtTrId("gamehq.navigation.about") != about
                || qtTrId("gamehq.navigation.support_gamehq") != support) {
                return fail(error, QStringLiteral("Repeated switch retained stale text for %1.")
                                       .arg(locale));
            }
        }
        manager.setRequestedLanguage(QStringLiteral("system"));
        if (manager.requestedLanguage() != QStringLiteral("system")
            || manager.effectiveLanguage() != locale) {
            return fail(error, QStringLiteral("System locale resolution failed for %1.").arg(locale));
        }
        preference.bind(&manager);
        manager.setRequestedLanguage(locale);
        ConfigManager restartedConfig(configPath);
        if (!restartedConfig.load())
            return fail(error, QStringLiteral("Cannot reload isolated profile for %1.").arg(locale));
        ProbeBootstrapStore consumed({}, false);
        LanguagePreference restarted(&restartedConfig, &registry);
        if (restarted.initialLanguage(consumed, false) != locale)
            return fail(error, QStringLiteral("Persisted locale did not survive restart for %1.").arg(locale));

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
            {QStringLiteral("requested_locale"), locale},
            {QStringLiteral("effective_locale"), locale},
            {QStringLiteral("catalog"), catalog},
            {QStringLiteral("catalog_bytes"), catalogBytes},
            {QStringLiteral("about"), about},
            {QStringLiteral("support_gamehq"), support},
            {QStringLiteral("formatted_date"), renderedDate},
            {QStringLiteral("live_switch"), true},
            {QStringLiteral("repeated_switch"), true},
            {QStringLiteral("system_resolution"), true},
            {QStringLiteral("persistence_restart"), true},
            {QStringLiteral("installer_handoff_consumed"), true},
            {QStringLiteral("qml_retranslations"), qmlRetranslates},
            {QStringLiteral("release_count"), notes.releases().size()},
            {QStringLiteral("fallback_documents"), fallbackDocuments},
            {QStringLiteral("whole_document_notes"), true},
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
        {QStringLiteral("runtime_locale_count"), productionLocales().size()},
        {QStringLiteral("external_browser_opened"), false},
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
