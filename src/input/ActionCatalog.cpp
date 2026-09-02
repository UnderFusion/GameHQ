#include "input/ActionCatalog.h"

#include <QCoreApplication>

namespace {

QVector<ActionCatalog::Action>& buildCatalog()
{
    using Scope = ActionCatalog::Scope;
    static QVector<ActionCatalog::Action> actions = {
        // Global — always active, independent of overlay/desktop/playback state.
        { QStringLiteral("global.screenshot"),     Scope::Global,   QStringLiteral("Screenshot"),
          QStringLiteral("Capture a screenshot of the current game."), true },
        { QStringLiteral("global.save_replay"),    Scope::Global,   QStringLiteral("Save Replay"),
          QStringLiteral("Save the rolling replay buffer as a clip."), true },
        { QStringLiteral("global.toggle_overlay"), Scope::Global,   QStringLiteral("Toggle Overlay"),
          QStringLiteral("Show or hide the in-game overlay."), true },
        { QStringLiteral("global.toggle_desktop"), Scope::Global,   QStringLiteral("Show / Hide GameHQ Window"),
          QStringLiteral("Bring the GameHQ window to the front with focus, or hide it and return to the game."), true },

        // Overlay — in-game overlay is visible.
        { QStringLiteral("overlay.navigate_up"),    Scope::Overlay, QStringLiteral("Navigate Up"),
          QStringLiteral("Move selection up in the overlay."), true, true },
        { QStringLiteral("overlay.navigate_down"),  Scope::Overlay, QStringLiteral("Navigate Down"),
          QStringLiteral("Move selection down in the overlay."), true, true },
        { QStringLiteral("overlay.navigate_left"),  Scope::Overlay, QStringLiteral("Navigate Left"),
          QStringLiteral("Move selection left in the overlay."), true, true },
        { QStringLiteral("overlay.navigate_right"), Scope::Overlay, QStringLiteral("Navigate Right"),
          QStringLiteral("Move selection right in the overlay."), true, true },
        { QStringLiteral("overlay.confirm"),        Scope::Overlay, QStringLiteral("Confirm"),
          QStringLiteral("Activate the selected item in the overlay."), true },
        { QStringLiteral("overlay.back"),           Scope::Overlay, QStringLiteral("Back"),
          QStringLiteral("Close the current overlay panel."), false },
        { QStringLiteral("overlay.favorite"),       Scope::Overlay, QStringLiteral("Toggle Favorite"),
          QStringLiteral("Mark or unmark the selected capture as a favorite."), true },
        { QStringLiteral("overlay.menu"),           Scope::Overlay, QStringLiteral("Open Menu"),
          QStringLiteral("Open the action menu for the selected item."), true },
        { QStringLiteral("overlay.sidebar_toggle"), Scope::Overlay, QStringLiteral("Toggle Sidebar"),
          QStringLiteral("Show or hide the overlay sidebar."), true },
        { QStringLiteral("overlay.game_prev"),      Scope::Overlay, QStringLiteral("Previous Game"),
          QStringLiteral("Step to the previous game in the sidebar."), true, true },
        { QStringLiteral("overlay.game_next"),      Scope::Overlay, QStringLiteral("Next Game"),
          QStringLiteral("Step to the next game in the sidebar."), true, true },

        // Desktop — the desktop gallery window has real Win32 foreground focus.
        { QStringLiteral("desktop.navigate_up"),    Scope::Desktop, QStringLiteral("Navigate Up"),
          QStringLiteral("Move selection up in the gallery."), true, true },
        { QStringLiteral("desktop.navigate_down"),  Scope::Desktop, QStringLiteral("Navigate Down"),
          QStringLiteral("Move selection down in the gallery."), true, true },
        { QStringLiteral("desktop.navigate_left"),  Scope::Desktop, QStringLiteral("Navigate Left"),
          QStringLiteral("Move selection left in the gallery."), true, true },
        { QStringLiteral("desktop.navigate_right"), Scope::Desktop, QStringLiteral("Navigate Right"),
          QStringLiteral("Move selection right in the gallery."), true, true },
        { QStringLiteral("desktop.confirm"),        Scope::Desktop, QStringLiteral("Confirm"),
          QStringLiteral("Activate the selected item in the gallery."), true },
        { QStringLiteral("desktop.back"),           Scope::Desktop, QStringLiteral("Back"),
          QStringLiteral("Close the current gallery panel."), false },
        { QStringLiteral("desktop.favorite"),       Scope::Desktop, QStringLiteral("Toggle Favorite"),
          QStringLiteral("Mark or unmark the selected capture as a favorite."), true },
        { QStringLiteral("desktop.menu"),           Scope::Desktop, QStringLiteral("Open Menu"),
          QStringLiteral("Open the action menu for the selected item."), true },
        { QStringLiteral("desktop.tab_prev"),       Scope::Desktop, QStringLiteral("Previous Tab"),
          QStringLiteral("Step to the previous sidebar category."), true, true },
        { QStringLiteral("desktop.tab_next"),       Scope::Desktop, QStringLiteral("Next Tab"),
          QStringLiteral("Step to the next sidebar category."), true, true },
        { QStringLiteral("desktop.settings"),       Scope::Desktop, QStringLiteral("Open Settings"),
          QStringLiteral("Open the Settings panel from anywhere in the gallery."), true },
        { QStringLiteral("desktop.zoom_out"),       Scope::Desktop, QStringLiteral("Zoom Out"),
          QStringLiteral("Make the gallery thumbnails smaller."), true, true },
        { QStringLiteral("desktop.zoom_in"),        Scope::Desktop, QStringLiteral("Zoom In"),
          QStringLiteral("Make the gallery thumbnails larger."), true, true },
        { QStringLiteral("desktop.scroll_up"),      Scope::Desktop, QStringLiteral("Scroll Up"),
          QStringLiteral("Scroll the current view up without moving the selection."), true, true },
        { QStringLiteral("desktop.scroll_down"),    Scope::Desktop, QStringLiteral("Scroll Down"),
          QStringLiteral("Scroll the current view down without moving the selection."), true, true },
        { QStringLiteral("desktop.bulk_toggle"),    Scope::Desktop, QStringLiteral("Bulk Select"),
          QStringLiteral("Enter or leave bulk selection mode."), true },

        // Playback — a clip is focused/playing in the overlay or gallery lightbox.
        { QStringLiteral("playback.play_pause"),  Scope::Playback, QStringLiteral("Play / Pause"),
          QStringLiteral("Toggle playback of the focused clip."), true },
        { QStringLiteral("playback.seek_back"),   Scope::Playback, QStringLiteral("Seek Back"),
          QStringLiteral("Step the focused clip backward."), true, true },
        { QStringLiteral("playback.seek_forward"),Scope::Playback, QStringLiteral("Seek Forward"),
          QStringLiteral("Step the focused clip forward."), true, true },
        { QStringLiteral("playback.frame_grab"), Scope::Playback, QStringLiteral("Save Frame"),
          QStringLiteral("Save the frame currently shown in the focused clip as a screenshot."), true },
    };
    return actions;
}

} // namespace

const QVector<ActionCatalog::Action>& ActionCatalog::all()
{
    return buildCatalog();
}

const ActionCatalog::Action* ActionCatalog::find(const QString& id)
{
    const auto& actions = all();
    for (const auto& action : actions) {
        if (action.id == id)
            return &action;
    }
    return nullptr;
}

void ActionCatalog::retranslate()
{
    auto& actions = buildCatalog();
    for (Action& action : actions) {
        if (action.id == QLatin1String("global.screenshot")) {
            //% "Screenshot"
            action.label = qtTrId("gamehq.action.screenshot.label");
            return;
        }
    }
}
