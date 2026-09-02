#pragma once

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QVariantList>
#include <QVector>

class LocaleRegistry final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QVariantList availableLanguages READ availableLanguages CONSTANT)

public:
    explicit LocaleRegistry(QObject *parent = nullptr);
    explicit LocaleRegistry(bool includeInternal, QObject *parent = nullptr);

    bool load(const QString &path, QString *error = nullptr);
    bool loadData(const QByteArray &data, QString *error = nullptr);

    QString sourceLanguage() const { return m_sourceLanguage; }
    QString defaultLocale() const { return m_defaultLocale; }
    QVariantList availableLanguages() const;
    QString canonicalTag(const QString &requested) const;
    QString resolveAvailable(const QString &requested) const;
    QString resolveSystem(const QStringList &uiLanguages) const;
    QString catalogName(const QString &tag) const;
    Qt::LayoutDirection layoutDirection(const QString &tag) const;
    bool isAvailable(const QString &tag) const;

private:
    struct Locale {
        QString tag;
        QString nativeName;
        QString englishName;
        QStringList aliases;
        QString state;
        QString direction;
        QString fallback;
        QString catalog;
        int tier = 0;
    };

    static QString keyFor(const QString &tag);
    static bool developmentLocalesEnabled();
    const Locale *find(const QString &tag) const;
    QString resolveKnown(const QString &canonical) const;

    bool m_includeInternal = false;
    QString m_sourceLanguage;
    QString m_defaultLocale;
    QVector<Locale> m_locales;
    QHash<QString, qsizetype> m_tagIndexes;
    QHash<QString, QString> m_aliases;
};
