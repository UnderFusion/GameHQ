#include "config/CaptureLocations.h"

#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/Paths.h"
#include "core/GameIdentity.h"
#include "localization/NativeText.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QVariantList>

namespace
{
const QString kScreenshotRoot = ConfigKeys::StorageScreenshotsRoot;
const QString kClipRoot = ConfigKeys::StorageClipsRoot;
// Safety metadata, not a user-facing setting. AppController preserves current
// roots before a reset and ConfigManager keeps internal.* keys during reset-all.
const QString kRootHistory = ConfigKeys::InternalCaptureRootHistory;

QString comparisonKey(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath()).toCaseFolded();
}
}

CaptureLocations::CaptureLocations(ConfigManager* config, QObject* parent)
    : QObject(parent), m_config(config)
{
    connect(m_config, &ConfigManager::valueChanged, this,
            [this](const QString& key, const QVariant&) {
                if (key == kScreenshotRoot || key == kClipRoot || key == kRootHistory)
                    emit locationsChanged();
            });
}

QString CaptureLocations::configuredRoot(const QString& key) const
{
    const QString configured = m_config->value(key, QString()).toString().trimmed();
    return configured.isEmpty() ? QDir::cleanPath(Paths::capturesRoot())
                                : Paths::fromStoredPath(configured);
}

QString CaptureLocations::screenshotsBaseRoot() const { return configuredRoot(kScreenshotRoot); }
QString CaptureLocations::clipsBaseRoot() const { return configuredRoot(kClipRoot); }

QString CaptureLocations::screenshotDir(const QString& gameName) const
{
    return screenshotsBaseRoot() + QLatin1Char('/') + GameIdentity::folderName(gameName)
           + QStringLiteral("/Screenshots");
}

QString CaptureLocations::clipsDir(const QString& gameName) const
{
    return clipsBaseRoot() + QLatin1Char('/') + GameIdentity::folderName(gameName)
           + QStringLiteral("/Clips");
}

QStringList CaptureLocations::managedRoots() const
{
    // Always retain the portable/installed default as a legacy scan root. This
    // matters when both writers have moved elsewhere: existing default-folder
    // media must not disappear from a rebuilt library.
    QStringList candidates{Paths::capturesRoot(), screenshotsBaseRoot(), clipsBaseRoot()};
    const QVariantList history = m_config->value(kRootHistory, QVariantList()).toList();
    for (const QVariant& value : history)
        candidates.append(value.toString());

    QStringList result;
    QStringList seen;
    for (const QString& candidate : candidates) {
        if (candidate.trimmed().isEmpty())
            continue;
        const QString clean = Paths::fromStoredPath(candidate);
        const QString key = comparisonKey(clean);
        if (!seen.contains(key)) {
            seen.append(key);
            result.append(clean);
        }
    }
    return result;
}

QString CaptureLocations::keyFor(Kind kind) const
{
    return kind == Kind::Screenshots ? kScreenshotRoot : kClipRoot;
}

void CaptureLocations::rememberPreviousRoot(const QString& root)
{
    if (root.isEmpty() || comparisonKey(root) == comparisonKey(Paths::capturesRoot()))
        return;
    QVariantList history = m_config->value(kRootHistory, QVariantList()).toList();
    for (const QVariant& value : history) {
        if (comparisonKey(value.toString()) == comparisonKey(root))
            return;
    }
    history.append(QDir::cleanPath(root));
    m_config->setValue(kRootHistory, history);
}

bool CaptureLocations::setBaseRoot(Kind kind, const QString& path, QString* error)
{
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        if (error) *error = NativeText::get(
            //: Capture-location validation error shown in Settings.
            //% "The selected folder is invalid."
            QT_TRID_NOOP("gamehq.error.capture_location.folder_invalid"),
            "The selected folder is invalid.");
        return false;
    }
    const QString clean = Paths::fromStoredPath(trimmed);
    if (clean.isEmpty() || clean == QStringLiteral(".")) {
        if (error) *error = NativeText::get(
            //% "The selected folder is invalid."
            QT_TRID_NOOP("gamehq.error.capture_location.folder_invalid"),
            "The selected folder is invalid.");
        return false;
    }
    if (!QDir().mkpath(clean)) {
        if (error) *error = NativeText::get(
            //: Capture-location filesystem error shown in Settings.
            //% "The selected folder could not be created."
            QT_TRID_NOOP("gamehq.error.capture_location.create_failed"),
            "The selected folder could not be created.");
        return false;
    }
    QTemporaryFile probe(clean + QStringLiteral("/.gamehq-write-test-XXXXXX.tmp"));
    probe.setAutoRemove(true);
    if (!probe.open()) {
        if (error) *error = NativeText::get(
            //: Capture-location filesystem error shown in Settings.
            //% "The selected folder is not writable."
            QT_TRID_NOOP("gamehq.error.capture_location.not_writable"),
            "The selected folder is not writable.");
        return false;
    }
    probe.close();

    const QString key = keyFor(kind);
    const QString previous = configuredRoot(key);
    if (comparisonKey(previous) == comparisonKey(clean))
        return true;

    const bool rootWasDefault = m_config->isDefault(key);
    const QVariant previousRootValue = m_config->value(key);
    const bool historyWasDefault = m_config->isDefault(kRootHistory);
    const QVariant previousHistory = m_config->value(kRootHistory, QVariantList());
    rememberPreviousRoot(previous);

    // Selecting the current default should remain a real default rather than
    // persisting an absolute path that would break if a portable package moves.
    if (comparisonKey(clean) == comparisonKey(Paths::capturesRoot()))
        m_config->resetValue(key);
    else
        m_config->setValue(key, Paths::toStoredPath(clean));

    if (m_config->save())
        return true;

    // Keep the live state consistent with disk when config persistence fails.
    if (rootWasDefault)
        m_config->resetValue(key);
    else
        m_config->setValue(key, previousRootValue);
    if (historyWasDefault)
        m_config->resetValue(kRootHistory);
    else
        m_config->setValue(kRootHistory, previousHistory);
    if (error) *error = NativeText::get(
        //: Capture-location persistence error shown in Settings.
        //% "GameHQ could not save the selected folder."
        QT_TRID_NOOP("gamehq.error.capture_location.save_failed"),
        "GameHQ could not save the selected folder.");
    return false;
}

bool CaptureLocations::resetBaseRoot(Kind kind, QString* error)
{
    const QString key = keyFor(kind);
    if (m_config->isDefault(key))
        return true;

    const QVariant previousRootValue = m_config->value(key);
    const bool historyWasDefault = m_config->isDefault(kRootHistory);
    const QVariant previousHistory = m_config->value(kRootHistory, QVariantList());
    rememberPreviousRoot(configuredRoot(key));
    m_config->resetValue(key);
    if (m_config->save())
        return true;

    m_config->setValue(key, previousRootValue);
    if (historyWasDefault)
        m_config->resetValue(kRootHistory);
    else
        m_config->setValue(kRootHistory, previousHistory);
    if (error) *error = NativeText::get(
        //: Capture-location persistence error shown in Settings.
        //% "GameHQ could not restore the default folder."
        QT_TRID_NOOP("gamehq.error.capture_location.restore_default_failed"),
        "GameHQ could not restore the default folder.");
    return false;
}

void CaptureLocations::preserveCurrentRoots()
{
    rememberPreviousRoot(screenshotsBaseRoot());
    rememberPreviousRoot(clipsBaseRoot());
}
