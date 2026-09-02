#pragma once
#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QVariant>

// JSON config (config.json). Flat keys with dotted namespaces, e.g. "capture.mode".
// Unknown keys are preserved on save so future versions stay compatible.
class ConfigManager : public QObject
{
    Q_OBJECT
public:
    explicit ConfigManager(QString filePath, QObject* parent = nullptr);

    // Outcome of load(). A missing file is normal; anything else means the
    // caller must not let a later save() overwrite the only copy of the user's
    // settings without preserving it first.
    enum class LoadResult { Loaded, Missing, Quarantined, Unrecoverable };

    // Reads the file, quarantining an unreadable or malformed one to
    // config.corrupt-<timestamp>.json before falling back to defaults.
    LoadResult loadOrQuarantine();
    bool load();   // missing file is fine → defaults
    bool save() const;

    // Path the last quarantine wrote, for the one-time user-facing warning.
    QString quarantinedPath() const { return m_quarantinedPath; }

    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    bool hasExplicitValue(const QString& key) const;
    void setValue(const QString& key, const QVariant& value);
    QVariant defaultValue(const QString& key, const QVariant& fallback = {}) const;
    bool isDefault(const QString& key) const;
    bool resetValue(const QString& key);
    bool resetGroup(const QString& prefix);
    bool resetAll();

signals:
    void valueChanged(const QString& key, const QVariant& value);
    void groupReset(const QString& prefix);

private:
    QString m_quarantinedPath;
    static QJsonObject defaults();
    static bool preservesExplicitDefault(const QString& key);

    QString m_filePath;
    QJsonObject m_overrides;
};
