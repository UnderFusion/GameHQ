import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import QtMultimedia
import GameHQ
import "components"
import "helpers/SidebarCategories.js" as SidebarCategories

// In-game overlay shell (milestone 0.2, growing into 0.6's couch gallery):
// dark scrim over the game, sidebar (categories + games, with the GameHQ
// brand mark at its bottom), recent-captures strip, big preview, per-capture
// action menu. Toggled by
// Ctrl+Shift+G (OverlayManager/HotkeyManager) or PS; Circle closes. The window
// never accepts keyboard focus, so Esc/Backspace only act on the desktop path,
// not here.
// Uses its own `overlayGallery` GalleryModel instance so its category/game
// filter never clashes with the main window's. PS5-style slide+fade per
// design-system Â§5.
//
// Navigation: Up/Down (D-pad or left stick) always steps the sidebar and
// applies its filter immediately; Left/Right (D-pad or stick) flips between
// captures like L1/R1, and seeks instead once X is playing a clip. L1/R1
// always flips. The only modal state is the action menu (Square/M).
Window {
    id: overlayWindow

    // Keep native window geometry separate from the scaled interface.
    readonly property ScaledSurface uiSurface: ScaledSurface {
        id: scaledViewport
        parent: overlayWindow.contentItem
        anchors.fill: parent
        settings: app
        configKey: "theme.overlay_scale"
        minimumContentWidth: Theme.minimumUiWidth
        minimumContentHeight: Theme.minimumUiHeight
        // A close stays invisible while the controller handoff finishes.
        opacity: overlay.closing ? 0 : 1
    }
    Overlay.overlay.transform: uiSurface.scaleTransform
    Overlay.overlay.opacity: overlay.closing ? 0 : 1
    LayoutMirroring.enabled: languageManager.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    objectName: "gamehqOverlay"
    visible: false
    color: "transparent"
    //% "%1 Overlay"
    title: qsTrId("gamehq.overlay.window_title").arg(Brand.name)

    // The window is created once and only shown/hidden afterward, so its
    // properties persist across toggles. Reset the action-menu state on
    // every close â€” otherwise the overlay can reopen already inside the
    // menu with "Delete" pre-selected, and the very next confirm silently
    // deletes a capture with no menu visibly just having been opened.
    onVisibleChanged: {
        if (!overlayWindow.visible) {
            content.keyboardDriving = false
            content.rememberSelection()
            content.menuOpen = false
            content.menuIndex = 0
            // A finished or unstarted Share never survives a hide; a send in
            // flight keeps running and shows its result on the next open.
            overlayShare.close()
            viewer.close()
            content.stopVideoFocus()
            content.collapseAutoSidebar()
            content.sidebarPointerHold = false
        } else if (!content.restoreSelection()) {
            content.selectDefaultSection()
        }
    }

    // Tracks which input source was used last so the footer, strip and
    // full-screen viewer show matching hints (keyboard glyphs vs DualSense
    // button names). True on pad input; false on a key press or a real mouse
    // move. Starts on the pad labels because the pad is what opens it.
    property bool usingGamepad: true

    // Keyboard input reaches QML through input.handleKeyPressed(), which
    // emits the SAME overlay* signals as the pad (and keeps emitting them
    // from the nav-repeat timer while a key is held). Those handlers must not
    // flip the hints back to the pad while a key is what drives them.
    function notePadInput() {
        if (!content.keyboardDriving)
            overlayWindow.usingGamepad = true
    }
    onActiveChanged: if (!overlayWindow.active) content.keyboardDriving = false

    // Row queued by the mouse delete path until the confirm dialog answers.
    // Keyed by file path, not row: the gallery follows the disk live, so rows
    // can shift while the confirmation is open.
    property string pendingDeletePath: ""

    Connections {
        target: app
        function onCurrentGameChanged() {
            // A game switch re-restores that game's remembered category, but
            // only while the user is on a category row — someone browsing a
            // specific game's row keeps it.
            if (overlayWindow.visible && content.sidebarIndex < content.categories.length)
                content.selectDefaultSection()
        }
    }

    Rectangle {
        parent: uiSurface.contentItem
        id: scrim
        z: -1
        anchors.fill: parent
        color: Theme.overlayScrim
        // No fade: the panels appear in the first frame, so an animated scrim
        // arrived visibly after them (hide is instant, so it only ever played
        // on open). Scrim and panels now land together.

        // Click-outside-to-close: this MouseArea sits below every panel in
        // z-order, so it only ever sees clicks that none of the panels
        // (sidebar, strip, preview) swallowed first â€” i.e. clicks that
        // actually landed on bare scrim/game background.
        MouseArea {
            anchors.fill: parent
            enabled: overlayWindow.visible
            onClicked: overlay.hide()
        }
    }

    // Also shown during the non-activating experiment: the game deliberately
    // keeps foreground and may receive the same controller input.
    Rectangle {
        parent: uiSurface.contentItem
        visible: overlayWindow.visible && !overlay.foregroundAcquired
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: Theme.s12
        width: Math.min(parent.width - Theme.s24, focusWarningText.implicitWidth + Theme.s24)
        height: focusWarningText.implicitHeight + Theme.s12
        radius: Theme.radiusM
        color: Theme.surface
        border.width: 1
        border.color: Theme.warning
        z: 10
        Text {
            id: focusWarningText
            anchors.centerIn: parent
            width: parent.width - Theme.s24
            wrapMode: Text.WordWrap
            horizontalAlignment: Text.AlignHCenter
            //% "The game still has focus and may react to controller input"
            text: qsTrId("gamehq.overlay.focus_warning")
            color: Theme.warning
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
        }
    }

    Item {
        parent: uiSurface.contentItem
        id: content
        anchors.fill: parent
        anchors.margins: Theme.s48
        focus: true

        property bool menuOpen: false
        property int sidebarIndex: 0
        property int menuIndex: 0
        property bool videoFocused: false   // X (Cross) enters clip-player mode in the big preview
        // A non-repeat key is down; see overlayWindow.notePadInput().
        property bool keyboardDriving: false
        // Playback bindings (Cross = play/pause, D-pad = seek, Share = frame
        // grab) apply to the inline clip and to a clip shown full screen.
        readonly property bool playbackActive: content.videoFocused
            || (viewer.open && viewer.currentIsVideo)
        onPlaybackActiveChanged: input.setPlaybackActive(content.playbackActive)
        property var categories: SidebarCategories.categories(app.currentGameAvailable)
        function totalSidebarCount() { return content.categories.length + app.games.length }

        // Sidebar presentation (ui.overlay_sidebar_mode): auto | expanded |
        // collapsed. Auto opens on the icon rail, widens while Up/Down walks
        // the sidebar or the mouse is over it, and narrows again after a short
        // pause or as soon as the captures are used. The mode toggle sits one
        // step past the last game: moving onto it only highlights it (the
        // filter stays put) and Cross/Enter cycles the mode.
        property string sidebarMode: {
            const v = app.config("ui.overlay_sidebar_mode", "auto")
            return v === "expanded" || v === "collapsed" ? v : "auto"
        }
        property bool sidebarNavActive: false
        property bool sidebarPointerHold: false
        property bool modeToggleFocused: false
        readonly property bool sidebarExpanded: content.sidebarMode === "expanded"
            || (content.sidebarMode === "auto"
                && (content.sidebarNavActive || content.sidebarPointerHold))

        function cycleSidebarMode() {
            const next = content.sidebarMode === "auto" ? "expanded"
                       : content.sidebarMode === "expanded" ? "collapsed"
                       : "auto"
            content.sidebarMode = next
            app.setConfig("ui.overlay_sidebar_mode", next)
            sounds.play("confirm")
        }

        function noteSidebarNavigation() {
            content.sidebarNavActive = true
            sidebarAutoCollapse.restart()
        }

        function collapseAutoSidebar() {
            sidebarAutoCollapse.stop()
            content.sidebarNavActive = false
            content.modeToggleFocused = false
        }

        Timer {
            id: sidebarAutoCollapse
            interval: Theme.sidebarAutoCollapseMs
            onTriggered: content.sidebarNavActive = false
        }
        Timer {
            id: sidebarPointerGrace
            interval: Theme.sidebarHoverGraceMs
            onTriggered: content.sidebarPointerHold = false
        }

        // Moves the highlight AND applies the filter immediately â€” same
        // instant-feedback feel as Left/Right on the strip.
        function selectSidebarEntryAt(idx, playSound) {
            if (idx < content.categories.length) {
                const key = content.categories[idx].key
                const f = SidebarCategories.resolveFilter(key, app.currentGameId)
                overlayGallery.setFilter(f.category, f.gameId)
                // Remember the category under the foreground game only: the
                // game binding itself always follows the foreground.
                if (app.currentGameAvailable)
                    app.setOverlayCategory(app.currentGameId, f.category)
            } else {
                const game = app.games[idx - content.categories.length]
                if (!game)
                    return
                overlayGallery.setFilter("all", game.id)
            }
            content.sidebarIndex = idx
            strip.currentIndex = 0
            sounds.play(playSound)
        }

        // Opening the overlay restores the category this game was left on
        // (rule R8); with no game in the foreground there is nothing per-game
        // to restore and the whole library opens on "all".
        function selectDefaultSection() {
            const gameId = app.currentGameAvailable ? app.currentGameId : -1
            const saved = app.overlayCategory(gameId)
            const key = gameId >= 0 && saved === "all" ? "game"
                      : gameId >= 0 && saved === "favorites" ? "game_favorites"
                      : saved
            const idx = Math.max(0, content.categories.findIndex(c => c.key === key))
            content.sidebarIndex = idx
            const f = SidebarCategories.resolveFilter(content.categories[idx].key, gameId)
            overlayGallery.setFilter(f.category, f.gameId)
            strip.currentIndex = 0
        }

        // Reopening returns to the capture the user left on (same sidebar entry,
        // same item) while the foreground game is unchanged. A capture made in
        // the meantime becomes the newest row, and then the strip starts on it.
        property var savedSelection: null
        function rememberSelection() {
            const current = overlayGallery.get(strip.currentIndex)
            const newest = overlayGallery.get(0)
            content.savedSelection = {
                gameId: app.currentGameAvailable ? app.currentGameId : -1,
                sidebarIndex: content.sidebarIndex,
                category: overlayGallery.category,
                filterGameId: overlayGallery.gameId,
                selectedPath: current.filePath || "",
                newestPath: newest.filePath || ""
            }
        }
        function restoreSelection() {
            const saved = content.savedSelection
            content.savedSelection = null
            const gameId = app.currentGameAvailable ? app.currentGameId : -1
            if (!saved || saved.gameId !== gameId
                    || saved.sidebarIndex >= content.totalSidebarCount())
                return false
            overlayGallery.setFilter(saved.category, saved.filterGameId)
            content.sidebarIndex = saved.sidebarIndex
            const newestNow = overlayGallery.get(0).filePath || ""
            const row = newestNow === saved.newestPath ? overlayGallery.rowOf(saved.selectedPath) : -1
            strip.currentIndex = Math.max(0, row)
            return true
        }

        function sidebarStep(direction) {
            const count = content.totalSidebarCount()
            if (count <= 0)
                return
            content.noteSidebarNavigation()
            const slots = count + 1   // + the mode toggle
            const current = content.modeToggleFocused ? count : content.sidebarIndex
            const idx = (current + direction + slots) % slots
            if (idx === count) {
                content.modeToggleFocused = true
                sounds.play("nav_tick")
                return
            }
            content.modeToggleFocused = false
            content.selectSidebarEntryAt(idx, "nav_tick")
        }

        function menuStep(direction) {
            const count = actionMenu.entries.length
            content.menuIndex = (content.menuIndex + direction + count) % count
            sounds.play("nav_tick")
        }

        function menuConfirm() {
            content.runMenuAction(actionMenu.currentActionId())
        }

        // Acts on the stable action id of a menu entry, never on its position.
        function runMenuAction(actionId) {
            content.menuOpen = false
            switch (actionId) {
            case "share":
                content.openShare(overlayGallery.get(strip.currentIndex))
                return
            case "show_in_folder":
                app.showInFolderFrom(overlayGallery, strip.currentIndex)
                break
            case "delete":
                app.deleteCaptureFrom(overlayGallery, strip.currentIndex)
                break
            default:
                return
            }
            sounds.play("confirm")
        }

        // Share flow for one capture record; modal until closed.
        function openShare(rec) {
            if (!rec || !rec.filePath)
                return
            content.stopVideoFocus()
            overlayShare.openFor(rec.filePath, rec.gameName)
        }

        function stopVideoFocus() {
            previewStage.stopPlayback()
            content.videoFocused = false
        }

        function revealVideoControls() {
            if (content.videoFocused)
                previewStage.revealControls()
        }

        function revealControls() {
            if (viewer.open)
                viewer.revealControls()
            else
                content.revealVideoControls()
        }

        function seekVideo(deltaMs) {
            if (!previewStage.canSeek)
                return
            content.revealVideoControls()
            previewStage.seekBy(deltaMs)
        }

        function toggleVideoPlayback() {
            if (!previewStage.displayedIsVideo)
                return

            if (!content.videoFocused) {
                content.videoFocused = true
                sounds.play("confirm")
                return
            }

            content.revealVideoControls()
            if (previewStage.isPlaying) {
                previewStage.pauseVideo()
            } else {
                previewStage.playVideo()
            }
            sounds.play("confirm")
        }

        // L1/R1 pad path: ALWAYS flips between captures, independent of
        // whether a clip is currently focused or playing. This is the only
        // pad gesture that switches items â€” see the user request that L1/R1
        // and d-pad seek must be two independent controls.
        function handleCaptureStep(direction) {
            if (overlayShare.isOpen)
                return   // Share is modal
            if (viewer.open) {
                viewer.step(direction)
                return
            }
            if (content.menuOpen)
                return
            content.collapseAutoSidebar()
            if (direction < 0)
                strip.decrementCurrentIndex()
            else
                strip.incrementCurrentIndex()
        }

        // D-pad left/right pad path: seeks a clip that is playing (inline after
        // X, or shown in the full-screen viewer); otherwise it flips captures
        // exactly like L1/R1.
        function handleSeekStep(direction) {
            if (overlayShare.isOpen)
                return
            if (viewer.open) {
                if (viewer.currentIsVideo)
                    viewer.seekVideo(direction * viewer.seekStepMs)
                else
                    viewer.step(direction)
                return
            }
            if (content.menuOpen)
                return
            if (!content.videoFocused) {
                content.handleCaptureStep(direction)
                return
            }
            content.seekVideo(direction * previewStage.seekStepMs)
        }

        function handleNavigateVertical(direction) {
            if (overlayShare.isOpen) {
                overlayShare.padNavigate(direction)
                return
            }
            if (viewer.open)
                return   // the sidebar filter behind the viewer stays put
            if (content.menuOpen)
                content.menuStep(direction)
            else
                content.sidebarStep(direction)
        }

        function handleConfirm() {
            if (overlayShare.isOpen) {
                overlayShare.padConfirm()
                return
            }
            if (content.menuOpen) {
                content.menuConfirm()
                return
            }
            if (viewer.open) {
                viewer.toggleVideoPlayback()
                return
            }
            if (content.modeToggleFocused) {
                content.cycleSidebarMode()
                content.noteSidebarNavigation()
                return
            }
            content.collapseAutoSidebar()
            // X (Cross) plays a clip inline in the big preview pane, and opens a
            // screenshot in the full-screen viewer.
            var item = overlayGallery.get(strip.currentIndex)
            if (item && item.captureType === "video")
                content.toggleVideoPlayback()
            else if (item && item.filePath)
                content.openViewer()
        }

        function openViewer() {
            content.stopVideoFocus()
            viewer.openAt(strip.currentIndex)
            sounds.play("confirm")
        }

        function toggleMenu() {
            if (overlayShare.isOpen)
                return
            // The full-screen viewer has no menu of its own: Square shares
            // the capture it shows.
            if (viewer.open) {
                content.openShare(overlayGallery.get(viewer.index))
                return
            }
            content.menuOpen = !content.menuOpen
            if (content.menuOpen)
                content.menuIndex = 0
        }

        function togglePlayback() {
            if (overlayShare.isOpen)
                return
            if (viewer.open)
                viewer.toggleVideoPlayback()
            else
                content.toggleVideoPlayback()
        }

        function handleFavorite() {
            if (overlayShare.isOpen)
                return
            if (content.menuOpen)
                return
            sounds.play("favorite")
            overlayGallery.toggleFavorite(viewer.open ? viewer.index : strip.currentIndex)
        }

        function handleBack() {
            if (overlayShare.isOpen) {
                overlayShare.padBack()
                return
            }
            if (viewer.open)
                viewer.close()   // Circle leaves full screen before anything else
            else if (content.modeToggleFocused)
                content.collapseAutoSidebar()
            else if (content.menuOpen)
                content.menuOpen = false
            else if (content.videoFocused) {
                content.revealVideoControls()
                content.stopVideoFocus()   // Circle/Esc backs out of playback first
            } else {
                overlay.hide()
            }
        }

        Keys.onPressed: (event) => {
            overlayWindow.usingGamepad = false
            if (!event.isAutoRepeat)
                content.keyboardDriving = true
            content.revealControls()
            event.accepted = input.handleKeyPressed(event.key, event.modifiers,
                                                    event.isAutoRepeat)
        }
        Keys.onReleased: (event) => {
            if (!event.isAutoRepeat)
                content.keyboardDriving = false
            event.accepted = input.handleKeyReleased(event.key, event.modifiers)
        }

        // DualSense navigation (0.3): InputEngine routes pad input here only
        // while the overlay is open. Same dispatch functions as keyboard.
        // D-pad and left-stick deflection both arrive as the same
        // DpadUp/Down/Left/Right edges (see DualSenseDevice::parseReport).
        Connections {
            target: input
            // D-pad left/right flips captures like L1/R1 while browsing, and
            // seeks once a clip is playing (see handleSeekStep).
            function onOverlayNavigate(direction) {
                overlayWindow.notePadInput()
                content.handleSeekStep(direction)
            }
            function onOverlayNavigateVertical(direction) {
                overlayWindow.notePadInput()
                content.revealControls()
                content.handleNavigateVertical(direction)
            }
            function onOverlayConfirm() {
                overlayWindow.notePadInput()
                content.revealControls()
                content.handleConfirm()
            }
            function onOverlayFavorite() {
                overlayWindow.notePadInput()
                content.revealControls()
                content.handleFavorite()
            }
            function onOverlayMenu() {
                overlayWindow.notePadInput()
                content.revealControls()
                content.toggleMenu()
            }
            // L1/R1: the ONLY pad path that flips between captures in the
            // strip, and it ALWAYS flips â€” even while a clip is focused or
            // playing. Video seeking is d-pad/analog's job (onOverlayNavigate
            // above), so the two stay fully independent.
            function onOverlayGameStep(direction) {
                overlayWindow.notePadInput()
                content.revealControls()
                content.handleCaptureStep(direction)
            }
            function onOverlayHideRequested() {
                overlayWindow.notePadInput()
                content.revealControls()
                content.handleBack()
            }
            function onPlaybackPlayPause() {
                overlayWindow.notePadInput()
                content.togglePlayback()
            }
            function onPlaybackSeek(direction) {
                overlayWindow.notePadInput()
                content.handleSeekStep(direction)
            }
            // Share while a clip is focused: grab the on-screen frame as a
            // screenshot instead of the global foreground screenshot.
            function onFrameGrabRequested() {
                overlayWindow.notePadInput()
                if (overlayShare.isOpen)
                    return
                if (viewer.open)
                    viewer.saveCurrentFrame()
                else
                    previewStage.saveCurrentFrame()
            }
        }

        // Body: sidebar (categories/games) on the left, strip + preview on
        // the right â€” layout per docs/product-spec.md Â§15. No header any
        // more (the GameHQ logo/title moved to the bottom of the sidebar
        // instead) â€” body now owns the space the header used to take.
        Item {
            id: body
            anchors.top: parent.top
            anchors.bottom: footer.top
            anchors.bottomMargin: Theme.s24
            anchors.left: parent.left
            anchors.right: parent.right

            OverlaySidebar {
                id: sidebarPane
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                categories: content.categories
                sidebarIndex: content.sidebarIndex
                mode: content.sidebarMode
                expanded: content.sidebarExpanded
                modeFocused: content.modeToggleFocused
                onEntrySelected: function(index) {
                    content.modeToggleFocused = false
                    content.selectSidebarEntryAt(index, "confirm")
                }
                onModeCycleRequested: content.cycleSidebarMode()
                onPointerInsideChanged: {
                    if (pointerInside) {
                        sidebarPointerGrace.stop()
                        content.sidebarPointerHold = true
                    } else {
                        sidebarPointerGrace.restart()
                    }
                }
            }

            OverlayCaptureStrip {
                id: strip
                anchors.top: parent.top
                anchors.left: sidebarPane.right
                anchors.leftMargin: Theme.s32
                anchors.right: parent.right
                model: overlayGallery
                usingGamepad: overlayWindow.usingGamepad
                videoFocused: content.videoFocused
                onDeleteRequested: function(index) {
                    overlayWindow.pendingDeletePath = overlayGallery.get(index).filePath || ""
                    deleteDialog.open()
                }
                onShareRequested: function(index) {
                    content.openShare(overlayGallery.get(index))
                }
                onOpenFolderRequested: function(index) {
                    sounds.play("confirm")
                    app.showInFolderFrom(overlayGallery, index)
                }
                onFavoriteToggleRequested: function(index) {
                    sounds.play("favorite")
                    overlayGallery.toggleFavorite(index)
                }
            }
            OverlayPreview {
                id: previewStage
                anchors.top: strip.bottom
                anchors.topMargin: Theme.s32
                anchors.bottom: parent.bottom
                anchors.left: sidebarPane.right
                anchors.leftMargin: Theme.s32
                anchors.right: parent.right
                galleryModel: overlayGallery
                currentIndex: strip.currentIndex
                videoFocused: content.videoFocused
                onPlayPauseRequested: content.toggleVideoPlayback()
                onSeekRequested: function(deltaMs) { content.seekVideo(deltaMs) }
                onBackRequested: content.handleBack()
            }

            Connections {
                target: strip
                function onCurrentIndexChanged() {
                    content.stopVideoFocus()
                }
            }
        }

        OverlayFooter {
            id: footer
            usingGamepad: overlayWindow.usingGamepad
            menuOpen: content.menuOpen
            videoFocused: content.videoFocused
        }
    }

    // A real mouse move switches the hints to keyboard/mouse glyphs. Topmost
    // and non-blocking, so every panel below still gets its own hover. The
    // first point after the cursor enters (the overlay appearing under a
    // resting cursor) only sets the baseline; it is not a move.
    Item {
        parent: uiSurface.contentItem
        anchors.fill: parent
        z: 1000
        HoverHandler {
            id: mouseActivity
            property point _last
            property bool _hasLast: false
            enabled: overlayWindow.visible
            blocking: false
            onHoveredChanged: mouseActivity._hasLast = false
            onPointChanged: {
                const p = mouseActivity.point.position
                if (mouseActivity._hasLast
                        && Math.abs(p.x - mouseActivity._last.x)
                           + Math.abs(p.y - mouseActivity._last.y) >= Theme.s4)
                    overlayWindow.usingGamepad = false
                mouseActivity._last = p
                mouseActivity._hasLast = true
            }
        }
    }

    // Full-screen viewer (Cross on a screenshot). Above the panels, below the
    // focus warning so that stays readable.
    OverlayViewer {
        parent: uiSurface.contentItem
        id: viewer
        anchors.fill: parent
        z: 5
        galleryModel: overlayGallery
        usingGamepad: overlayWindow.usingGamepad
        onClosed: function(lastIndex) {
            // Return to the strip on the capture the viewer ended on.
            if (lastIndex >= 0 && lastIndex < overlayGallery.rowCount())
                strip.currentIndex = lastIndex
        }
    }

    // Per-capture action menu (Square / M) â€” Share / Show in folder / Delete.
    OverlayActionMenu {
        parent: uiSurface.contentItem
        id: actionMenu
        z: 100
        open: content.menuOpen
        currentIndex: content.menuIndex
        onCloseRequested: content.menuOpen = false
        onItemHovered: function(index) { content.menuIndex = index }
        onActionConfirmed: function(actionId) { content.runMenuAction(actionId) }
    }

    // Share (docs/share-platform.md): same flow as the desktop, modal while
    // open — every pad handler above hands its input to it first.
    ShareDialog {
        parent: uiSurface.contentItem
        id: overlayShare
        // Set when the user still has a step to read before the overlay may
        // step aside (see the Connections below).
        property bool hideOverlayOnClose: false
        onClosed: {
            content.forceActiveFocus()
            if (hideOverlayOnClose) {
                hideOverlayOnClose = false
                overlay.hide()
            }
        }
    }
    // A hand-off (e.g. Telegram's "choose a chat" box) needs the screen: the
    // overlay is topmost over the game, so step aside once the app has the file.
    // The exception is a hand-off that ends with something for the user to do
    // (Discord: paste into a chat). That instruction must be readable, so the
    // dialog stays until the user confirms it and only then the overlay hides.
    Connections {
        target: shareService
        function onFinished(result) {
            if (!overlayShare.isOpen || result.outcome !== "handed_off")
                return
            if (result.detail === "paste") {
                overlayShare.hideOverlayOnClose = true
                return
            }
            overlayShare.close()
            overlay.hide()
        }
    }

    // Mouse delete confirmation. Above the action menu in z-order so it stays
    // usable no matter which path opened it.
    ConfirmDialog {
        parent: uiSurface.contentItem
        id: deleteDialog
        anchors.fill: parent
        z: 200
        //% "Delete capture?"
        title: qsTrId("gamehq.gallery.delete_capture.title")
        //% "Delete"
        confirmLabel: qsTrId("gamehq.action.delete")
        onConfirmed: {
            sounds.play("confirm")
            const row = overlayGallery.rowOf(overlayWindow.pendingDeletePath)
            if (row >= 0)
                app.deleteCaptureFrom(overlayGallery, row)
            overlayWindow.pendingDeletePath = ""
        }
        onCanceled: overlayWindow.pendingDeletePath = ""
    }
}
