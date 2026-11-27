#pragma once
#include <QObject>
#include <QJsonObject>
#include <QString>
#include <QStringList>
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
    // Writes config.json atomically when anything changed since the last
    // successful write; otherwise returns true without touching the disk.
    // A failed write keeps the changes pending, so the next save retries.
    bool save() const;
    bool hasUnsavedChanges() const { return m_dirty; }
    // Diagnostics: successful writes of the file by this instance.
    int fileWrites() const { return m_fileWrites; }
    bool setCaptureExecutableAllowed(const QString& path, bool allowed);

    // Path the last quarantine wrote, for the one-time user-facing warning.
    QString quarantinedPath() const { return m_quarantinedPath; }

    QVariant value(const QString& key, const QVariant& fallback = {}) const;
    bool hasExplicitValue(const QString& key) const;
    bool setValue(const QString& key, const QVariant& value);
    QVariant defaultValue(const QString& key, const QVariant& fallback = {}) const;
    bool isDefault(const QString& key) const;
    bool resetValue(const QString& key);
    bool resetGroup(const QString& prefix);
    bool resetAll();
    QStringList overriddenKeys(const QString& prefix = {}) const;
    static QStringList defaultKeys();

signals:
    void valueChanged(const QString& key, const QVariant& value);
    void groupReset(const QString& prefix);

private:
    QString m_quarantinedPath;
    static QJsonObject defaults();
    static bool preservesExplicitDefault(const QString& key);

    QString m_filePath;
    QJsonObject m_overrides;
    // Not yet written: nothing loaded, a missing or quarantined file, a value
    // canonicalized on load, or a change since the last successful save.
    mutable bool m_dirty = true;
    mutable int m_fileWrites = 0;
};
