import QtQuick
import QtQuick.Effects
import QtMultimedia
import GameHQ
import "components"
import "helpers/SidebarCategories.js" as SidebarCategories

// In-game overlay shell (milestone 0.2, growing into 0.6's couch gallery):
// dark scrim over the game, sidebar (categories + games, with the GameHQ
// brand mark at its bottom), recent-captures strip, big preview, per-capture
// action menu. Toggled by
// Ctrl+Shift+G (OverlayManager/HotkeyManager) or PS; Esc/Circle close.
// Uses its own `overlayGallery` GalleryModel instance so its category/game
// filter never clashes with the main window's. PS5-style slide+fade per
// design-system Â§5.
//
// Navigation: Up/Down (D-pad or left stick) always steps the sidebar and
// applies its filter immediately; Left/Right (D-pad or stick) ALWAYS seeks
// the focused clip (no-op on a screenshot). Flipping between captures is
// L1/R1 only â€” these are two fully independent pad controls. The only
// modal state is the action menu (Square/M).
Window {
    id: overlayWindow
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
            content.menuOpen = false
            content.menuIndex = 0
            content.stopVideoFocus()
        } else {
            content.selectDefaultSection()
        }
    }

    // Tracks which input source was used last so the footer hint can show
    // matching labels (keyboard glyphs vs DualSense button names). Flipped
    // to true on any pad input, false on any key press.
    property bool usingGamepad: false

    // Row queued by the mouse delete path until the confirm dialog answers.
    property int pendingDeleteRow: -1

    Connections {
        target: app
        function onCurrentGameChanged() {
            if (overlayWindow.visible && content.sidebarIndex === 0)
                content.selectDefaultSection()
        }
    }

    Rectangle {
        id: scrim
        anchors.fill: parent
        color: Theme.overlayScrim
        opacity: overlayWindow.visible ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durSlow; easing.type: Easing.OutQuint } }

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

    // Honest-isolation notice: shown only when Windows refused to hand the
    // overlay the foreground, i.e. the game underneath still owns focus and
    // may keep reacting to the controller. Non-blocking on purpose.
    Rectangle {
        visible: overlayWindow.visible && !overlay.foregroundAcquired
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: Theme.s12
        width: focusWarningText.implicitWidth + Theme.s24
        height: focusWarningText.implicitHeight + Theme.s12
        radius: Theme.radiusM
        color: Theme.surface
        border.width: 1
        border.color: Theme.warning
        z: 10
        Text {
            id: focusWarningText
            anchors.centerIn: parent
            //% "The game still has focus and may react to controller input"
            text: qsTrId("gamehq.overlay.focus_warning")
            color: Theme.warning
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
        }
    }

    Item {
        id: content
        anchors.fill: parent
        anchors.margins: Theme.s48
        focus: true

        property bool menuOpen: false
        property int sidebarIndex: 0
        property int menuIndex: 0
        property bool videoFocused: false   // X (Cross) enters clip-player mode in the big preview
        onVideoFocusedChanged: input.setPlaybackActive(content.videoFocused)
        property var categories: SidebarCategories.categories(app.currentGameAvailable)
        function totalSidebarCount() { return content.categories.length + app.games.length }

        // Moves the highlight AND applies the filter immediately â€” same
        // instant-feedback feel as Left/Right on the strip.
        function selectSidebarEntryAt(idx, playSound) {
            if (idx < content.categories.length) {
                const f = SidebarCategories.resolveFilter(content.categories[idx].key,
                                                          app.currentGameId)
                overlayGallery.setFilter(f.category, f.gameId)
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

        function selectDefaultSection() {
            content.sidebarIndex = 0
            if (app.currentGameAvailable)
                overlayGallery.setFilter("all", app.currentGameId)
            else
                overlayGallery.setFilter("all", -1)
            strip.currentIndex = 0
        }

        function sidebarStep(direction) {
            const count = content.totalSidebarCount()
            if (count <= 0)
                return
            const idx = (content.sidebarIndex + direction + count) % count
            content.selectSidebarEntryAt(idx, "nav_tick")
        }

        function toggleMenu() {
            content.menuOpen = !content.menuOpen
            if (content.menuOpen)
                content.menuIndex = 0
        }

        function menuStep(direction) {
            content.menuIndex = (content.menuIndex + direction + 2) % 2
            sounds.play("nav_tick")
        }

        function menuConfirm() {
            if (content.menuIndex === 0)
                app.showInFolderFrom(overlayGallery, strip.currentIndex)
            else
                app.deleteCaptureFrom(overlayGallery, strip.currentIndex)
            sounds.play("confirm")
            content.menuOpen = false
        }

        function stopVideoFocus() {
            previewStage.stopPlayback()
            content.videoFocused = false
        }

        function revealVideoControls() {
            if (content.videoFocused)
                previewStage.revealControls()
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

        // Keyboard left/right fallback: keeps the legacy dual behavior (seek
        // when a clip is focused, otherwise flip). Pad uses the two dedicated
        // paths below so the controls stay independent on the gamepad.
        function handleNavigate(direction) {
            if (content.menuOpen)
                return
            if (content.videoFocused) {
                content.seekVideo(direction * previewStage.seekStepMs)
                return
            }
            if (direction < 0)
                strip.decrementCurrentIndex()
            else
                strip.incrementCurrentIndex()
        }

        // L1/R1 pad path: ALWAYS flips between captures, independent of
        // whether a clip is currently focused or playing. This is the only
        // pad gesture that switches items â€” see the user request that L1/R1
        // and d-pad seek must be two independent controls.
        function handleCaptureStep(direction) {
            if (content.menuOpen)
                return
            if (direction < 0)
                strip.decrementCurrentIndex()
            else
                strip.incrementCurrentIndex()
        }

        // D-pad left/right pad path: ALWAYS seeks the focused clip. No-op
        // when no clip is in focus (e.g. on a screenshot, or before X has
        // been pressed on a video) â€” d-pad never flips captures.
        function handleSeekStep(direction) {
            if (content.menuOpen)
                return
            if (content.videoFocused) {
                content.seekVideo(direction * previewStage.seekStepMs)
                return
            }
            content.handleCaptureStep(direction)
        }

        function handleNavigateVertical(direction) {
            if (content.menuOpen)
                content.menuStep(direction)
            else
                content.sidebarStep(direction)
        }

        function handleConfirm() {
            if (content.menuOpen) {
                content.menuConfirm()
                return
            }
            // X (Cross) is reserved for playing a video clip in the big preview
            // pane itself. Screenshots are already shown there on selection, so
            // X only does something for videos.
            var item = overlayGallery.get(strip.currentIndex)
            if (item && item.captureType === "video")
                content.toggleVideoPlayback()
        }

        function handleFavorite() {
            if (content.menuOpen)
                return
            sounds.play("favorite")
            overlayGallery.toggleFavorite(strip.currentIndex)
        }

        function handleBack() {
            if (content.menuOpen)
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
            if (content.videoFocused)
                content.revealVideoControls()
            event.accepted = input.handleKeyPressed(event.key, event.modifiers,
                                                    event.isAutoRepeat)
        }
        Keys.onReleased: (event) => {
            event.accepted = input.handleKeyReleased(event.key, event.modifiers)
        }

        // DualSense navigation (0.3): InputEngine routes pad input here only
        // while the overlay is open. Same dispatch functions as keyboard.
        // D-pad and left-stick deflection both arrive as the same
        // DpadUp/Down/Left/Right edges (see DualSenseDevice::parseReport).
        Connections {
            target: input
            // D-pad left/right is the SEEK path: only meaningful when a clip
            // is focused (X entered clip-player mode). On a screenshot or
            // before X has been pressed, it's an intentional no-op â€” d-pad
            // never flips captures. That job belongs to L1/R1 below, so the
            // two gestures stay independent per the user's spec.
            function onOverlayNavigate(direction) {
                overlayWindow.usingGamepad = true
                content.handleSeekStep(direction)
            }
            function onOverlayNavigateVertical(direction) {
                overlayWindow.usingGamepad = true
                content.revealVideoControls()
                content.handleNavigateVertical(direction)
            }
            function onOverlayConfirm() {
                overlayWindow.usingGamepad = true
                content.revealVideoControls()
                content.handleConfirm()
            }
            function onOverlayFavorite() {
                overlayWindow.usingGamepad = true
                content.revealVideoControls()
                content.handleFavorite()
            }
            function onOverlayMenu() {
                overlayWindow.usingGamepad = true
                content.revealVideoControls()
                content.toggleMenu()
            }
            // L1/R1: the ONLY pad path that flips between captures in the
            // strip, and it ALWAYS flips â€” even while a clip is focused or
            // playing. Video seeking is d-pad/analog's job (onOverlayNavigate
            // above), so the two stay fully independent.
            function onOverlayGameStep(direction) {
                overlayWindow.usingGamepad = true
                content.revealVideoControls()
                content.handleCaptureStep(direction)
            }
            function onOverlayHideRequested() {
                overlayWindow.usingGamepad = true
                content.revealVideoControls()
                content.handleBack()
            }
            function onPlaybackPlayPause() {
                overlayWindow.usingGamepad = true
                content.toggleVideoPlayback()
            }
            function onPlaybackSeek(direction) {
                overlayWindow.usingGamepad = true
                content.seekVideo(direction * previewStage.seekStepMs)
            }
            // Share while a clip is focused: grab the on-screen frame as a
            // screenshot instead of the global foreground screenshot.
            function onFrameGrabRequested() {
                overlayWindow.usingGamepad = true
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
                onEntrySelected: function(index) { content.selectSidebarEntryAt(index, "confirm") }
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
                    overlayWindow.pendingDeleteRow = index
                    deleteDialog.open()
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

    // Per-capture action menu (Square / M) â€” Show in folder / Delete.
    OverlayActionMenu {
        id: actionMenu
        open: content.menuOpen
        currentIndex: content.menuIndex
        onCloseRequested: content.menuOpen = false
        onItemHovered: function(index) { content.menuIndex = index }
        onItemConfirmed: function(index) {
            content.menuIndex = index
            content.menuConfirm()
        }
    }

    // Mouse delete confirmation. Above the action menu in z-order so it stays
    // usable no matter which path opened it.
    ConfirmDialog {
        id: deleteDialog
        anchors.fill: parent
        z: 200
        //% "Delete capture?"
        title: qsTrId("gamehq.gallery.delete_capture.title")
        //% "Delete"
        confirmLabel: qsTrId("gamehq.action.delete")
        onConfirmed: {
            sounds.play("confirm")
            if (overlayWindow.pendingDeleteRow >= 0)
                app.deleteCaptureFrom(overlayGallery, overlayWindow.pendingDeleteRow)
            overlayWindow.pendingDeleteRow = -1
        }
        onCanceled: overlayWindow.pendingDeleteRow = -1
    }
}
