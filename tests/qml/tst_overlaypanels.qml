import QtQuick
import QtTest
import "../../src/ui/qml"

TestCase {
    id: fixture
    name: "OverlayPanels"

    property var window
    property int hideRequests
    property string sharedPath
    property bool playbackActive

    QtObject {
        id: app
        property bool currentGameAvailable: false
        property int currentGameId: -1
        property var games: []
        property string version: "test"
        signal configChanged(string key, var value)
        signal configGroupReset(string prefix)
        signal currentGameChanged()
        function config(key, fallback) { return fallback }
        function setConfig(key, value) {}
        function overlayCategory(gameId) { return "all" }
    }

    ListModel {
        id: overlayGallery
        property string category: "all"
        property int gameId: -1
        function setFilter(category, gameId) {
            overlayGallery.category = category
            overlayGallery.gameId = gameId
        }
        function rowOf(path) { return path === "example.png" ? 0 : -1 }
        ListElement {
            filePath: "example.png"
            fileUrl: ""
            thumbnail: ""
            captureType: "image"
            gameName: "Fixture"
            dateText: "Fixture date"
            favorite: false
        }
    }

    QtObject {
        id: overlay
        property bool closing: false
        property bool foregroundAcquired: true
        property bool internalPanelsEnabled: false
        function hide() {
            fixture.hideRequests += 1
            fixture.window.hide()
        }
    }

    QtObject {
        id: input
        signal overlayNavigate(int direction)
        signal overlayNavigateVertical(int direction)
        signal overlayConfirm()
        signal overlayFavorite()
        signal overlayMenu()
        signal overlayGameStep(int direction)
        signal overlayHideRequested()
        signal playbackPlayPause()
        signal playbackSeek(int direction)
        signal frameGrabRequested()
        function setPlaybackActive(active) { fixture.playbackActive = active }
        function handleKeyPressed(key, modifiers, repeat) { return false }
        function handleKeyReleased(key, modifiers) { return false }
    }

    QtObject {
        id: sounds
        function play(name) {}
    }

    QtObject {
        id: languageManager
        property int layoutDirection: Qt.LeftToRight
        property int translationRevision: 0
        function formatDuration(ms) { return String(ms) }
    }

    QtObject {
        id: shareService
        property bool busy: false
        property string fileName: ""
        signal targetsChanged()
        signal finished(var result)
        function providers() { return [] }
        function open(path, gameName) {
            fixture.sharedPath = path
            return true
        }
        function close() {}
    }

    Component {
        id: overlayComponent
        OverlayWindow { width: 1200; height: 800 }
    }

    function openOverlay(enabled) {
        overlay.internalPanelsEnabled = enabled
        window = createTemporaryObject(overlayComponent, null)
        verify(window)
        window.show()
        tryCompare(window, "visible", true)
    }

    function child(name) { return findChild(window.contentItem, name) }

    function enterTestPanel() {
        input.overlayNavigateVertical(-1)
        input.overlayConfirm()
        verify(!window.galleryActive)
        verify(child("internalTestPanel"))
    }

    function init() {
        hideRequests = 0
        sharedPath = ""
        playbackActive = false
        overlay.closing = false
        overlayGallery.setProperty(0, "captureType", "image")
    }

    function cleanup() {
        if (window)
            window.hide()
        window = null
    }

    function test_defaultOffKeepsGalleryNavigation() {
        openOverlay(false)
        compare(child("internalPanelHostLoader").item, null)
        verify(child("overlayGalleryBody").visible)
        input.overlayNavigateVertical(-1)
        input.overlayConfirm()
        verify(window.galleryActive)
        compare(child("internalTestPanelEntry"), null)
        input.overlayHideRequested()
        input.overlayHideRequested()
        compare(hideRequests, 1)
    }

    function test_controllerSwitchAndRelease() {
        openOverlay(true)
        verify(child("internalTestPanelEntry"))
        compare(child("internalTestPanel"), null)
        const gallery = child("overlayGalleryBody")
        window.requestActivate()
        tryVerify(() => window.activeFocusItem !== null)
        const focus = window.activeFocusItem
        for (let i = 0; i < 30; ++i) {
            enterTestPanel()
            verify(!gallery.visible)
            verify(!playbackActive)
            compare(window.activeFocusItem, focus)
            input.overlayNavigate(1)
            input.overlayMenu()
            input.overlayFavorite()
            verify(!window.galleryActive)
            input.overlayHideRequested()
            verify(gallery.visible)
            compare(window.activeFocusItem, focus)
            tryVerify(() => child("internalTestPanel") === null)
        }
        input.overlayHideRequested()
        compare(hideRequests, 1)
        tryCompare(child("internalPanelHostLoader"), "item", null)
    }

    function test_hidingDestroysTestPanelAndStartsInGallery() {
        openOverlay(true)
        enterTestPanel()
        overlay.closing = true
        input.overlayHideRequested()
        verify(!window.galleryActive)
        window.hide()
        tryCompare(child("internalPanelHostLoader"), "item", null)
        tryVerify(() => child("internalTestPanel") === null)
        overlay.closing = false
        window.show()
        tryVerify(() => window.galleryActive && child("internalTestPanelEntry"))
        compare(hideRequests, 0)
    }

    function test_shutdownWhileTestPanelIsActive() {
        openOverlay(true)
        enterTestPanel()
        window.destroy()
        tryVerify(() => !window)
        compare(hideRequests, 0)
    }

    function test_existingShareAndModalPriority() {
        openOverlay(true)
        input.overlayMenu()
        input.overlayConfirm()
        compare(sharedPath, "example.png")
        const share = child("overlayCaptureShareDialog")
        verify(share.isOpen)
        window.openTestPanel()
        verify(window.galleryActive)
        input.overlayHideRequested()
        verify(!share.isOpen)
        enterTestPanel()
        share.openFor("second.png", "Other fixture")
        input.overlayHideRequested()
        verify(!share.isOpen)
        verify(!window.galleryActive)

        const deletion = child("overlayDeleteDialog")
        deletion.open()
        input.overlayConfirm()
        verify(!window.galleryActive)
        input.overlayHideRequested()
        verify(!deletion.visible)
        verify(!window.galleryActive)
        input.overlayHideRequested()
        verify(window.galleryActive)
        compare(hideRequests, 0)
    }

    function test_switchReleasesPlaybackRouting() {
        overlayGallery.setProperty(0, "captureType", "video")
        openOverlay(true)
        input.overlayConfirm()
        verify(playbackActive)
        enterTestPanel()
        verify(!playbackActive)
        input.frameGrabRequested()
        input.playbackPlayPause()
        verify(!playbackActive)
        input.overlayHideRequested()
        verify(window.galleryActive)
        verify(!playbackActive)
    }
}
