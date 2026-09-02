#pragma once

#include <QObject>
#include <QSettings>
#include <QString>

class ConfigManager;
class LanguageManager;
class LocaleRegistry;

class LanguageBootstrapStore
{
public:
    virtual ~LanguageBootstrapStore() = default;
    virtual bool hasValue() const = 0;
    virtual QString value() const = 0;
    virtual bool removeValue() = 0;
};

// One-time installer-to-application handoff. The installer side is added in a
// later plan item; this class only reads and clears its narrowly scoped value.
class RegistryLanguageBootstrapStore final : public LanguageBootstrapStore
{
public:
    RegistryLanguageBootstrapStore();

    bool hasValue() const override;
    QString value() const override;
    bool removeValue() override;

private:
    QSettings m_settings;
};

// Coordinates persistence only. LanguageManager remains the sole translator
// owner and the source of runtime language state.
class LanguagePreference final : public QObject
{
public:
    LanguagePreference(ConfigManager *config, LocaleRegistry *registry,
                       QObject *parent = nullptr);

    QString initialLanguage(LanguageBootstrapStore &bootstrap, bool portable,
                            QString *warning = nullptr);
    void bind(LanguageManager *manager);

private:
    QString normalized(const QString &requested) const;
    static void appendWarning(QString *warning, const QString &message);

    ConfigManager *m_config = nullptr;
    LocaleRegistry *m_registry = nullptr;
    LanguageManager *m_manager = nullptr;
    bool m_applyingConfig = false;
};
