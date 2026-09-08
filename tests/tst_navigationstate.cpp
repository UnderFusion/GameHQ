#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "ui/NavigationState.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

// M10 — last-view memory. NavigationState is the whole rule set from
// docs/product-spec.md "Navigation memory" (R1-R10) expressed as pure logic
// over ConfigManager, so every rule can be checked here without the database,
// the QML engine or a window.
class TestNavigationState : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    QString path() const { return m_dir.path() + QStringLiteral("/config.json"); }

private slots:
    void init()
    {
        QVERIFY(m_dir.isValid());
        QFile::remove(path());
    }

    // R1/R6/R7: a fresh profile opens on the gallery, General and every game.
    void defaultsAreTheFirstRunView()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        QCOMPARE(nav.page(), QStringLiteral("gallery"));
        QCOMPARE(nav.settingsCategory(), QStringLiteral("general"));
        const NavigationState::GalleryFilter filter = nav.galleryFilter({});
        QCOMPARE(filter.category, QStringLiteral("all"));
        QCOMPARE(filter.gameId, -1);
        // Defaults are never written, so a user who only browses keeps
        // config.json free of navigation noise.
        QVERIFY(cfg.isDefault(ConfigKeys::UiPage));
        QVERIFY(cfg.isDefault(ConfigKeys::UiGalleryFilterCategory));
    }

    // R1: page and category survive a restart; Help is not a page at all.
    void pageAndCategoryRoundTrip()
    {
        {
            ConfigManager cfg(path());
            NavigationState nav(&cfg);
            nav.setPage(QStringLiteral("settings"));
            nav.setSettingsCategory(QStringLiteral("replay"));
            QVERIFY(cfg.save());
        }
        ConfigManager cfg(path());
        QVERIFY(cfg.load());
        NavigationState nav(&cfg);
        QCOMPARE(nav.page(), QStringLiteral("settings"));
        QCOMPARE(nav.settingsCategory(), QStringLiteral("replay"));
    }

    void helpIsNeverRestored()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setPage(QStringLiteral("help"));
        QCOMPARE(nav.page(), QStringLiteral("gallery"));
        cfg.setValue(ConfigKeys::UiPage, QStringLiteral("help"));
        QCOMPARE(nav.page(), QStringLiteral("gallery"));
    }

    void unknownStoredValuesFallBackToDefaults()
    {
        ConfigManager cfg(path());
        cfg.setValue(ConfigKeys::UiSettingsCategory, QStringLiteral("nonsense"));
        cfg.setValue(ConfigKeys::UiGalleryFilterCategory, QStringLiteral("nonsense"));
        NavigationState nav(&cfg);
        QCOMPARE(nav.settingsCategory(), QStringLiteral("general"));
        QCOMPARE(nav.galleryFilter({}).category, QStringLiteral("all"));
    }

    // R6: releases up to 0.7.10 stored the sidebar index. It is rewritten as a
    // key exactly once, and a second start finds nothing left to migrate.
    void legacyCategoryIndexMigratesOnceAndIsIdempotent()
    {
        ConfigManager cfg(path());
        cfg.setValue(ConfigKeys::UiSettingsCategory, 5);   // Notifications & sound
        NavigationState nav(&cfg);
        nav.migrateLegacyKeys();
        QCOMPARE(cfg.value(ConfigKeys::UiSettingsCategory).toString(),
                 QStringLiteral("notifications_sound"));
        nav.migrateLegacyKeys();
        QCOMPARE(cfg.value(ConfigKeys::UiSettingsCategory).toString(),
                 QStringLiteral("notifications_sound"));
        // A key the user then changes is left alone by later migrations.
        nav.setSettingsCategory(QStringLiteral("input"));
        nav.migrateLegacyKeys();
        QCOMPARE(nav.settingsCategory(), QStringLiteral("input"));
    }

    void legacyCategoryIndexOutOfRangeFallsBackToGeneral()
    {
        ConfigManager cfg(path());
        cfg.setValue(ConfigKeys::UiSettingsCategory, 99);
        NavigationState nav(&cfg);
        nav.migrateLegacyKeys();
        QCOMPARE(nav.settingsCategory(), QStringLiteral("general"));
        // The migrated value equals the default, so it stops being an override.
        QVERIFY(cfg.isDefault(ConfigKeys::UiSettingsCategory));
    }

    void migrationIsANoOpWithoutAStoredValue()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.migrateLegacyKeys();
        QVERIFY(cfg.isDefault(ConfigKeys::UiSettingsCategory));
    }

    // R7: the saved game must still exist, otherwise the gallery would reopen
    // empty with nothing on screen explaining why.
    void galleryFilterKeepsAGameThatStillExists()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setGalleryFilter(QStringLiteral("favorites"), 12);
        const NavigationState::GalleryFilter filter = nav.galleryFilter({ 7, 12 });
        QCOMPARE(filter.category, QStringLiteral("favorites"));
        QCOMPARE(filter.gameId, 12);
    }

    void galleryFilterDropsAGameThatIsGone()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setGalleryFilter(QStringLiteral("favorites"), 12);
        const NavigationState::GalleryFilter filter = nav.galleryFilter({ 7 });
        QCOMPARE(filter.category, QStringLiteral("all"));
        QCOMPARE(filter.gameId, -1);
    }

    // R5: an explicit one-shot jump navigates without rewriting the memory.
    void transientNavigationDoesNotPersist()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setGalleryFilter(QStringLiteral("clips"), 3);
        nav.setGalleryFilter(QStringLiteral("screenshots"), 9, NavigationState::Persist::No);
        nav.setPage(QStringLiteral("settings"), NavigationState::Persist::No);
        nav.setSettingsCategory(QStringLiteral("about"), NavigationState::Persist::No);
        nav.setOverlayCategory(3, QStringLiteral("clips"), NavigationState::Persist::No);

        const NavigationState::GalleryFilter filter = nav.galleryFilter({ 3, 9 });
        QCOMPARE(filter.category, QStringLiteral("clips"));
        QCOMPARE(filter.gameId, 3);
        QCOMPARE(nav.page(), QStringLiteral("gallery"));
        QCOMPARE(nav.settingsCategory(), QStringLiteral("general"));
        QCOMPARE(nav.overlayCategory(3), QStringLiteral("all"));
    }

    // R8: the overlay remembers a category per game; the game binding itself is
    // always the foreground game and is never restored.
    void overlayCategoryIsRememberedPerGame()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setOverlayCategory(4, QStringLiteral("favorites"));
        nav.setOverlayCategory(8, QStringLiteral("clips"));
        QCOMPARE(nav.overlayCategory(4), QStringLiteral("favorites"));
        QCOMPARE(nav.overlayCategory(8), QStringLiteral("clips"));
        QCOMPARE(nav.overlayCategory(99), QStringLiteral("all"));
        QCOMPARE(nav.overlayCategory(-1), QStringLiteral("all"));

        // Only "all" and "favorites" stay narrowed to the game — the same rule
        // SidebarCategories.resolveFilter() applies on the QML side.
        QCOMPARE(nav.overlayFilter(4).gameId, 4);
        QCOMPARE(nav.overlayFilter(8).category, QStringLiteral("clips"));
        QCOMPARE(nav.overlayFilter(8).gameId, -1);
        QCOMPARE(nav.overlayFilter(-1).gameId, -1);
    }

    void overlayCategoryBackToAllDropsItsKey()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setOverlayCategory(4, QStringLiteral("favorites"));
        QVERIFY(!cfg.isDefault(NavigationState::overlayKeyFor(4)));
        nav.setOverlayCategory(4, QStringLiteral("all"));
        QVERIFY(cfg.isDefault(NavigationState::overlayKeyFor(4)));
        QCOMPARE(nav.overlayCategory(4), QStringLiteral("all"));
    }

    void overlayCategoryIgnoresAnAbsentGame()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setOverlayCategory(-1, QStringLiteral("clips"));
        QVERIFY(cfg.isDefault(NavigationState::overlayKeyFor(-1)));
    }

    // R10: flushing on hide/quit rewrites the same view without changing it.
    void flushIsIdempotent()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.flush(QStringLiteral("settings"), QStringLiteral("input"),
                  QStringLiteral("clips"), 5);
        const QVariant page = cfg.value(ConfigKeys::UiPage);
        nav.flush(QStringLiteral("settings"), QStringLiteral("input"),
                  QStringLiteral("clips"), 5);
        QCOMPARE(cfg.value(ConfigKeys::UiPage), page);
        QCOMPARE(nav.page(), QStringLiteral("settings"));
        QCOMPARE(nav.settingsCategory(), QStringLiteral("input"));
        QCOMPARE(nav.galleryFilter({ 5 }).gameId, 5);
    }

    // R10: "Restore all defaults" clears the remembered view. The post-update
    // greeting keys are internal.* and survive, so a reset never re-fires the
    // What's New dialog the user already dismissed — the greeting keeps its
    // precedence over whatever page is restored.
    void resetAllClearsNavigationButKeepsTheGreeting()
    {
        ConfigManager cfg(path());
        NavigationState nav(&cfg);
        nav.setPage(QStringLiteral("settings"));
        nav.setGalleryFilter(QStringLiteral("clips"), 5);
        nav.setOverlayCategory(5, QStringLiteral("favorites"));
        cfg.setValue(ConfigKeys::InternalUiWhatsNewSeenVersion, QStringLiteral("0.7.11"));

        QVERIFY(cfg.resetAll());
        QCOMPARE(nav.page(), QStringLiteral("gallery"));
        QCOMPARE(nav.galleryFilter({ 5 }).category, QStringLiteral("all"));
        QCOMPARE(nav.galleryFilter({ 5 }).gameId, -1);
        QCOMPARE(nav.overlayCategory(5), QStringLiteral("all"));
        QCOMPARE(cfg.value(ConfigKeys::InternalUiWhatsNewSeenVersion).toString(),
                 QStringLiteral("0.7.11"));
    }
};

QTEST_MAIN(TestNavigationState)
#include "tst_navigationstate.moc"
