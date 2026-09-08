#include "ui/NavigationState.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"

#include <QVariant>

NavigationState::NavigationState(ConfigManager* config)
    : m_config(config)
{
}

const QStringList& NavigationState::pageKeys()
{
    // "help" is deliberately absent: Help is a dialog the user dismisses, not a
    // place to reopen on, so a stored "help" resolves back to the gallery.
    static const QStringList kPages = { QStringLiteral("gallery"), QStringLiteral("settings") };
    return kPages;
}

const QStringList& NavigationState::settingsCategoryKeys()
{
    // Same order as SettingsView.qml's `categories`, which is what makes the
    // one-time index migration below a plain lookup.
    static const QStringList kCategories = {
        QStringLiteral("general"),
        QStringLiteral("capture"),
        QStringLiteral("replay"),
        QStringLiteral("input"),
        QStringLiteral("library"),
        QStringLiteral("notifications_sound"),
        QStringLiteral("advanced"),
        QStringLiteral("about"),
    };
    return kCategories;
}

const QStringList& NavigationState::galleryCategoryKeys()
{
    // The library-wide categories from SidebarCategories.js. The two
    // current-game rows are not stored: they are this same set narrowed to a
    // game id, which the filter's gameId half already carries.
    static const QStringList kCategories = {
        QStringLiteral("all"),
        QStringLiteral("recent"),
        QStringLiteral("favorites"),
        QStringLiteral("screenshots"),
        QStringLiteral("clips"),
    };
    return kCategories;
}

QString NavigationState::overlayKeyFor(int gameId)
{
    return QString(ConfigKeys::UiOverlayFilterPrefix) + QString::number(gameId);
}

void NavigationState::migrateLegacyKeys()
{
    if (!m_config->hasExplicitValue(ConfigKeys::UiSettingsCategory))
        return;
    const QVariant stored = m_config->value(ConfigKeys::UiSettingsCategory);
    if (stored.typeId() == QMetaType::QString)
        return;   // already a key, or a shape we will not guess at

    bool numeric = false;
    const int index = stored.toInt(&numeric);
    const QStringList& keys = settingsCategoryKeys();
    const QString migrated = (numeric && index >= 0 && index < keys.size())
        ? keys.at(index) : keys.first();
    m_config->setValue(ConfigKeys::UiSettingsCategory, migrated);
}

QString NavigationState::page() const
{
    const QString stored = m_config->value(ConfigKeys::UiPage).toString();
    return pageKeys().contains(stored) ? stored : pageKeys().first();
}

void NavigationState::setPage(const QString& page, Persist persist)
{
    if (persist != Persist::Yes)
        return;
    m_config->setValue(ConfigKeys::UiPage,
                       pageKeys().contains(page) ? page : pageKeys().first());
}

QString NavigationState::settingsCategory() const
{
    const QString stored = m_config->value(ConfigKeys::UiSettingsCategory).toString();
    return settingsCategoryKeys().contains(stored) ? stored : settingsCategoryKeys().first();
}

void NavigationState::setSettingsCategory(const QString& category, Persist persist)
{
    if (persist != Persist::Yes)
        return;
    m_config->setValue(ConfigKeys::UiSettingsCategory,
                       settingsCategoryKeys().contains(category) ? category
                                                                 : settingsCategoryKeys().first());
}

NavigationState::GalleryFilter NavigationState::galleryFilter(const QList<int>& knownGameIds) const
{
    GalleryFilter filter;
    const QString category = m_config->value(ConfigKeys::UiGalleryFilterCategory).toString();
    filter.category = galleryCategoryKeys().contains(category) ? category
                                                               : galleryCategoryKeys().first();
    const int gameId = m_config->value(ConfigKeys::UiGalleryFilterGame).toInt();
    if (gameId < 0)
        return filter;
    if (!knownGameIds.contains(gameId))
        return { galleryCategoryKeys().first(), -1 };   // the game is gone: back to the whole library
    filter.gameId = gameId;
    return filter;
}

void NavigationState::setGalleryFilter(const QString& category, int gameId, Persist persist)
{
    if (persist != Persist::Yes)
        return;
    m_config->setValue(ConfigKeys::UiGalleryFilterCategory,
                       galleryCategoryKeys().contains(category) ? category
                                                                : galleryCategoryKeys().first());
    m_config->setValue(ConfigKeys::UiGalleryFilterGame, gameId < 0 ? -1 : gameId);
}

QString NavigationState::overlayCategory(int gameId) const
{
    if (gameId < 0)
        return galleryCategoryKeys().first();
    const QString stored = m_config->value(overlayKeyFor(gameId)).toString();
    return galleryCategoryKeys().contains(stored) ? stored : galleryCategoryKeys().first();
}

NavigationState::GalleryFilter NavigationState::overlayFilter(int gameId) const
{
    GalleryFilter filter;
    filter.category = overlayCategory(gameId);
    if (gameId >= 0
        && (filter.category == QStringLiteral("all") || filter.category == QStringLiteral("favorites")))
        filter.gameId = gameId;
    return filter;
}

void NavigationState::setOverlayCategory(int gameId, const QString& category, Persist persist)
{
    if (persist != Persist::Yes || gameId < 0)
        return;
    const QString key = overlayKeyFor(gameId);
    const QString value = galleryCategoryKeys().contains(category) ? category
                                                                   : galleryCategoryKeys().first();
    // "all" is the implicit state for every game, so a game that is back on it
    // drops its key instead of leaving one entry per game ever visited behind.
    if (value == galleryCategoryKeys().first())
        m_config->resetValue(key);
    else
        m_config->setValue(key, value);
}

void NavigationState::flush(const QString& page, const QString& settingsCategory,
                            const QString& galleryCategory, int galleryGameId)
{
    setPage(page);
    setSettingsCategory(settingsCategory);
    setGalleryFilter(galleryCategory, galleryGameId);
}
