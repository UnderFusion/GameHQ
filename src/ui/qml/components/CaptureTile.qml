import QtQuick
import QtQuick.Effects
import GameHQ

// Gallery tile: 16:9 thumbnail, PS5-style focus zoom, hover action icons.
// On hover (quick fade) it reveals delete + open-folder (bottom-left) and a
// favourite heart (bottom-right). A favourited tile shows a filled heart at all
// times. Caption (game · date) below.
Item {
    id: root
    property string thumbnail
    property string captureType: "screenshot"
    property string gameName
    property string dateText
    property bool favorite: false
    property bool selected: false
    property bool bulkMode: false
    property bool checked: false

    signal activated()               // single click — select / focus
    signal openRequested()           // double click — open in viewer
    signal requestDelete()
    signal requestOpenFolder()
    signal toggleFavoriteRequested()
    signal checkToggled(bool extendRange)

    Rectangle {
        id: card
        width: parent.width
        height: parent.width * 9 / 16
        radius: Theme.radiusM
        color: Theme.surface
        border.width: root.selected ? 2 : 1
        border.color: root.selected ? Theme.accent : Theme.stroke

        scale: root.selected ? Theme.focusScale : 1.0
        Behavior on scale { NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic } }

        // `clip: true` only clips children to the plain bounding box — it
        // does NOT respect `radius`, so the thumbnail image was always
        // square underneath. `layer.effect` is Qt's canonical way to
        // post-process a subtree's actual rendered output (unlike a
        // separate MultiEffect referencing a hidden `visible: false`
        // source, which didn't reliably re-grab the texture once the
        // async image finished loading — the tile stayed blank). Inset
        // from `card`'s edge by the border width so `card`'s own border
        // (native Rectangle rendering, already correctly rounded) stays
        // visible as a ring around the masked photo instead of being
        // painted over by it.
        Item {
            id: photoLayer
            anchors.fill: parent
            anchors.margins: card.border.width
            layer.enabled: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: photoMask
            }

            Image {
                anchors.fill: parent
                source: root.thumbnail !== "" ? "file:///" + root.thumbnail : ""
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
                visible: status === Image.Ready
            }

            // Placeholder when no thumbnail (e.g. video pre-0.5, or still generating)
            Rectangle {
                anchors.fill: parent
                visible: root.thumbnail === ""
                gradient: Gradient {
                    GradientStop { position: 0; color: Theme.surfaceAlt }
                    GradientStop { position: 1; color: Theme.surface }
                }
            }
        }

        Rectangle {
            id: photoMask
            anchors.fill: photoLayer
            radius: Math.max(0, card.radius - card.border.width)
            visible: false
            layer.enabled: true   // same as photoLayer: visible:false alone
                                   // skips the scene graph, leaving the mask
                                   // effectively empty (masks everything away).
        }

        // Big circular play badge so a video clip clearly reads as a clip.
        VideoBadge {
            anchors.centerIn: parent
            visible: root.captureType === "video"
            diameter: Math.min(card.width, card.height) * 0.32
        }

        HoverHandler { id: cardHover }
        TapHandler {
            acceptedButtons: Qt.LeftButton
            onTapped: {
                if (root.bulkMode)
                    root.checkToggled(!!(point.modifiers & Qt.ShiftModifier))
                else
                    root.activated()
            }
            onDoubleTapped: if (!root.bulkMode) root.openRequested()
        }

        Rectangle {
            visible: root.bulkMode
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: Theme.s8
            width: 24
            height: 24
            radius: Theme.radiusS
            color: root.checked ? "transparent" : Theme.surface
            border.width: 2
            border.color: root.checked ? Theme.accent : Theme.borderLight
            gradient: root.checked ? checkedGradient : null

            Gradient {
                id: checkedGradient
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.accent1 }
                GradientStop { position: 1; color: Theme.accent2 }
            }

            Text {
                anchors.centerIn: parent
                visible: root.checked
                text: "✓"
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontH3
                font.weight: Font.Bold
            }
        }

        // Bottom-left actions — revealed on hover only
        Row {
            anchors.left: parent.left
            anchors.bottom: parent.bottom
            anchors.margins: Theme.s8
            spacing: Theme.s8
            opacity: (!root.bulkMode && cardHover.hovered) ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.durFast } }

            TileIconButton {
                glyph: ""              // Delete (trash)
                glyphColor: Theme.danger
                onClicked: root.requestDelete()
            }
            TileIconButton {
                glyph: ""              // FolderOpen
                onClicked: root.requestOpenFolder()
            }
        }

        // Bottom-right favourite heart — filled + always visible when favourite,
        // otherwise an outline that appears on hover.
        TileIconButton {
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: Theme.s8
            glyph: root.favorite ? "" : ""   // HeartFill : Heart
            glyphColor: Theme.text            // filled heart is white, not red
            opacity: (!root.bulkMode && (root.favorite || cardHover.hovered)) ? 1 : 0
            visible: opacity > 0
            Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
            onClicked: root.toggleFavoriteRequested()
        }
    }

    Text {
        anchors.top: card.bottom
        anchors.topMargin: Theme.s8
        width: parent.width
            //% "%1 · %2"
            text: qsTrId("gamehq.gallery.capture_caption").arg(root.gameName).arg(root.dateText)
        color: Theme.textMuted
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
        elide: Text.ElideRight
    }
}
