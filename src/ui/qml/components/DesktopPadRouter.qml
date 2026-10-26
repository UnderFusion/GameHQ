import QtQuick

// Routes the desktop controller commands (InputManager desktop* and playback
// signals) for the gallery window. Ownership, highest first:
//   1. an open Share dialog is modal: it gets the commands it understands
//      and every other one is dropped;
//   2. the lightbox (its own top-level window) gets everything while shown;
//   3. otherwise the host window's pad* handlers run, and they hand the
//      command to whichever in-window modal is up (host.activeModal()).
// Pad input only arrives while one of these windows has real OS focus, so a
// focused game never loses the pad to the gallery.
Connections {
    // The gallery window (Main.qml): pad* handlers, activeShareDialog(),
    // activeModal(), usingGamepad and the page flags.
    property var host: null
    // The lightbox window.
    property var viewer: null

    function onDesktopNavigate(direction) {
        host.usingGamepad = true
        if (host.activeShareDialog())
            return
        if (viewer.visible) {
            viewer.padNavigate(direction)
            return
        }
        host.padNavigate(direction)
    }
    function onDesktopNavigateVertical(direction) {
        host.usingGamepad = true
        const share = host.activeShareDialog()
        if (share) {
            share.padNavigate(direction)
            return
        }
        if (viewer.visible) {
            viewer.padReveal()
            return
        }
        host.padNavigateVertical(direction)
    }
    function onDesktopConfirm() {
        host.usingGamepad = true
        const share = host.activeShareDialog()
        if (share) {
            share.padConfirm()
            return
        }
        if (viewer.visible) {
            viewer.padConfirm()
            return
        }
        host.padConfirm()
    }
    function onDesktopFavorite() {
        if (host.activeShareDialog())
            return   // Share is modal
        host.usingGamepad = true
        if (viewer.visible) {
            viewer.padReveal()
            viewer.toggleFavorite()
            return
        }
        host.padFavorite()
    }
    function onDesktopMenu() {
        host.usingGamepad = true
        if (host.activeShareDialog())
            return   // Share is modal
        if (viewer.visible) {
            viewer.openShare()
            return
        }
        host.padToggleMenu()
    }
    function onDesktopTabStep(direction) {
        if (host.activeShareDialog())
            return   // Share is modal
        host.usingGamepad = true
        if (viewer.visible) {
            viewer.padStep(direction)
            return
        }
        host.padTabStep(direction)
    }
    function onDesktopBack() {
        host.usingGamepad = true
        const share = host.activeShareDialog()
        if (share) {
            share.padBack()
            return
        }
        host.padBack()
    }
    function onDesktopSettings() {
        if (host.activeShareDialog())
            return   // Share is modal
        host.usingGamepad = true
        if (viewer.visible || host.activeModal())
            return
        host.openSettings()
    }
    function onDesktopZoom(direction) {
        if (host.activeShareDialog())
            return   // Share is modal
        host.usingGamepad = true
        if (viewer.visible || host.activeModal() || host.settingsOpen || host.helpOpen)
            return
        if (direction > 0)
            host.zoomIn()
        else
            host.zoomOut()
    }
    function onDesktopScroll(direction) {
        if (host.activeShareDialog())
            return   // Share is modal
        host.usingGamepad = true
        if (viewer.visible)
            return
        host.padScroll(direction)
    }
    function onDesktopBulkToggle() {
        if (host.activeShareDialog())
            return   // Share is modal
        host.usingGamepad = true
        if (viewer.visible || host.activeModal() || host.settingsOpen || host.helpOpen)
            return
        host.padBulkToggle()
    }
    function onPlaybackPlayPause() {
        if (host.activeShareDialog())
            return   // Share is modal
        if (viewer.visible)
            viewer.toggleVideoPlayback()
    }
    function onPlaybackSeek(direction) {
        if (host.activeShareDialog())
            return   // Share is modal
        if (viewer.visible)
            viewer.padNavigate(direction)
    }
    // Share on a focused clip in the lightbox: save the on-screen frame.
    function onFrameGrabRequested() {
        if (host.activeShareDialog())
            return   // Share is modal
        if (viewer.visible)
            viewer.saveCurrentFrame()
    }
}
