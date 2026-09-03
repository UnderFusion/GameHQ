#include "input/ActionCatalog.h"

#include "localization/NativeText.h"

#include <QCoreApplication>

namespace {

struct PresentationSpec
{
    const char* labelId;
    const char* labelSource;
    const char* descriptionId;
    const char* descriptionSource;
};

const QVector<PresentationSpec>& presentationSpecs();

QVector<ActionCatalog::Action>& buildCatalog()
{
    using Scope = ActionCatalog::Scope;
    static QVector<ActionCatalog::Action> actions = {
        // Global — always active, independent of overlay/desktop/playback state.
        { QStringLiteral("global.screenshot"), Scope::Global, {}, {}, true },
        { QStringLiteral("global.save_replay"), Scope::Global, {}, {}, true },
        { QStringLiteral("global.toggle_overlay"), Scope::Global, {}, {}, true },
        { QStringLiteral("global.toggle_desktop"), Scope::Global, {}, {}, true },

        // Overlay — in-game overlay is visible.
        { QStringLiteral("overlay.navigate_up"), Scope::Overlay, {}, {}, true, true },
        { QStringLiteral("overlay.navigate_down"), Scope::Overlay, {}, {}, true, true },
        { QStringLiteral("overlay.navigate_left"), Scope::Overlay, {}, {}, true, true },
        { QStringLiteral("overlay.navigate_right"), Scope::Overlay, {}, {}, true, true },
        { QStringLiteral("overlay.confirm"), Scope::Overlay, {}, {}, true },
        { QStringLiteral("overlay.back"), Scope::Overlay, {}, {}, false },
        { QStringLiteral("overlay.favorite"), Scope::Overlay, {}, {}, true },
        { QStringLiteral("overlay.menu"), Scope::Overlay, {}, {}, true },
        { QStringLiteral("overlay.sidebar_toggle"), Scope::Overlay, {}, {}, true },
        { QStringLiteral("overlay.game_prev"), Scope::Overlay, {}, {}, true, true },
        { QStringLiteral("overlay.game_next"), Scope::Overlay, {}, {}, true, true },

        // Desktop — the desktop gallery window has real Win32 foreground focus.
        { QStringLiteral("desktop.navigate_up"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.navigate_down"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.navigate_left"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.navigate_right"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.confirm"), Scope::Desktop, {}, {}, true },
        { QStringLiteral("desktop.back"), Scope::Desktop, {}, {}, false },
        { QStringLiteral("desktop.favorite"), Scope::Desktop, {}, {}, true },
        { QStringLiteral("desktop.menu"), Scope::Desktop, {}, {}, true },
        { QStringLiteral("desktop.tab_prev"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.tab_next"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.settings"), Scope::Desktop, {}, {}, true },
        { QStringLiteral("desktop.zoom_out"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.zoom_in"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.scroll_up"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.scroll_down"), Scope::Desktop, {}, {}, true, true },
        { QStringLiteral("desktop.bulk_toggle"), Scope::Desktop, {}, {}, true },

        // Playback — a clip is focused/playing in the overlay or gallery lightbox.
        { QStringLiteral("playback.play_pause"), Scope::Playback, {}, {}, true },
        { QStringLiteral("playback.seek_back"), Scope::Playback, {}, {}, true, true },
        { QStringLiteral("playback.seek_forward"), Scope::Playback, {}, {}, true, true },
        { QStringLiteral("playback.frame_grab"), Scope::Playback, {}, {}, true },
    };
    if (!actions.isEmpty() && actions.first().label.isEmpty()) {
        const auto& specs = presentationSpecs();
        Q_ASSERT(actions.size() == specs.size());
        for (qsizetype index = 0; index < actions.size(); ++index) {
            actions[index].label = QString::fromUtf8(specs[index].labelSource);
            actions[index].description = QString::fromUtf8(specs[index].descriptionSource);
        }
    }
    return actions;
}

const QVector<PresentationSpec>& presentationSpecs()
{
    // Keep this table index-aligned with buildCatalog(). It owns presentation
    // only; stable action IDs, scopes and behavior remain in the catalog.
    static const QVector<PresentationSpec> specs = {
        //: Binding-editor label for the global screenshot action.
        //% "Screenshot"
        {QT_TRID_NOOP("gamehq.action.screenshot.label"), "Screenshot",
         //: Binding-editor description for the global screenshot action.
         //% "Capture a screenshot of the current game."
         QT_TRID_NOOP("gamehq.action.global.screenshot.description"),
         "Capture a screenshot of the current game."},
        //: Binding-editor label for the global replay-save action.
        //% "Save Replay"
        {QT_TRID_NOOP("gamehq.action.global.save_replay.label"), "Save Replay",
         //: Binding-editor description for the global replay-save action.
         //% "Save the rolling replay buffer as a clip."
         QT_TRID_NOOP("gamehq.action.global.save_replay.description"),
         "Save the rolling replay buffer as a clip."},
        //: Binding-editor label for the global overlay-toggle action.
        //% "Toggle Overlay"
        {QT_TRID_NOOP("gamehq.action.global.toggle_overlay.label"), "Toggle Overlay",
         //: Binding-editor description for the global overlay-toggle action.
         //% "Show or hide the in-game overlay."
         QT_TRID_NOOP("gamehq.action.global.toggle_overlay.description"),
         "Show or hide the in-game overlay."},
        //: Binding-editor label for the global desktop-window action.
        //% "Show / Hide GameHQ Window"
        {QT_TRID_NOOP("gamehq.action.global.toggle_desktop.label"),
         "Show / Hide GameHQ Window",
         //: Binding-editor description for the global desktop-window action.
         //% "Bring the GameHQ window to the front with focus, or hide it and return to the game."
         QT_TRID_NOOP("gamehq.action.global.toggle_desktop.description"),
         "Bring the GameHQ window to the front with focus, or hide it and return to the game."},

        //: Binding-editor label for moving up in the overlay.
        //% "Navigate Up"
        {QT_TRID_NOOP("gamehq.action.overlay.navigate_up.label"), "Navigate Up",
         //: Binding-editor description for moving up in the overlay.
         //% "Move selection up in the overlay."
         QT_TRID_NOOP("gamehq.action.overlay.navigate_up.description"),
         "Move selection up in the overlay."},
        //: Binding-editor label for moving down in the overlay.
        //% "Navigate Down"
        {QT_TRID_NOOP("gamehq.action.overlay.navigate_down.label"), "Navigate Down",
         //: Binding-editor description for moving down in the overlay.
         //% "Move selection down in the overlay."
         QT_TRID_NOOP("gamehq.action.overlay.navigate_down.description"),
         "Move selection down in the overlay."},
        //: Binding-editor label for moving left in the overlay.
        //% "Navigate Left"
        {QT_TRID_NOOP("gamehq.action.overlay.navigate_left.label"), "Navigate Left",
         //: Binding-editor description for moving left in the overlay.
         //% "Move selection left in the overlay."
         QT_TRID_NOOP("gamehq.action.overlay.navigate_left.description"),
         "Move selection left in the overlay."},
        //: Binding-editor label for moving right in the overlay.
        //% "Navigate Right"
        {QT_TRID_NOOP("gamehq.action.overlay.navigate_right.label"), "Navigate Right",
         //: Binding-editor description for moving right in the overlay.
         //% "Move selection right in the overlay."
         QT_TRID_NOOP("gamehq.action.overlay.navigate_right.description"),
         "Move selection right in the overlay."},
        //: Binding-editor label for activating an overlay item.
        //% "Confirm"
        {QT_TRID_NOOP("gamehq.action.overlay.confirm.label"), "Confirm",
         //: Binding-editor description for activating an overlay item.
         //% "Activate the selected item in the overlay."
         QT_TRID_NOOP("gamehq.action.overlay.confirm.description"),
         "Activate the selected item in the overlay."},
        //: Binding-editor label for the fixed overlay back action.
        //% "Back"
        {QT_TRID_NOOP("gamehq.action.overlay.back.label"), "Back",
         //: Binding-editor description for the fixed overlay back action.
         //% "Close the current overlay panel."
         QT_TRID_NOOP("gamehq.action.overlay.back.description"),
         "Close the current overlay panel."},
        //: Binding-editor label for toggling an overlay favorite.
        //% "Toggle Favorite"
        {QT_TRID_NOOP("gamehq.action.overlay.favorite.label"), "Toggle Favorite",
         //: Binding-editor description for toggling an overlay favorite.
         //% "Mark or unmark the selected capture as a favorite."
         QT_TRID_NOOP("gamehq.action.overlay.favorite.description"),
         "Mark or unmark the selected capture as a favorite."},
        //: Binding-editor label for opening an overlay item menu.
        //% "Open Menu"
        {QT_TRID_NOOP("gamehq.action.overlay.menu.label"), "Open Menu",
         //: Binding-editor description for opening an overlay item menu.
         //% "Open the action menu for the selected item."
         QT_TRID_NOOP("gamehq.action.overlay.menu.description"),
         "Open the action menu for the selected item."},
        //: Binding-editor label for toggling the overlay sidebar.
        //% "Toggle Sidebar"
        {QT_TRID_NOOP("gamehq.action.overlay.sidebar_toggle.label"), "Toggle Sidebar",
         //: Binding-editor description for toggling the overlay sidebar.
         //% "Show or hide the overlay sidebar."
         QT_TRID_NOOP("gamehq.action.overlay.sidebar_toggle.description"),
         "Show or hide the overlay sidebar."},
        //: Binding-editor label for selecting the previous overlay game.
        //% "Previous Game"
        {QT_TRID_NOOP("gamehq.action.overlay.game_prev.label"), "Previous Game",
         //: Binding-editor description for selecting the previous overlay game.
         //% "Step to the previous game in the sidebar."
         QT_TRID_NOOP("gamehq.action.overlay.game_prev.description"),
         "Step to the previous game in the sidebar."},
        //: Binding-editor label for selecting the next overlay game.
        //% "Next Game"
        {QT_TRID_NOOP("gamehq.action.overlay.game_next.label"), "Next Game",
         //: Binding-editor description for selecting the next overlay game.
         //% "Step to the next game in the sidebar."
         QT_TRID_NOOP("gamehq.action.overlay.game_next.description"),
         "Step to the next game in the sidebar."},

        //: Binding-editor label for moving up in the gallery.
        //% "Navigate Up"
        {QT_TRID_NOOP("gamehq.action.desktop.navigate_up.label"), "Navigate Up",
         //: Binding-editor description for moving up in the gallery.
         //% "Move selection up in the gallery."
         QT_TRID_NOOP("gamehq.action.desktop.navigate_up.description"),
         "Move selection up in the gallery."},
        //: Binding-editor label for moving down in the gallery.
        //% "Navigate Down"
        {QT_TRID_NOOP("gamehq.action.desktop.navigate_down.label"), "Navigate Down",
         //: Binding-editor description for moving down in the gallery.
         //% "Move selection down in the gallery."
         QT_TRID_NOOP("gamehq.action.desktop.navigate_down.description"),
         "Move selection down in the gallery."},
        //: Binding-editor label for moving left in the gallery.
        //% "Navigate Left"
        {QT_TRID_NOOP("gamehq.action.desktop.navigate_left.label"), "Navigate Left",
         //: Binding-editor description for moving left in the gallery.
         //% "Move selection left in the gallery."
         QT_TRID_NOOP("gamehq.action.desktop.navigate_left.description"),
         "Move selection left in the gallery."},
        //: Binding-editor label for moving right in the gallery.
        //% "Navigate Right"
        {QT_TRID_NOOP("gamehq.action.desktop.navigate_right.label"), "Navigate Right",
         //: Binding-editor description for moving right in the gallery.
         //% "Move selection right in the gallery."
         QT_TRID_NOOP("gamehq.action.desktop.navigate_right.description"),
         "Move selection right in the gallery."},
        //: Binding-editor label for activating a gallery item.
        //% "Confirm"
        {QT_TRID_NOOP("gamehq.action.desktop.confirm.label"), "Confirm",
         //: Binding-editor description for activating a gallery item.
         //% "Activate the selected item in the gallery."
         QT_TRID_NOOP("gamehq.action.desktop.confirm.description"),
         "Activate the selected item in the gallery."},
        //: Binding-editor label for the fixed gallery back action.
        //% "Back"
        {QT_TRID_NOOP("gamehq.action.desktop.back.label"), "Back",
         //: Binding-editor description for the fixed gallery back action.
         //% "Close the current gallery panel."
         QT_TRID_NOOP("gamehq.action.desktop.back.description"),
         "Close the current gallery panel."},
        //: Binding-editor label for toggling a gallery favorite.
        //% "Toggle Favorite"
        {QT_TRID_NOOP("gamehq.action.desktop.favorite.label"), "Toggle Favorite",
         //: Binding-editor description for toggling a gallery favorite.
         //% "Mark or unmark the selected capture as a favorite."
         QT_TRID_NOOP("gamehq.action.desktop.favorite.description"),
         "Mark or unmark the selected capture as a favorite."},
        //: Binding-editor label for opening a gallery item menu.
        //% "Open Menu"
        {QT_TRID_NOOP("gamehq.action.desktop.menu.label"), "Open Menu",
         //: Binding-editor description for opening a gallery item menu.
         //% "Open the action menu for the selected item."
         QT_TRID_NOOP("gamehq.action.desktop.menu.description"),
         "Open the action menu for the selected item."},
        //: Binding-editor label for selecting the previous gallery tab.
        //% "Previous Tab"
        {QT_TRID_NOOP("gamehq.action.desktop.tab_prev.label"), "Previous Tab",
         //: Binding-editor description for selecting the previous gallery tab.
         //% "Step to the previous sidebar category."
         QT_TRID_NOOP("gamehq.action.desktop.tab_prev.description"),
         "Step to the previous sidebar category."},
        //: Binding-editor label for selecting the next gallery tab.
        //% "Next Tab"
        {QT_TRID_NOOP("gamehq.action.desktop.tab_next.label"), "Next Tab",
         //: Binding-editor description for selecting the next gallery tab.
         //% "Step to the next sidebar category."
         QT_TRID_NOOP("gamehq.action.desktop.tab_next.description"),
         "Step to the next sidebar category."},
        //: Binding-editor label for opening Settings from the gallery.
        //% "Open Settings"
        {QT_TRID_NOOP("gamehq.action.desktop.settings.label"), "Open Settings",
         //: Binding-editor description for opening Settings from the gallery.
         //% "Open the Settings panel from anywhere in the gallery."
         QT_TRID_NOOP("gamehq.action.desktop.settings.description"),
         "Open the Settings panel from anywhere in the gallery."},
        //: Binding-editor label for making gallery thumbnails smaller.
        //% "Zoom Out"
        {QT_TRID_NOOP("gamehq.action.desktop.zoom_out.label"), "Zoom Out",
         //: Binding-editor description for making gallery thumbnails smaller.
         //% "Make the gallery thumbnails smaller."
         QT_TRID_NOOP("gamehq.action.desktop.zoom_out.description"),
         "Make the gallery thumbnails smaller."},
        //: Binding-editor label for making gallery thumbnails larger.
        //% "Zoom In"
        {QT_TRID_NOOP("gamehq.action.desktop.zoom_in.label"), "Zoom In",
         //: Binding-editor description for making gallery thumbnails larger.
         //% "Make the gallery thumbnails larger."
         QT_TRID_NOOP("gamehq.action.desktop.zoom_in.description"),
         "Make the gallery thumbnails larger."},
        //: Binding-editor label for scrolling the gallery upward.
        //% "Scroll Up"
        {QT_TRID_NOOP("gamehq.action.desktop.scroll_up.label"), "Scroll Up",
         //: Binding-editor description for scrolling the gallery upward.
         //% "Scroll the current view up without moving the selection."
         QT_TRID_NOOP("gamehq.action.desktop.scroll_up.description"),
         "Scroll the current view up without moving the selection."},
        //: Binding-editor label for scrolling the gallery downward.
        //% "Scroll Down"
        {QT_TRID_NOOP("gamehq.action.desktop.scroll_down.label"), "Scroll Down",
         //: Binding-editor description for scrolling the gallery downward.
         //% "Scroll the current view down without moving the selection."
         QT_TRID_NOOP("gamehq.action.desktop.scroll_down.description"),
         "Scroll the current view down without moving the selection."},
        //: Binding-editor label for toggling gallery bulk-selection mode.
        //% "Bulk Select"
        {QT_TRID_NOOP("gamehq.action.desktop.bulk_toggle.label"), "Bulk Select",
         //: Binding-editor description for toggling gallery bulk-selection mode.
         //% "Enter or leave bulk selection mode."
         QT_TRID_NOOP("gamehq.action.desktop.bulk_toggle.description"),
         "Enter or leave bulk selection mode."},

        //: Binding-editor label for toggling playback.
        //% "Play / Pause"
        {QT_TRID_NOOP("gamehq.action.playback.play_pause.label"), "Play / Pause",
         //: Binding-editor description for toggling playback.
         //% "Toggle playback of the focused clip."
         QT_TRID_NOOP("gamehq.action.playback.play_pause.description"),
         "Toggle playback of the focused clip."},
        //: Binding-editor label for seeking backward.
        //% "Seek Back"
        {QT_TRID_NOOP("gamehq.action.playback.seek_back.label"), "Seek Back",
         //: Binding-editor description for seeking backward.
         //% "Step the focused clip backward."
         QT_TRID_NOOP("gamehq.action.playback.seek_back.description"),
         "Step the focused clip backward."},
        //: Binding-editor label for seeking forward.
        //% "Seek Forward"
        {QT_TRID_NOOP("gamehq.action.playback.seek_forward.label"), "Seek Forward",
         //: Binding-editor description for seeking forward.
         //% "Step the focused clip forward."
         QT_TRID_NOOP("gamehq.action.playback.seek_forward.description"),
         "Step the focused clip forward."},
        //: Binding-editor label for saving the current video frame.
        //% "Save Frame"
        {QT_TRID_NOOP("gamehq.action.playback.frame_grab.label"), "Save Frame",
         //: Binding-editor description for saving the current video frame.
         //% "Save the frame currently shown in the focused clip as a screenshot."
         QT_TRID_NOOP("gamehq.action.playback.frame_grab.description"),
         "Save the frame currently shown in the focused clip as a screenshot."},
    };
    return specs;
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
    const auto& specs = presentationSpecs();
    Q_ASSERT(actions.size() == specs.size());
    for (qsizetype index = 0; index < actions.size(); ++index) {
        actions[index].label = NativeText::get(specs[index].labelId,
                                               specs[index].labelSource);
        actions[index].description = NativeText::get(specs[index].descriptionId,
                                                     specs[index].descriptionSource);
    }
}
