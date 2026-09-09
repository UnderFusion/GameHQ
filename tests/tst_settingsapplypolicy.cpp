#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "config/SettingsApplyPolicy.h"

#include <QSet>
#include <QTest>

class SettingsApplyPolicyTest : public QObject
{
    Q_OBJECT

private slots:
    void restartKeysAreExplicit();
    void liveKeysAreExplicit();
    void everyBuiltInDefaultIsClassified();
    void groupResetsRestartAtMostOnce();
};

void SettingsApplyPolicyTest::restartKeysAreExplicit()
{
    const QStringList keys{
        ConfigKeys::ReplayResolution,
        ConfigKeys::ReplayFps,
        ConfigKeys::ReplayBitrateMbps,
        ConfigKeys::ReplayLengthSeconds,
        ConfigKeys::ReplaySegmentSeconds,
        ConfigKeys::ReplayAuto,
        ConfigKeys::AudioEnabled,
    };
    for (const QString& key : keys)
        QVERIFY2(SettingsApplyPolicy::requiresReplayBufferRestart(key), qPrintable(key));
}

void SettingsApplyPolicyTest::liveKeysAreExplicit()
{
    const QStringList keys{
        ConfigKeys::ReplayClipSound,
        ConfigKeys::ReplayClipNotify,
        ConfigKeys::ReplayManualIdleSeconds,
        ConfigKeys::CaptureMode,
        ConfigKeys::CaptureHideBorder,
        ConfigKeys::SoundsCaptureVolume,
        ConfigKeys::NotificationsEnabled,
    };
    for (const QString& key : keys)
        QVERIFY2(!SettingsApplyPolicy::requiresReplayBufferRestart(key), qPrintable(key));
    QVERIFY(!SettingsApplyPolicy::requiresReplayBufferRestart(QStringLiteral("future.key")));
}

void SettingsApplyPolicyTest::everyBuiltInDefaultIsClassified()
{
    const QStringList defaultList = ConfigManager::defaultKeys();
    const QSet<QString> defaults(defaultList.cbegin(), defaultList.cend());
    const QStringList classifiedList = SettingsApplyPolicy::classifiedKeys();
    const QSet<QString> classified(classifiedList.cbegin(), classifiedList.cend());
    QCOMPARE(classified.size(), classifiedList.size());
    QCOMPARE(classified, defaults);
}

void SettingsApplyPolicyTest::groupResetsRestartAtMostOnce()
{
    const QList<QStringList> groups{
        { ConfigKeys::ReplayClipSound, ConfigKeys::ReplayFps,
          ConfigKeys::ReplayManualIdleSeconds, ConfigKeys::ReplayLengthSeconds },
        { ConfigKeys::SoundsEnabled, ConfigKeys::SoundsCaptureVolume },
        { ConfigKeys::CaptureMode, ConfigKeys::CaptureHideBorder },
        { ConfigKeys::NotificationsEnabled },
    };

    int restartCount = 0;
    for (const QStringList& group : groups) {
        if (SettingsApplyPolicy::requiresReplayBufferRestart(group))
            ++restartCount;
    }
    QCOMPARE(restartCount, 1);
}

QTEST_GUILESS_MAIN(SettingsApplyPolicyTest)
#include "tst_settingsapplypolicy.moc"
