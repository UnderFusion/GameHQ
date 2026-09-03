#pragma once

#include <QObject>
#include <QDateTime>
#include <QStringList>
#include <QVariantList>
#include <functional>
#include <memory>

class LocaleRegistry;
class QTranslator;

class LanguageManager final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString requestedLanguage READ requestedLanguage WRITE setRequestedLanguage
               NOTIFY requestedLanguageChanged)
    Q_PROPERTY(QString effectiveLanguage READ effectiveLanguage NOTIFY languageChanged)
    Q_PROPERTY(QVariantList availableLanguages READ availableLanguages CONSTANT)
    Q_PROPERTY(QString localeName READ localeName NOTIFY languageChanged)
    Q_PROPERTY(Qt::LayoutDirection layoutDirection READ layoutDirection NOTIFY languageChanged)
    Q_PROPERTY(int translationRevision READ translationRevision NOTIFY translationRevisionChanged)

public:
    explicit LanguageManager(LocaleRegistry *registry, QObject *parent = nullptr);
    ~LanguageManager() override;

    bool initialize(const QString &requestedLanguage,
                    const QStringList &systemUiLanguages = {}, QString *error = nullptr);

    QString requestedLanguage() const { return m_requestedLanguage; }
    QString effectiveLanguage() const { return m_effectiveLanguage; }
    QVariantList availableLanguages() const;
    QString localeName() const { return m_effectiveLanguage; }
    Qt::LayoutDirection layoutDirection() const;
    int translationRevision() const { return m_translationRevision; }
    const LocaleRegistry *localeRegistry() const { return m_registry; }
    void setQmlRetranslateCallback(std::function<void()> callback);

    Q_INVOKABLE QString formatInteger(qint64 value) const;
    Q_INVOKABLE QString formatDecimal(double value, int precision = 1) const;
    Q_INVOKABLE QString formatDate(const QDateTime &value) const;
    Q_INVOKABLE QString formatDateTime(const QDateTime &value) const;
    Q_INVOKABLE QString formatDuration(qint64 milliseconds) const;
    Q_INVOKABLE QString formatList(const QStringList &values) const;

public slots:
    void setRequestedLanguage(const QString &requestedLanguage);

signals:
    void requestedLanguageChanged();
    void languageChanged();
    void translationRevisionChanged();
    void retranslationRequested();
    void catalogLoadFailed(const QString &language, const QString &reason);

private:
    bool apply(const QString &requestedLanguage, const QStringList &systemUiLanguages,
               QString *error);
    QString catalogPath(const QString &tag) const;
    void uninstallTranslators();

    LocaleRegistry *m_registry = nullptr;
    QString m_requestedLanguage;
    QString m_effectiveLanguage;
    QStringList m_systemUiLanguages;
    int m_translationRevision = 0;
    std::unique_ptr<QTranslator> m_sourceTranslator;
    std::unique_ptr<QTranslator> m_activeTranslator;
    std::function<void()> m_qmlRetranslate;
};
