#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/SettingsCategories.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

// ConfigManager stores only what the user actually changed: defaults live in
// code, config.json holds overrides. These tests run against a real file in a
// QTemporaryDir — the class takes its path by constructor, so no seam is needed.
class TestConfigManager : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;

    QString path() const { return m_dir.path() + QStringLiteral("/config.json"); }

    QJsonObject readFile() const
    {
        QFile f(path());
        if (!f.open(QIODevice::ReadOnly))
            return {};
        return QJsonDocument::fromJson(f.readAll()).object();
    }

    static void resetCategory(ConfigManager& cfg, const QString& category)
    {
        for (const QString& prefix : SettingsCategories::groups().value(category))
            cfg.resetGroup(prefix);
        for (const QString& key : SettingsCategories::keys().value(category))
            cfg.resetValue(key);
    }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QFile::remove(path());
    }

    void corruptConfigIsPreservedBeforeDefaults()
    {
        // Silently starting on defaults let the next save() destroy the only
        // copy of the user's settings.
        QFile file(path());
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{ \"capture.mode\": \"whitel") > 0);
        file.close();

        ConfigManager cfg(path());
        QCOMPARE(cfg.loadOrQuarantine(), ConfigManager::LoadResult::Quarantined);
        const QString preserved = cfg.quarantinedPath();
        QVERIFY(!preserved.isEmpty());
        QVERIFY(QFile::exists(preserved));
        QVERIFY(!QFile::exists(path()));
        QCOMPARE(cfg.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("only_in_games"));

        // The preserved copy still holds exactly what the user had.
        QFile kept(preserved);
        QVERIFY(kept.open(QIODevice::ReadOnly));
        QCOMPARE(kept.readAll(), QByteArray("{ \"capture.mode\": \"whitel"));
        kept.close();

        // Saving now writes a fresh file and leaves the preserved one alone.
        QVERIFY(cfg.save());
        QVERIFY(QFile::exists(path()));
        QVERIFY(QFile::exists(preserved));
        QFile::remove(preserved);
    }

    void missingAndValidConfigAreDistinguished()
    {
        ConfigManager missing(path());
        QCOMPARE(missing.loadOrQuarantine(), ConfigManager::LoadResult::Missing);
        QVERIFY(missing.quarantinedPath().isEmpty());

        missing.setValue(ConfigKeys::ReplayFps, 60);
        QVERIFY(missing.save());

        ConfigManager reopened(path());
        QCOMPARE(reopened.loadOrQuarantine(), ConfigManager::LoadResult::Loaded);
        QVERIFY(reopened.quarantinedPath().isEmpty());
        QCOMPARE(reopened.value(ConfigKeys::ReplayFps).toInt(), 60);
    }

    void missingFileLoadsDefaults()
    {
        ConfigManager cfg(path());
        QVERIFY(cfg.load());   // a missing file is not an error — it is a first run
        QCOMPARE(cfg.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("only_in_games"));
        QCOMPARE(cfg.value(ConfigKeys::ReplayLengthSeconds).toInt(), 300);
        QCOMPARE(cfg.value(ConfigKeys::ThemeActiveSkin).toString(), QStringLiteral("obsidian"));
        QVERIFY(cfg.isDefault(ConfigKeys::CaptureMode));
        QVERIFY(cfg.isDefault(ConfigKeys::ThemeActiveSkin));
    }

    void captureBorderDefaultsAndPersistence()
    {
        ConfigManager cfg(path());
        QVERIFY(cfg.load());
        QVERIFY(cfg.value(ConfigKeys::CaptureHideBorder).toBool());
        QVERIFY(cfg.isDefault(ConfigKeys::CaptureHideBorder));
        cfg.setValue(ConfigKeys::CaptureHideBorder, false);
        QVERIFY(cfg.save());
        ConfigManager reopened(path());
        QVERIFY(reopened.load());
        QVERIFY(!reopened.value(ConfigKeys::CaptureHideBorder).toBool());
        QVERIFY(!reopened.isDefault(ConfigKeys::CaptureHideBorder));
        resetCategory(reopened, QStringLiteral("Capture"));
        QVERIFY(reopened.value(ConfigKeys::CaptureHideBorder).toBool());
        QVERIFY(reopened.isDefault(ConfigKeys::CaptureHideBorder));
        QVERIFY(reopened.save());
        ConfigManager reset(path());
        QVERIFY(reset.load());
        QVERIFY(reset.value(ConfigKeys::CaptureHideBorder).toBool());
    }

    void unknownKeyFallsBackToTheCallersValue()
    {
        ConfigManager cfg(path());
        cfg.load();
        QCOMPARE(cfg.value("nope.not_a_key", 42).toInt(), 42);
    }

    void setValueOverridesDefault()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);

        QCOMPARE(cfg.value(ConfigKeys::ReplayFps).toInt(), 60);
        QVERIFY(!cfg.isDefault(ConfigKeys::ReplayFps));
        // The default itself must stay reachable and unchanged.
        QCOMPARE(cfg.defaultValue(ConfigKeys::ReplayFps).toInt(), 30);
    }

    void setValueEmitsValueChanged()
    {
        ConfigManager cfg(path());
        cfg.load();
        QSignalSpy spy(&cfg, &ConfigManager::valueChanged);

        cfg.setValue(ConfigKeys::ReplayFps, 60);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QString(ConfigKeys::ReplayFps));
        QCOMPARE(spy.at(0).at(1).toInt(), 60);
    }

    void saveWritesOnlyOverrides()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);
        QVERIFY(cfg.save());

        // Defaults must not be baked into the file, or changing a default in a
        // later version would silently not reach existing installs.
        const QJsonObject json = readFile();
        QCOMPARE(json.size(), 1);
        QVERIFY(json.contains(ConfigKeys::ReplayFps));
        QCOMPARE(json.value(ConfigKeys::ReplayFps).toInt(), 60);
    }

    void overridesSurviveAReload()
    {
        {
            ConfigManager cfg(path());
            cfg.load();
            cfg.setValue(ConfigKeys::ReplayFps, 60);
            cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));
            QVERIFY(cfg.save());
        }
        ConfigManager reloaded(path());
        QVERIFY(reloaded.load());
        QCOMPARE(reloaded.value(ConfigKeys::ReplayFps).toInt(), 60);
        QCOMPARE(reloaded.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("always"));
        QVERIFY(!reloaded.isDefault(ConfigKeys::ReplayFps));
        QVERIFY(reloaded.isDefault(ConfigKeys::ReplayBitrateMbps));
    }

    void unknownKeysInTheFileArePreserved()
    {
        // Forward compatibility: a config written by a newer version must not
        // lose its keys just because this version round-tripped the file.
        {
            QFile f(path());
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(QJsonObject{
                { "future.setting", "keep me" },
                { QString(ConfigKeys::ReplayFps), 60 },
            }).toJson());
        }
        ConfigManager cfg(path());
        QVERIFY(cfg.load());
        cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));
        QVERIFY(cfg.save());

        QCOMPARE(readFile().value("future.setting").toString(), QStringLiteral("keep me"));
    }

    void resetValueRestoresTheDefault()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);

        QVERIFY(cfg.resetValue(ConfigKeys::ReplayFps));
        QCOMPARE(cfg.value(ConfigKeys::ReplayFps).toInt(), 30);
        QVERIFY(cfg.isDefault(ConfigKeys::ReplayFps));
    }

    void resetValueReportsWhetherAnythingChanged()
    {
        ConfigManager cfg(path());
        cfg.load();
        // Never overridden → nothing to reset.
        QVERIFY(!cfg.resetValue(ConfigKeys::ReplayFps));
    }

    void resetGroupResetsOnlyItsOwnPrefix()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);
        cfg.setValue(ConfigKeys::ReplayBitrateMbps, 20);
        cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));

        QVERIFY(cfg.resetGroup("replay"));

        QVERIFY(cfg.isDefault(ConfigKeys::ReplayFps));
        QVERIFY(cfg.isDefault(ConfigKeys::ReplayBitrateMbps));
        // A different group must be untouched.
        QCOMPARE(cfg.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("always"));
    }

    void resetGroupAcceptsATrailingDot()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);
        QVERIFY(cfg.resetGroup("replay."));
        QVERIFY(cfg.isDefault(ConfigKeys::ReplayFps));
    }

    void resetGroupDoesNotMatchOnPrefixAlone()
    {
        // "replay" must not reach "replay_something.x" — the dot is the boundary.
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue("replay_other.key", 1);
        QVERIFY(!cfg.resetGroup("replay"));
        QCOMPARE(cfg.value("replay_other.key").toInt(), 1);
    }

    void resetGroupEmitsGroupReset()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);
        QSignalSpy spy(&cfg, &ConfigManager::groupReset);

        QVERIFY(cfg.resetGroup("replay"));
        QCOMPARE(spy.count(), 1);
    }

    void resetAllClearsEveryOverride()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);
        cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));

        QVERIFY(cfg.resetAll());

        QCOMPARE(cfg.value(ConfigKeys::ReplayFps).toInt(), 30);
        QCOMPARE(cfg.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("only_in_games"));
        QVERIFY(cfg.isDefault(ConfigKeys::ReplayFps));
        QVERIFY(cfg.isDefault(ConfigKeys::CaptureMode));
    }

    void generalCategoryResetIncludesThemeButNotCapture()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::StartupMinimized, true);
        cfg.setValue(ConfigKeys::TrayMinimizeToTray, true);
        cfg.setValue(ConfigKeys::ThemeActiveSkin, QStringLiteral("light"));
        cfg.setValue(ConfigKeys::ThemeOverlayScrimStrength, 140);
        cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));

        resetCategory(cfg, QStringLiteral("General"));

        QVERIFY(cfg.isDefault(ConfigKeys::StartupMinimized));
        QVERIFY(cfg.isDefault(ConfigKeys::TrayMinimizeToTray));
        QVERIFY(cfg.isDefault(ConfigKeys::ThemeActiveSkin));
        QVERIFY(cfg.isDefault(ConfigKeys::ThemeOverlayScrimStrength));
        QCOMPARE(cfg.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("always"));
    }

    void feedbackCategoryResetIncludesMirroredEventsOnly()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::SoundsEnabled, false);
        cfg.setValue(ConfigKeys::NotificationsEnabled, false);
        cfg.setValue(ConfigKeys::CaptureScreenshotSound, false);
        cfg.setValue(ConfigKeys::CaptureScreenshotNotify, false);
        cfg.setValue(ConfigKeys::ReplayClipSound, false);
        cfg.setValue(ConfigKeys::ReplayClipNotify, false);
        cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));
        cfg.setValue(ConfigKeys::ReplayFps, 60);

        resetCategory(cfg, QStringLiteral("Notifications & Sound"));

        QVERIFY(cfg.isDefault(ConfigKeys::SoundsEnabled));
        QVERIFY(cfg.isDefault(ConfigKeys::NotificationsEnabled));
        QVERIFY(cfg.isDefault(ConfigKeys::CaptureScreenshotSound));
        QVERIFY(cfg.isDefault(ConfigKeys::CaptureScreenshotNotify));
        QVERIFY(cfg.isDefault(ConfigKeys::ReplayClipSound));
        QVERIFY(cfg.isDefault(ConfigKeys::ReplayClipNotify));
        QCOMPARE(cfg.value(ConfigKeys::CaptureMode).toString(), QStringLiteral("always"));
        QCOMPARE(cfg.value(ConfigKeys::ReplayFps).toInt(), 60);
    }

    void captureCategoryResetPreservesInternalRootHistory()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::CaptureMode, QStringLiteral("always"));
        cfg.setValue(ConfigKeys::StorageScreenshotsRoot, QStringLiteral("D:/Screenshots"));
        cfg.setValue(ConfigKeys::StorageClipsRoot, QStringLiteral("E:/Clips"));
        cfg.setValue(ConfigKeys::InternalCaptureRootHistory,
                     QStringList{QStringLiteral("D:/Old"), QStringLiteral("E:/Older")});

        resetCategory(cfg, QStringLiteral("Capture"));

        QVERIFY(cfg.isDefault(ConfigKeys::CaptureMode));
        QVERIFY(cfg.isDefault(ConfigKeys::StorageScreenshotsRoot));
        QVERIFY(cfg.isDefault(ConfigKeys::StorageClipsRoot));
        QCOMPARE(cfg.value(ConfigKeys::InternalCaptureRootHistory).toStringList(),
                 QStringList({QStringLiteral("D:/Old"), QStringLiteral("E:/Older")}));
    }

    void resetAllPreservesInternalSafetyMetadata()
    {
        ConfigManager cfg(path());
        cfg.load();
        cfg.setValue(ConfigKeys::ReplayFps, 60);
        cfg.setValue(ConfigKeys::InternalCaptureRootHistory,
                     QStringList{QStringLiteral("D:/Old")});
        cfg.setValue(ConfigKeys::InternalUpdatesPendingPostUpdateVersion,
                     QStringLiteral("9.9.9"));

        QVERIFY(cfg.resetAll());

        QVERIFY(cfg.isDefault(ConfigKeys::ReplayFps));
        QCOMPARE(cfg.value(ConfigKeys::InternalCaptureRootHistory).toStringList(),
                 QStringList({QStringLiteral("D:/Old")}));
        QCOMPARE(cfg.value(ConfigKeys::InternalUpdatesPendingPostUpdateVersion).toString(),
                 QStringLiteral("9.9.9"));
    }
};

QTEST_MAIN(TestConfigManager)
#include "tst_configmanager.moc"
