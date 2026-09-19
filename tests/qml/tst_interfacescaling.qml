import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic
import QtTest
import "../../src/ui/qml/components"

TestCase {
    name: "InterfaceScaling"
    when: windowShown
    width: 1100
    height: 700

    QtObject {
        id: mockSettings
        property var values: ({})
        signal configChanged(string key, var value)
        signal configGroupReset(string prefix)
        function config(key, fallback) { return values[key] === undefined ? fallback : values[key] }
        function setConfig(key, value) { values[key] = value; configChanged(key, value) }
    }

    Component {
        id: windowComponent
        ApplicationWindow {
            id: host
            visible: true
            width: 1024
            height: 640
            property alias surface: surface
            property alias button: button
            property alias popup: popup
            property int clicks: 0
            readonly property ScaledSurface scaleHost: ScaledSurface {
                id: surface
                parent: host.contentItem
                anchors.fill: parent
                settings: mockSettings
                configKey: "theme.main_scale"
                minimumContentWidth: 1024
                minimumContentHeight: 640
            }
            Overlay.overlay.transform: surface.scaleTransform
            Rectangle {
                parent: host.scaleHost.contentItem
                anchors.fill: parent
                z: -1
                color: "#101010"
            }
            RowLayout {
                id: layout
                parent: host.scaleHost.contentItem
                anchors.fill: parent
                Rectangle { Layout.fillWidth: true; Layout.fillHeight: true; color: "#ff0000" }
            }
            Button {
                id: button
                parent: surface.contentItem
                x: 20; y: 20; width: 100; height: 40
                text: "Open"
                onClicked: { host.clicks++; popup.open() }
                Popup {
                    id: popup
                    y: button.height
                    width: 140; height: 100
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                }
            }
        }
    }

    function init() { mockSettings.values = ({}) }

    function test_backgroundDoesNotCoverLayout() {
        const host = createTemporaryObject(windowComponent, this)
        verify(host)
        wait(100)
        const shot = grabImage(host.contentItem)
        compare(shot.red(400, 300), 255)
        compare(shot.green(400, 300), 0)
    }

    function test_presetsAndIndependentWindows() {
        const main = createTemporaryObject(windowComponent, this)
        const overlay = createTemporaryObject(windowComponent, this)
        verify(main && overlay)
        main.width = overlay.width = 3840
        main.height = overlay.height = 2160
        overlay.surface.configKey = "theme.overlay_scale"
        tryCompare(main.surface, "width", 3840)
        tryCompare(overlay.surface, "width", 3840)
        for (const percent of [100, 125, 150, 175, 200]) {
            mockSettings.setConfig("theme.main_scale", percent)
            compare(main.surface.effectiveScale, percent / 100)
            compare(main.surface.contentItem.width, 3840 / (percent / 100))
            compare(main.surface.contentItem.height, 2160 / (percent / 100))
            compare(overlay.surface.effectiveScale, 1)
        }
        mockSettings.setConfig("theme.overlay_scale", 150)
        compare(main.surface.effectiveScale, 2)
        compare(overlay.surface.effectiveScale, 1.5)
        mockSettings.values = ({})
        mockSettings.configGroupReset("theme")
        compare(main.surface.effectiveScale, 1)
        compare(overlay.surface.effectiveScale, 1)
    }

    function test_resizeAndInvalidSavedValues() {
        mockSettings.setConfig("theme.main_scale", 200)
        const host = createTemporaryObject(windowComponent, this)
        verify(host)
        compare(host.surface.requestedPercent, 200)
        compare(host.surface.effectiveScale, 1)
        host.width = 2048; host.height = 1280
        tryCompare(host.surface, "width", 2048)
        tryCompare(host.surface, "height", 1280)
        compare(host.surface.effectiveScale, 2)
        host.width = 1280; host.height = 800
        tryCompare(host.surface, "width", 1280)
        tryCompare(host.surface, "height", 800)
        compare(host.surface.effectiveScale, 1.25)
        compare(host.surface.requestedPercent, 200)
        for (const invalid of [0, -100, 300, "invalid", 133]) {
            mockSettings.setConfig("theme.main_scale", invalid)
            compare(host.surface.requestedPercent, 100)
        }
    }

    function test_scaledHitTargetsAndPopup() {
        const host = createTemporaryObject(windowComponent, this)
        verify(host)
        host.width = 2048; host.height = 1280
        tryCompare(host.surface, "width", 2048)
        tryCompare(host.surface, "height", 1280)
        mockSettings.setConfig("theme.main_scale", 200)
        tryCompare(host, "visible", true)
        host.requestActivate()
        wait(50)
        const point = host.button.mapToItem(host.contentItem, 50, 20)
        compare(point.x, 140)
        compare(point.y, 80)
        mouseClick(host.contentItem, point.x, point.y)
        compare(host.clicks, 1)
        tryCompare(host.popup, "opened", true)
        const a = host.popup.contentItem.mapToItem(host.contentItem, 0, 0)
        const b = host.popup.contentItem.mapToItem(host.contentItem, 10, 10)
        compare(b.x - a.x, 20)
        compare(b.y - a.y, 20)
        host.popup.close()
    }
}
