import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Window
import GameHQ
import "components"
import "helpers/SidebarCategories.js" as SidebarCategories

// Desktop gallery window: dark gradient background,
// thin display type, sidebar → grid → preview.
ApplicationWindow {
    id: window
    visible: !app.startMinimized
    width: app ? app.config("ui.window_width", 1280) : 1280
    height: app ? app.config("ui.window_height", 760) : 760
    x: {
        var sx = app ? app.config("ui.window_x", -1) : -1
        return sx >= 0 ? sx : Math.max(0, Math.round((Screen.width - width) / 2))
    }
    y: {
        var sy = app ? app.config("ui.window_y", -1) : -1
        return sy >= 0 ? sy : Math.max(0, Math.round((Screen.height - height) / 2))
    }
    minimumWidth: 1024
    minimumHeight: 640
    title: Brand.name
    // ThemeBackdrop paints the window; this stays as the fill behind it so a
    // resize never flashes the default white.
    color: Theme.bg0

    ThemeBackdrop {
        anchors.fill: parent
        z: -1
    }

    // Debounce timer for geometry persistence (avoid writing config.json on
    // every pixel of a drag). Fires 500ms after the last resize/move.
    Timer {
        id: persistTimer
        interval: 500
        repeat: false
        onTriggered: window.doPersistGeometry()
    }

    BulkSelection {
        id: bulkSelection
        galleryModel: app.gallery
    }

    onWidthChanged: window.persistGeometry()
    onHeightChanged: window.persistGeometry()
    onXChanged: window.persistGeometry()
    onYChanged: window.persistGeometry()

    property bool settingsOpen: false
    property bool helpOpen: false
    property string pendingPostUpdateVersion: app
        ? app.config("internal.updates.pending_post_update_version", "") : ""
    property string whatsNewSeenVersion: app
        ? app.config("internal.ui.whats_new_seen_version", "") : ""

    function maybeShowPostUpdateGreeting() {
        if (!window.visible || !window.active || lightbox.visible
                || aboutDialog.visible || helpDialog.visible
                || pendingPostUpdateVersion === "")
            return
        if (app.config("internal.ui.whats_new_seen_version", "") === pendingPostUpdateVersion) {
            app.setConfig("internal.updates.pending_post_update_version", "")
            pendingPostUpdateVersion = ""
            return
        }
        window.openAbout(true)
    }

    function acknowledgePostUpdateGreeting() {
        app.setConfig("internal.ui.whats_new_seen_version", pendingPostUpdateVersion)
        app.setConfig("internal.updates.pending_post_update_version", "")
        pendingPostUpdateVersion = ""
    }

    function aboutSidebarIndex() {
        return window.sidebarCategories.length + app.games.length + 2
    }

    function openAbout(asPostUpdateGreeting) {
        if (lightbox.visible || helpDialog.visible)
            return
        window.menuOpen = false
        window.sidebarFocused = false
        aboutDialog.open(asPostUpdateGreeting)
        sounds.play("nav_tick")
    }

    function finishAbout() {
        app.setConfig("internal.ui.whats_new_seen_version", app.version)
        if (pendingPostUpdateVersion !== "")
            window.acknowledgePostUpdateGreeting()
        Qt.callLater(window.focusGalleryOrSidebar)
    }

    function openHelp() {
        if (lightbox.visible || aboutDialog.visible)
            return
        window.menuOpen = false
        window.settingsOpen = false
        window.helpOpen = true
        window.sidebarFocused = false
        helpDialog.open()
        sounds.play("nav_tick")
    }

    function finishHelp() {
        window.helpOpen = false
        window.focusGalleryOrSidebar()
    }

    function openAboutSettings() {
        aboutDialog.close()
        window.helpOpen = false
        window.settingsOpen = true
        window.sidebarFocused = false
        settingsView.selectCategory(settingsView.categories.length - 1, true)
    }

    Connections {
        target: app
        function onConfigChanged(key, value) {
            if (key === "internal.updates.pending_post_update_version") {
                window.pendingPostUpdateVersion = value
                window.maybeShowPostUpdateGreeting()
            } else if (key === "internal.ui.whats_new_seen_version") {
                window.whatsNewSeenVersion = value
            }
        }
    }

    // DualSense support (0.3/0.6 extended to the desktop window): the grid
    // never row-wraps — LEFT at the leftmost column enters the sidebar (left
    // panel), RIGHT at the row edge clamps, and RIGHT from the sidebar
    // returns to the grid. L1/R1 still jump between the two panels as a
    // shortcut (kept for now; may be repurposed later). Inside either region
    // D-pad/stick (or arrow keys) drives the cursor, Up/Down auto-repeats
    // while held.
    // Cross opens the lightbox (grid) or selects the row (sidebar), Circle
    // backs out, Triangle favorites, Square opens the action menu.
    // Pad input only reaches here while this window has real OS focus (see
    // onActiveChanged below) — never steals the pad from a focused game.
    readonly property var sidebarCategoryLabels: ({
        //% "All"
        "all": qsTrId("gamehq.navigation.category.all"),
        //% "Recent"
        "recent": qsTrId("gamehq.navigation.category.recent"),
        //% "Favorites"
        "favorites": qsTrId("gamehq.navigation.category.favorites"),
        //% "Screenshots"
        "screenshots": qsTrId("gamehq.navigation.category.screenshots"),
        //% "Clips"
        "clips": qsTrId("gamehq.navigation.category.clips"),
        //% "Game"
        "game": qsTrId("gamehq.navigation.category.game"),
        //% "Game favorites"
        "game_favorites": qsTrId("gamehq.navigation.category.game_favorites")
    })
    property var sidebarCategories: SidebarCategories.categories(app.currentGameAvailable)
        .map(function (category) {
            return {
                key: category.key,
                label: window.sidebarCategoryLabels[category.key],
                glyph: category.glyph
            }
        })
    property bool menuOpen: false
    property int menuIndex: 0
    property bool bulkMode: false
    // Tracks which input source was used last so the footer hint bar can
    // render matching labels (keyboard glyphs vs DualSense button names).
    // Flipped to true on any pad input, false on any key press.
    property bool usingGamepad: false
    // A separate top-level window (currently the desktop lightbox) can emit
    // its close signal before Windows has activated this window again. Keep
    // the restore request alive until activation so QML focus is not assigned
    // to an inactive window and silently lost.
    property bool controllerFocusRestorePending: false
    // Sidebar focus region: when true the "cursor" has left the grid (L1 from
    // the pad — the only entry path) and now lives in the sidebar. UP/DOWN
    // walks the flat sidebar list (categories → games → Settings → Help → About),
    // R1 (or activating an entry, or Esc) returns focus to the grid.
    property bool sidebarFocused: false
    // Flat index into the sidebar list while sidebarFocused. Layout:
    //   0 .. catCount-1                                  → categories
    //   catCount .. catCount+gameCount-1                 → games
    //   catCount+gameCount                               → Settings
    //   catCount+gameCount+1                             → Help
    //   catCount+gameCount+2                             → About / What's New
    property int sidebarHoverIndex: 0

    // Gallery zoom target — the *ideal* tile size in px (160 → 480). The actual
    // cellWidth is grid.width / columns so tiles always fill the full width with
    // no right-side gap. +/- step to the exact next column boundary (no dead
    // clicks), the slider maps continuously. Persisted to config.json under
    // "ui.zoom_level" and restored on startup.
    property int zoomLevel: 280

    // Min/max columns for a given width, derived from the 160/480 tile limits.
    function minColumns(w) {
        if (w <= 0) return 1
        return Math.max(1, Math.floor(w / 480))
    }
    function maxColumns(w) {
        if (w <= 0) return 1
        return Math.max(1, Math.floor(w / 160))
    }

    // Number of columns the grid shows for the given width + current zoom target.
    function gridColumns(w) {
        if (w <= 0)
            return 1
        return Math.max(1, Math.round(w / window.zoomLevel))
    }

    // Zoom IN = bigger tiles = one fewer column. Steps to the exact zoomLevel
    // that yields (curCols − 1) so every click is guaranteed to change the grid.
    function zoomIn() {
        var w = grid.width
        if (w <= 0) return
        var cur = gridColumns(w)
        if (cur <= minColumns(w)) return
        window.zoomLevel = Math.round(w / (cur - 1))
        sounds.play("nav_tick")
    }
    // Zoom OUT = smaller tiles = one more column. Same boundary-stepping logic.
    function zoomOut() {
        var w = grid.width
        if (w <= 0) return
        var cur = gridColumns(w)
        if (cur >= maxColumns(w)) return
        window.zoomLevel = Math.round(w / (cur + 1))
        sounds.play("nav_tick")
    }

    // Debounced persist of window geometry + zoom to config.json so the app
    // remembers its size/position across restarts.
    function persistGeometry() {
        persistTimer.restart()
    }
    function doPersistGeometry() {
        app.setConfig("ui.window_width", window.width)
        app.setConfig("ui.window_height", window.height)
        app.setConfig("ui.window_x", window.x)
        app.setConfig("ui.window_y", window.y)
        app.setConfig("ui.zoom_level", window.zoomLevel)
    }

    onActiveChanged: {
        input.setDesktopFocused(window.active || lightbox.active)
        if (window.active) {
            Qt.callLater(window.finishControllerFocusRestore)
            Qt.callLater(window.maybeShowPostUpdateGreeting)
        }
    }
    // OS-level minimize (title bar button or taskbar): when enabled, drop
    // straight to the tray instead of leaving a taskbar entry. showWindow()
    // (tray click / gallery open) restores to the normal windowed state.
    onVisibilityChanged: function(visibility) {
        if (visibility === Window.Minimized && app.config("tray.minimize_to_tray", false))
            window.hide()
        if (visibility === Window.Windowed || visibility === Window.Maximized)
            window.maybeShowPostUpdateGreeting()
    }
    onClosing: function(close) {
        window.doPersistGeometry()
        if (app.config("tray.close_to_tray", true)) {
            close.accepted = false
            window.hide()
        } else {
            close.accepted = true
            app.quitApplication()
        }
    }
    Component.onCompleted: {
        input.setDesktopFocused(window.active)
        // Restore saved zoom level (if any) from config.json.
        var savedZoom = app.config("ui.zoom_level", 0)
        if (savedZoom >= 160 && savedZoom <= 480)
            window.zoomLevel = savedZoom
        Qt.callLater(window.focusGalleryOrSidebar)
        Qt.callLater(window.maybeShowPostUpdateGreeting)
    }
    onZoomLevelChanged: window.persistGeometry()

    function currentTabIndex() {
        if (app.gameId >= 0) {
            if (app.currentGameAvailable && app.gameId === app.currentGameId) {
                const currentGameKey = app.category === "favorites" ? "game_favorites" : "game"
                const currentGameIdx = window.sidebarCategories.findIndex(c => c.key === currentGameKey)
                return currentGameIdx >= 0 ? currentGameIdx : 0
            }
            const idx = app.games.findIndex(g => g.id === app.gameId)
            return idx >= 0 ? window.sidebarCategories.length + idx : 0
        }
        const idx = window.sidebarCategories.findIndex(c => c.key === app.category)
        return idx >= 0 ? idx : 0
    }

    // The selection algebra lives in BulkSelection.qml; these stay as the window
    // API every caller (grid, header, pad handlers) already binds to.
    function bulkCount() {
        return bulkSelection.count()
    }

    function bulkIsChecked(path) {
        return bulkSelection.isChecked(path)
    }

    function bulkAllSelected() {
        return bulkSelection.allSelected()
    }

    function bulkToggle(index, extendRange) {
        if (bulkSelection.toggle(index, extendRange))
            sounds.play("nav_tick")
    }

    function bulkClear() {
        bulkSelection.clear()
    }

    function bulkEnter() {
        if (grid.count === 0)
            return
        window.menuOpen = false
        window.sidebarFocused = false
        window.bulkMode = true
        window.bulkClear()
        grid.forceActiveFocus()
    }

    function bulkExit() {
        window.bulkMode = false
        window.bulkClear()
        window.focusGalleryOrSidebar()
    }

    // Settings from a button rather than the sidebar row: same end state the
    // sidebar produces, minus the sidebar cursor.
    function openSettings() {
        if (window.settingsOpen)
            return
        if (window.bulkMode)
            window.bulkExit()
        window.menuOpen = false
        window.helpOpen = false
        window.settingsOpen = true
        window.sidebarFocused = false
        sounds.play("nav_tick")
    }

    function padBulkToggle() {
        if (window.bulkMode)
            window.bulkExit()
        else
            window.bulkEnter()
        sounds.play("nav_tick")
    }

    function bulkSelectAll() {
        sounds.play(bulkSelection.selectAll() ? "nav_tick" : "favorite")
    }

    function bulkRows() {
        return bulkSelection.rows()
    }

    function bulkAskDelete() {
        var n = window.bulkCount()
        if (n === 0)
            return
        bulkDeleteDialog.message = n + " capture" + (n === 1 ? "" : "s")
            + " will be permanently deleted.\nThis cannot be undone."
        bulkDeleteDialog.open()
    }

    function bulkConfirmDelete() {
        var rows = window.bulkRows()
        if (rows.length === 0) {
            window.bulkExit()
            return
        }
        sounds.play("confirm")
        app.deleteCaptures(rows)
        window.bulkExit()
    }

    // ───────────────── Flat sidebar list helpers (sidebarFocused mode) ─────────────────
    function sidebarFlatCount() {
        return window.sidebarCategories.length + app.games.length + 3  // +Settings +Help +About
    }

    function refreshSidebarHoverIndex() {
        // Snap the cursor to the row that's currently active so entering the
        // sidebar always lands on the row the user is already looking at.
        if (window.settingsOpen) {
            window.sidebarHoverIndex = window.sidebarCategories.length + app.games.length
        } else if (window.helpOpen) {
            window.sidebarHoverIndex = window.sidebarCategories.length + app.games.length + 1
        } else {
            window.sidebarHoverIndex = window.currentTabIndex()
        }
    }

    function focusGalleryOrSidebar() {
        if (aboutDialog.visible || helpDialog.visible || lightbox.visible)
            return
        // Settings replaces the gallery surface. Focusing the hidden grid here
        // wins the same callLater race as SettingsView.activate() and leaves no
        // visible controller cursor after opening or closing a modal.
        if (window.settingsOpen) {
            window.sidebarFocused = false
            settingsView.restoreFocus()
            return
        }
        // An empty gallery has nothing meaningful to focus on. Keep the real
        // key target on the grid (it owns the desktop key handlers), but put
        // the visible controller cursor in the sidebar so the first D-pad
        // movement gives immediate feedback.
        if (!window.settingsOpen && !window.helpOpen && grid.count === 0) {
            window.sidebarFocused = true
            window.refreshSidebarHoverIndex()
        } else {
            window.sidebarFocused = false
        }
        grid.forceActiveFocus()
    }

    function requestControllerFocusRestore() {
        window.controllerFocusRestorePending = true
        app.requestDesktopFocus()
        Qt.callLater(window.finishControllerFocusRestore)
    }

    function finishControllerFocusRestore() {
        if (!window.controllerFocusRestorePending || !window.active)
            return
        window.controllerFocusRestorePending = false
        window.focusGalleryOrSidebar()
    }

    function activateSidebarRow(i) {
        const catCount = window.sidebarCategories.length
        const gameCount = app.games.length
        if (i < catCount) {
            window.settingsOpen = false; window.helpOpen = false
            const f = SidebarCategories.resolveFilter(window.sidebarCategories[i].key,
                                                      app.currentGameId)
            app.setGameCategory(f.category, f.gameId)
        } else if (i < catCount + gameCount) {
            window.settingsOpen = false; window.helpOpen = false
            app.setGame(app.games[i - catCount].id)
        } else if (i === catCount + gameCount) {
            window.helpOpen = false; window.settingsOpen = true
        } else if (i === catCount + gameCount + 1) {
            window.openHelp()
            return
        } else {  // About / What's New is a modal over the current page.
            window.openAbout(false)
            return
        }
        sounds.play("nav_tick")
        // Gallery pages return to the grid only when it has captures; an empty
        // result keeps the visible controller cursor in the sidebar.
        Qt.callLater(window.focusGalleryOrSidebar)
    }

    function sidebarStepVertical(direction) {
        const total = window.sidebarFlatCount()
        if (total <= 0)
            return
        window.sidebarHoverIndex = (window.sidebarHoverIndex + direction + total) % total
        sounds.play("nav_tick")
    }

    // Column count of the visible grid — used to detect column edges so the
    // grid row-wraps (rightmost → next row leftmost, leftmost → prev row rightmost).
    function gridColumnCount() {
        return window.gridColumns(grid.width)
    }

    function padTabStep(direction) {
        if (helpDialog.visible)
            return
        if (aboutDialog.visible) {
            aboutDialog.padVertical(direction)
            return
        }
        if (window.settingsOpen) {
            settingsView.padCategoryStep(direction)
            return
        }
        if (window.menuOpen || window.bulkMode || window.helpOpen)
            return
        // L1/R1 no longer cycles tabs inside the sidebar — instead it jumps
        // focus between the two panels: L1 → left panel (sidebar, controller
        // driven via D-pad ↑↓), R1 → right panel (thumbnail grid, works as
        // before). Up/Down already does the right thing per region (see
        // padNavigateVertical), so this just flips which region is "active".
        if (direction < 0) {
            // L1: enter the sidebar (left panel). No-op if already there.
            if (window.sidebarFocused)
                return
            window.sidebarFocused = true
            window.refreshSidebarHoverIndex()
        } else {
            // R1: enter the thumbnail grid (right panel). No-op if already there.
            if (grid.count === 0) {
                window.focusGalleryOrSidebar()
                return
            }
            if (!window.sidebarFocused)
                return
            window.sidebarFocused = false
            grid.forceActiveFocus()
        }
        sounds.play("nav_tick")
    }

    function padNavigate(direction) {
        if (helpDialog.visible)
            return
        if (aboutDialog.visible) {
            aboutDialog.padStep(direction)
            return
        }
        // Settings is three panels — sidebar │ categories │ options — and
        // Left/Right walks between them. Right off the sidebar enters the
        // categories; Left past the categories returns to the sidebar.
        if (window.settingsOpen) {
            if (window.sidebarFocused) {
                if (direction > 0) {
                    window.sidebarFocused = false
                    settingsView.enterPanel(settingsView.panelCategories)
                }
                return
            }
            if (!settingsView.padPanelStep(direction)) {
                window.sidebarFocused = true
                window.refreshSidebarHoverIndex()
                sounds.play("nav_tick")
            }
            return
        }
        if (window.menuOpen || window.helpOpen)
            return
        if (deleteDialog.visible || bulkDeleteDialog.visible)
            return
        if (window.sidebarFocused) {
            // RIGHT returns to the grid — the mirror of LEFT-at-the-edge
            // entering the sidebar below. LEFT stays a no-op; L1/R1 still
            // jump panels as a shortcut (see padTabStep).
            if (direction > 0 && grid.count > 0) {
                window.sidebarFocused = false
                grid.forceActiveFocus()
                sounds.play("nav_tick")
            }
            return
        }
        if (grid.count === 0) {
            window.focusGalleryOrSidebar()
            return
        }
        // Refresh the nav-lock so hover-follow can't override this move.
        grid._navLockUntil = Date.now() + 250
        const cols = window.gridColumnCount()
        const col = grid.currentIndex % cols
        if (direction < 0) {
            // LEFT at the leftmost column enters the sidebar; rows never
            // wrap. Bulk mode stays inside the grid (the sidebar has no
            // meaning for a selection in progress), so the edge just clamps.
            if (col === 0) {
                if (window.bulkMode)
                    return
                window.sidebarFocused = true
                window.refreshSidebarHoverIndex()
                sounds.play("nav_tick")
                return
            }
            grid.moveCurrentIndexLeft()
        } else {
            // RIGHT clamps at the row edge and at the last item; rows never
            // wrap.
            if (col === cols - 1 || grid.currentIndex === grid.count - 1)
                return
            grid.moveCurrentIndexRight()
        }
        grid.revealCurrent()
    }

    function padNavigateVertical(direction) {
        if (helpDialog.visible) {
            helpDialog.padScroll(direction)
            return
        }
        if (aboutDialog.visible) {
            aboutDialog.padStep(direction)
            return
        }
        if (window.settingsOpen) {
            // Up/Down moves inside whichever panel is focused, never across.
            if (window.sidebarFocused)
                window.sidebarStepVertical(direction)
            else
                settingsView.padFocusStep(direction)
            return
        }
        if (window.helpOpen)
            return
        if (deleteDialog.visible || bulkDeleteDialog.visible)
            return
        if (window.menuOpen) {
            const menuCount = padMenu.entries.length
            window.menuIndex = (window.menuIndex + direction + menuCount) % menuCount
            sounds.play("nav_tick")
        } else if (window.sidebarFocused) {
            window.sidebarStepVertical(direction)
        } else if (grid.count === 0) {
            window.focusGalleryOrSidebar()
            window.sidebarStepVertical(direction)
        } else {
            // Refresh the nav-lock so hover-follow can't override this move.
            grid._navLockUntil = Date.now() + 250
            // Explicit boundary clamp so spurious events (or a stuck analog
            // stick reading) can't wrap or jump: no item above the first row,
            // no item below the last row.
            const cols = window.gridColumnCount()
            if (direction < 0) {
                if (grid.currentIndex < cols)
                    return
                grid.moveCurrentIndexUp()
            } else {
                // currentIndex + cols >= count means no item sits directly
                // below (handles partial last rows too).
                if (grid.currentIndex + cols >= grid.count)
                    return
                grid.moveCurrentIndexDown()
            }
            grid.revealCurrent()
        }
    }

    // Right stick: wheel-like scroll of whatever surface is showing. The
    // selection stays put — the next D-pad move snaps the view back to it.
    function padScroll(direction) {
        if (helpDialog.visible) {
            helpDialog.padScroll(direction)
            return
        }
        if (aboutDialog.visible) {
            // Only the full release notes scroll; the compact About card
            // keeps Up/Down as focus steps and has nothing to wheel.
            if (aboutDialog.showFullNotes)
                aboutDialog.padVertical(direction)
            return
        }
        if (deleteDialog.visible || bulkDeleteDialog.visible)
            return
        if (window.settingsOpen) {
            settingsView.padScroll(direction)
            return
        }
        if (window.helpOpen || window.menuOpen)
            return
        if (grid.count > 0)
            grid.scrollBy(direction)
    }

    function padConfirm() {
        if (helpDialog.visible) { helpDialog.padConfirm(); return }
        if (aboutDialog.visible) { aboutDialog.padConfirm(); return }
        if (deleteDialog.visible) { deleteDialog.confirmed(); deleteDialog.close(); return }
        if (bulkDeleteDialog.visible) { window.bulkConfirmDelete(); bulkDeleteDialog.close(); return }
        if (window.settingsOpen) {
            // On the sidebar, Cross still activates the sidebar row (it is the
            // app's nav, not a settings control).
            if (window.sidebarFocused)
                window.activateSidebarRow(window.sidebarHoverIndex)
            else
                settingsView.padConfirm()
            return
        }
        if (window.helpOpen)
            return
        if (window.bulkMode) {
            const rec = app.gallery.get(grid.currentIndex)
            if (rec.filePath)
                window.bulkToggle(grid.currentIndex, false)
            return
        }
        if (window.menuOpen) {
            window.padMenuConfirm()
        } else if (window.sidebarFocused) {
            window.activateSidebarRow(window.sidebarHoverIndex)
        } else {
            sounds.play("confirm")
            lightbox.openAt(grid.currentIndex)
        }
    }

    function padFavorite() {
        if (aboutDialog.visible || window.menuOpen || window.settingsOpen || window.helpOpen)
            return
        if (window.sidebarFocused)
            return  // favorites only act on a grid tile
        if (window.bulkMode) {
            window.bulkSelectAll()
            return
        }
        sounds.play("favorite")
        app.toggleFavorite(grid.currentIndex)
    }

    function padToggleMenu() {
        if (aboutDialog.visible || helpDialog.visible || window.settingsOpen || grid.count === 0)
            return
        if (window.sidebarFocused)
            return  // action menu only acts on a grid tile
        if (window.bulkMode) {
            window.bulkAskDelete()
            return
        }
        window.menuOpen = !window.menuOpen
        if (window.menuOpen)
            window.menuIndex = 0
    }

    function padMenuConfirm() {
        // Index order mirrors padMenu.entries.
        if (window.menuIndex === 0) {
            app.showInFolder(grid.currentIndex)
        } else if (window.menuIndex === 1) {
            const rec = app.gallery.get(grid.currentIndex)
            window.askDelete(grid.currentIndex, rec.gameName, rec.dateText)
        } else {
            window.menuOpen = false
            window.bulkEnter()
            sounds.play("confirm")
            return
        }
        sounds.play("confirm")
        window.menuOpen = false
    }

    function padBack() {
        if (helpDialog.visible) { helpDialog.close(); return }
        if (aboutDialog.visible) { aboutDialog.close(); return }
        if (deleteDialog.visible) { deleteDialog.canceled(); deleteDialog.close(); return }
        if (bulkDeleteDialog.visible) { bulkDeleteDialog.canceled(); bulkDeleteDialog.close(); return }
        if (window.settingsOpen) {
            // Circle unwinds one step at a time: dropdown → options →
            // categories → out of Settings entirely.
            if (!window.sidebarFocused && settingsView.padBack())
                return
            window.settingsOpen = false
            window.focusGalleryOrSidebar()
        } else if (window.bulkMode) {
            window.bulkExit()
        } else if (window.sidebarFocused) {
            if (grid.count === 0)
                return
            window.sidebarFocused = false
            grid.forceActiveFocus()
        } else if (window.menuOpen) {
            window.menuOpen = false
        } else if (lightbox.visible) {
            lightbox.close()
        }
    }

    Connections {
        target: input
        function onDesktopNavigate(direction) {
            window.usingGamepad = true
            if (lightbox.visible) {
                lightbox.padNavigate(direction)
                return
            }
            window.padNavigate(direction)
        }
        function onDesktopNavigateVertical(direction) {
            window.usingGamepad = true
            if (lightbox.visible) {
                lightbox.padReveal()
                return
            }
            window.padNavigateVertical(direction)
        }
        function onDesktopConfirm() {
            window.usingGamepad = true
            if (lightbox.visible) {
                lightbox.padConfirm()
                return
            }
            window.padConfirm()
        }
        function onDesktopFavorite() {
            window.usingGamepad = true
            if (lightbox.visible) {
                lightbox.padReveal()
                return
            }
            window.padFavorite()
        }
        function onDesktopMenu() {
            window.usingGamepad = true
            if (lightbox.visible) {
                lightbox.padReveal()
                return
            }
            window.padToggleMenu()
        }
        function onDesktopTabStep(direction) {
            window.usingGamepad = true
            if (lightbox.visible) {
                lightbox.padStep(direction)
                return
            }
            window.padTabStep(direction)
        }
        function onDesktopBack() { window.usingGamepad = true; window.padBack() }
        function onDesktopSettings() {
            window.usingGamepad = true
            if (lightbox.visible || aboutDialog.visible || helpDialog.visible)
                return
            window.openSettings()
        }
        function onDesktopZoom(direction) {
            window.usingGamepad = true
            if (lightbox.visible || aboutDialog.visible || window.settingsOpen || window.helpOpen)
                return
            if (direction > 0)
                window.zoomIn()
            else
                window.zoomOut()
        }
        function onDesktopScroll(direction) {
            window.usingGamepad = true
            if (lightbox.visible)
                return
            window.padScroll(direction)
        }
        function onDesktopBulkToggle() {
            window.usingGamepad = true
            if (lightbox.visible || aboutDialog.visible || window.settingsOpen || window.helpOpen)
                return
            window.padBulkToggle()
        }
        function onPlaybackPlayPause() {
            if (lightbox.visible)
                lightbox.toggleVideoPlayback()
        }
        function onPlaybackSeek(direction) {
            if (lightbox.visible)
                lightbox.padNavigate(direction)
        }
        // Share on a focused clip in the lightbox: save the on-screen frame.
        function onFrameGrabRequested() {
            if (lightbox.visible)
                lightbox.saveCurrentFrame()
        }
    }

    Connections {
        target: lightbox
        function onActiveChanged() {
            input.setDesktopFocused(window.active || lightbox.active)
        }
        function onVisibleChanged() {
            input.setPlaybackActive(lightbox.visible
                                    && lightbox.current.captureType === "video")
        }
        function onIndexChanged() {
            input.setPlaybackActive(lightbox.visible
                                    && lightbox.current.captureType === "video")
        }
    }

    // Background gradient (design-system §1: never flat black)
    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.bg1 }
            GradientStop { position: 1; color: Theme.bg0 }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: Theme.s16
        spacing: Theme.s16

        // ───────────────────────── Sidebar ─────────────────────────
        DesktopSidebar {
            id: desktopSidebar
            categories: window.sidebarCategories
            settingsOpen: window.settingsOpen
            helpOpen: window.helpOpen
            aboutUnread: window.whatsNewSeenVersion !== app.version
            updateAvailable: updates.latestVersion !== ""
                             && ["UpdateAvailable", "Downloading", "ReadyToInstall",
                                 "PreparingForUpdate", "Quiescent", "Installing",
                                 "Failed"].includes(updates.stateName)
            availableVersion: updates.latestVersion
            sidebarFocused: window.sidebarFocused
            sidebarHoverIndex: window.sidebarHoverIndex
            onPageClosed: {
                window.settingsOpen = false
                window.helpOpen = false
            }
            onSettingsRequested: {
                window.helpOpen = false
                window.settingsOpen = true
            }
            onHelpRequested: {
                window.openHelp()
            }
            onAboutRequested: window.openAbout(false)
        }
        // ───────────────────────── Settings ─────────────────────────
        SettingsView {
            id: settingsView
            visible: window.settingsOpen
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.fillHeight: true
            onCloseRequested: {
                window.settingsOpen = false
                window.focusGalleryOrSidebar()
            }
        }

        // ───────────────────────── Content ─────────────────────────
        ColumnLayout {
            visible: !window.settingsOpen
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: Theme.s16

            DesktopGalleryHeader {
                titleText: {
                    if (app.currentGameAvailable && app.gameId === app.currentGameId && app.category === "favorites")
                        return "Game Favourites"
                    if (app.gameId >= 0) {
                        for (const g of app.games)
                            if (g.id === app.gameId) return g.name
                    }
                    return app.category.charAt(0).toUpperCase() + app.category.slice(1)
                }
                bulkMode: window.bulkMode
                bulkCount: window.bulkCount()
                bulkAllSelected: window.bulkAllSelected()
                onBulkEnterRequested: window.bulkEnter()
                onBulkSelectAllRequested: window.bulkSelectAll()
                onBulkDeleteRequested: window.bulkAskDelete()
                onBulkExitRequested: window.bulkExit()
            }
            DesktopGalleryGrid {
                id: grid
                columns: window.gridColumns(grid.viewWidth)
                zoomLevel: window.zoomLevel
                bulkMode: window.bulkMode
                bulkIsChecked: window.bulkIsChecked
                deleteDialog: deleteDialog
                bulkDeleteDialog: bulkDeleteDialog
                onCaptureActivated: (index) => lightbox.openAt(index)
                onDeleteRequested: (index, gameName, dateText) => window.askDelete(index, gameName, dateText)
                onAddFolderRequested: folderDialog.open()
                onKeyboardActivity: window.usingGamepad = false
                onBulkToggleRequested: (index, extendRange) => window.bulkToggle(index, extendRange)
                onBulkDeleteRequested: window.bulkAskDelete()
                onBulkSelectAllRequested: window.bulkSelectAll()
                onCountChanged: {
                    if (count === 0 && !window.settingsOpen && !window.helpOpen
                            && !aboutDialog.visible && !helpDialog.visible
                            && !lightbox.visible)
                        Qt.callLater(window.focusGalleryOrSidebar)
                }
            }

            DesktopGalleryFooter {
                hasCaptures: grid.count > 0
                sidebarFocused: window.sidebarFocused
                bulkMode: window.bulkMode
                usingGamepad: window.usingGamepad
                zoomLevel: window.zoomLevel
                onZoomOutRequested: window.zoomOut()
                onZoomMoved: (value) => window.zoomLevel = value
                onZoomInRequested: window.zoomIn()
            }
        }

    }

    // ───────────────────────── Update banner ─────────────────────────
    // Floats above the content rather than participating in the RowLayout,
    // so it never has to fight the sidebar/settings/help width math. Only
    // ever shown here (the desktop gallery window) — never above the pad
    // overlay or a running game.
    UpdateBanner {
        id: updateBanner
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Theme.s16
        z: 50
        onDetailsRequested: window.openAbout(false)
    }

    FolderDialog {
        id: folderDialog
        title: "Choose a folder to watch"
        onAccepted: app.addWatchedFolder(selectedFolder)
    }

    // ───────────────────────── Lightbox viewer ─────────────────────────
    Lightbox {
        id: lightbox
        parentWindow: window
        galleryModel: app.gallery
        onClosed: window.requestControllerFocusRestore()
    }

    // ───────────────────────── Delete confirmation ─────────────────────────
    property int pendingDeleteRow: -1

    function askDelete(row, name, date) {
        window.pendingDeleteRow = row
        deleteDialog.message = name + " · " + date + "\nThis permanently deletes the file."
        deleteDialog.open()
    }

    ConfirmDialog {
        id: deleteDialog
        anchors.fill: parent
        z: 100
        title: "Delete capture?"
        confirmLabel: "Delete"
        onConfirmed: {
            sounds.play("confirm")
            app.deleteCapture(window.pendingDeleteRow)
            window.pendingDeleteRow = -1
        }
        onCanceled: window.pendingDeleteRow = -1
    }

    ConfirmDialog {
        id: bulkDeleteDialog
        anchors.fill: parent
        z: 100
        title: "Delete selected captures?"
        confirmLabel: "Delete"
        onConfirmed: window.bulkConfirmDelete()
    }

    AboutWhatsNewDialog {
        id: aboutDialog
        anchors.fill: parent
        z: 500
        onClosed: window.finishAbout()
        onUpdateSettingsRequested: window.openAboutSettings()
        onUpdateDeferred: updateBanner.dismissed = true
    }

    HelpDialog {
        id: helpDialog
        anchors.fill: parent
        z: 500
        onClosed: window.finishHelp()
    }

    // ───────────────────────── Pad action menu (Square) ─────────────────────────
    // Show in folder / Delete for the currently pad-selected tile — mouse
    // users already have per-tile hover icons for this; pad users need an
    // equivalent that doesn't require hovering.
    OverlayActionMenu {
        id: padMenu
        // Bulk select is desktop-only, so it is added here rather than in the
        // shared component. padMenuConfirm() maps these by index.
        entries: ["Show in folder", "Delete", "Bulk select"]
        open: window.menuOpen
        currentIndex: window.menuIndex
        onCloseRequested: window.menuOpen = false
        onItemHovered: function(index) { window.menuIndex = index }
        onItemConfirmed: function(index) {
            window.menuIndex = index
            window.padMenuConfirm()
        }
    }
}
