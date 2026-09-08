#pragma once
#include <QList>
#include <QString>
#include <QStringList>

class ConfigManager;

// Last-view memory: which page, settings category, gallery filter and per-game
// overlay category the app reopens on. The rules it implements are the ones
// agreed in docs/product-spec.md ("Navigation memory"):
//
//   * a normal launch restores the page, the settings category and the gallery
//     filter; Help is never restored and collapses to the gallery;
//   * raising the window (tray, summon, --open-gallery) never rewrites the
//     saved view — only the page changes, and only when the caller says so;
//   * a saved game id is validated against the games that still exist before it
//     is applied, otherwise the filter falls back to "every game";
//   * the overlay always follows the foreground game and only remembers the
//     category it was left on, per game;
//   * an explicit one-shot jump (the "clip saved" toast, later) passes
//     Persist::No so it can navigate without overwriting the saved view.
//
// Everything here is pure logic over ConfigManager, which is what keeps it
// testable without the database, the QML engine or a window.
class NavigationState
{
public:
    explicit NavigationState(ConfigManager* config);

    // Whether a change is written to config.json. Persist::No is the plumbing
    // for transient navigation: the caller still moves, the memory does not.
    enum class Persist { No, Yes };

    struct GalleryFilter
    {
        QString category;
        int gameId = -1;
    };

    // Rewrites a legacy numeric ui.settings_category (releases up to 0.7.10
    // stored the sidebar index) as its stable key, in place and exactly once.
    // Safe to call on every start: a value that is already a key is left alone.
    void migrateLegacyKeys();

    QString page() const;
    void setPage(const QString& page, Persist persist = Persist::Yes);

    QString settingsCategory() const;
    void setSettingsCategory(const QString& category, Persist persist = Persist::Yes);

    // Validated against the games that currently exist. An id that is gone
    // takes the whole filter back to "every game", because a category narrowed
    // to a deleted game would open on an empty gallery with no visible reason.
    GalleryFilter galleryFilter(const QList<int>& knownGameIds) const;
    void setGalleryFilter(const QString& category, int gameId, Persist persist = Persist::Yes);

    // "all" when this game has no remembered category. gameId < 0 means no
    // game is in the foreground, which has nothing to remember.
    QString overlayCategory(int gameId) const;
    // The remembered category resolved into a filter, mirroring
    // SidebarCategories.resolveFilter(): only "all" and "favorites" stay
    // narrowed to the foreground game, every other category is library-wide.
    GalleryFilter overlayFilter(int gameId) const;
    void setOverlayCategory(int gameId, const QString& category, Persist persist = Persist::Yes);

    // Writes the current view again without changing it. Called on hide and on
    // quit so a view that was only ever set by a restore is still durable;
    // ConfigManager drops writes that change nothing, so this is idempotent.
    void flush(const QString& page, const QString& settingsCategory,
               const QString& galleryCategory, int galleryGameId);

    // The vocabularies, in the order the UI presents them. Public so the QML
    // sidebars and the tests share one source of truth for the spellings.
    static const QStringList& pageKeys();
    static const QStringList& settingsCategoryKeys();
    static const QStringList& galleryCategoryKeys();
    static QString overlayKeyFor(int gameId);

private:
    ConfigManager* m_config;
};
