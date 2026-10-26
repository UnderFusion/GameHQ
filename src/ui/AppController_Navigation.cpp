// AppController: gallery filter, page and window navigation state.
#include "ui/AppController.h"
#include "config/ConfigKeys.h"
#include "config/ConfigManager.h"
#include "core/WindowPlacement.h"
#include "storage/CaptureDatabase.h"
#include "ui/GalleryModel.h"

#include <QDebug>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

void AppController::applyFilter(const QString& category, int gameId,
                                NavigationState::Persist persist)
{
    m_category = category;
    m_gameId = gameId;
    m_gallery->setFilter(m_category, m_gameId);
    m_navigation->setGalleryFilter(m_category, m_gameId, persist);
    if (persist == NavigationState::Persist::Yes)
        scheduleNavigationSave();
    emit filterChanged();
}

void AppController::setCategory(const QString& category)
{
    applyFilter(category, -1, NavigationState::Persist::Yes);
}

void AppController::setGame(int gameId)
{
    applyFilter(QStringLiteral("all"), gameId, NavigationState::Persist::Yes);
}

void AppController::setGameCategory(const QString& category, int gameId)
{
    applyFilter(category, gameId, NavigationState::Persist::Yes);
}

void AppController::setGameCategoryTransient(const QString& category, int gameId)
{
    applyFilter(category, gameId, NavigationState::Persist::No);
}

QList<int> AppController::knownGameIds() const
{
    QList<int> ids;
    const auto entries = m_db->listGames();
    ids.reserve(entries.size());
    for (const GameEntry& g : entries)
        ids.append(g.id);
    return ids;
}

// The saved filter can only be trusted once the first scan has published the
// library: restoring it earlier would silently drop a valid game id just
// because its row was not read yet.
void AppController::restoreGalleryFilter()
{
    const NavigationState::GalleryFilter saved = m_navigation->galleryFilter(knownGameIds());
    qInfo() << "Navigation: restored view — page" << m_navigation->page()
            << "settings category" << m_navigation->settingsCategory()
            << "gallery filter" << saved.category << "game" << saved.gameId;
    if (saved.category == m_category && saved.gameId == m_gameId)
        return;
    applyFilter(saved.category, saved.gameId, NavigationState::Persist::No);
}

QString AppController::restoredPage() const
{
    return m_navigation->page();
}

QRect AppController::restoredWindowGeometry() const
{
    WindowPlacement::Saved saved;
    saved.x = m_config->value(ConfigKeys::UiWindowX).toInt();
    saved.y = m_config->value(ConfigKeys::UiWindowY).toInt();
    saved.width = m_config->value(ConfigKeys::UiWindowWidth).toInt();
    saved.height = m_config->value(ConfigKeys::UiWindowHeight).toInt();

    // Per-screen work areas, not the virtual bounding box: the bounding box of
    // an L-shaped desktop covers gaps where no monitor exists, and a window
    // "restored" into a gap is invisible.
    QList<QRect> screens;
    const auto available = QGuiApplication::screens();
    screens.reserve(available.size());
    for (const QScreen* screen : available)
        screens.append(screen->availableGeometry());
    const QScreen* primary = QGuiApplication::primaryScreen();
    const QRect fallback = primary ? primary->availableGeometry() : QRect();

    const QRect restored = WindowPlacement::restore(saved, screens, fallback);
    qInfo() << "Window: restored geometry" << restored << "from saved" << saved.x << saved.y
            << saved.width << "x" << saved.height << "screens" << screens;
    return restored;
}

void AppController::setPage(const QString& page)
{
    m_navigation->setPage(page);
    scheduleNavigationSave();
}

void AppController::scheduleNavigationSave()
{
    m_navigationSaveTimer->start();
}

QString AppController::settingsCategory() const
{
    return m_navigation->settingsCategory();
}

void AppController::setSettingsCategory(const QString& category)
{
    m_navigation->setSettingsCategory(category);
    scheduleNavigationSave();
}

QString AppController::overlayCategory(int gameId) const
{
    return m_navigation->overlayCategory(gameId);
}

void AppController::setOverlayCategory(int gameId, const QString& category)
{
    m_navigation->setOverlayCategory(gameId, category);
    scheduleNavigationSave();
}

void AppController::persistNavigationState(const QString& page)
{
    m_navigation->flush(page, m_navigation->settingsCategory(), m_category, m_gameId);
    m_navigationSaveTimer->stop();
    m_config->save();
}
