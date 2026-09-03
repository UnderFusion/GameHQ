#pragma once

#include <QByteArray>
#include <QLocale>
#include <QString>
#include <QVariantList>
#include <functional>

class LocaleRegistry;

// Strict parser for the small, bundled release-notes document shown by the
// desktop About modal. It emits only plain strings; QML never receives HTML.
class ReleaseNotes
{
public:
    using BundleReader = std::function<QByteArray(const QString &filename)>;

    static ReleaseNotes fromJson(const QByteArray& json, QString* error = nullptr);
    static ReleaseNotes fromJson(const QByteArray& json, const QLocale& locale,
                                 QString* error = nullptr);
    // Release notes come only from the versioned source's generated,
    // integrity-checked locale bundles.
    static ReleaseNotes loadBundled(const QString &requestedLocale,
                                    const LocaleRegistry &registry,
                                    QString *error = nullptr);
    // Testable integrity boundary used by loadBundled(). The index is trusted
    // only as a compiled resource; every selected bundle still has to match its
    // declared filename, byte length, SHA-256 and internal document metadata.
    static ReleaseNotes loadVerifiedBundle(const QByteArray &indexJson,
                                           const QString &requestedLocale,
                                           const LocaleRegistry &registry,
                                           const BundleReader &reader,
                                           QString *error = nullptr);
    // Converts the small, untrusted Markdown subset used by GitHub release
    // bodies into plain structured data for QML. No HTML, links, or images
    // survive this boundary.
    static QVariantList blocksFromMarkdown(const QString& markdown);

    bool isValid() const
    {
        return !m_version.isEmpty() && !m_sections.isEmpty() && !m_releases.isEmpty();
    }
    QString version() const { return m_version; }
    QString locale() const { return m_locale; }
    QVariantList sections() const { return m_sections; }
    QVariantList releases() const { return m_releases; }

private:
    QString m_version;
    QString m_locale;
    QVariantList m_sections;
    QVariantList m_releases;
};
