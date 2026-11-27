import QtQuick
import GameHQ

// The overlay owns the gallery and all input. This host creates the one
// built-in prototype surface only while selected.
Item {
    id: host
    readonly property var galleryPanel: ({ id: "gallery" })
    readonly property var testPanel: ({ id: "test", title: "Internal test panel" })
    readonly property string activePanelId: testSurface.active ? testPanel.id : galleryPanel.id
    property bool entryHighlighted: false

    signal testRequested()
    signal galleryRequested()

    function activateTest() { testSurface.active = true }
    function activateGallery() { testSurface.active = false }

    AccentButton {
        objectName: "internalTestPanelEntry"
        anchors.right: parent.right
        anchors.top: parent.top
        visible: host.activePanelId === host.galleryPanel.id
        label: host.testPanel.title
        primary: host.entryHighlighted
        onClicked: host.testRequested()
    }

    Loader {
        id: testSurface
        anchors.fill: parent
        active: false
        sourceComponent: Item {
            objectName: "internalTestPanel"

            Column {
                anchors.centerIn: parent
                spacing: Theme.s16

                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: Theme.text
                    font.family: Theme.fontFamily
                    text: host.testPanel.title
                }

                AccentButton {
                    objectName: "internalGalleryEntry"
                    anchors.horizontalCenter: parent.horizontalCenter
                    label: "Back to gallery"
                    onClicked: host.galleryRequested()
                }
            }
        }
    }
}
