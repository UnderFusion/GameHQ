#pragma once

#include <QByteArray>
#include <QLocale>
#include <QString>
#include <QVariantList>

// Strict parser for the small, bundled release-notes document shown by the
// desktop About modal. It emits only plain strings; QML never receives HTML.
class ReleaseNotes
{
public:
    static ReleaseNotes fromJson(const QByteArray& json, QString* error = nullptr);
    static ReleaseNotes fromJson(const QByteArray& json, const QLocale& locale,
                                 QString* error = nullptr);
    static ReleaseNotes loadBundled();
    // Converts the small, untrusted Markdown subset used by GitHub release
    // bodies into plain structured data for QML. No HTML, links, or images
    // survive this boundary.
    static QVariantList blocksFromMarkdown(const QString& markdown);

    bool isValid() const
    {
        return !m_version.isEmpty() && !m_sections.isEmpty() && !m_releases.isEmpty();
    }
    QString version() const { return m_version; }
    QVariantList sections() const { return m_sections; }
    QVariantList releases() const { return m_releases; }

private:
    QString m_version;
    QVariantList m_sections;
    QVariantList m_releases;
};
