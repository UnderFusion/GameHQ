import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import GameHQ

// Sidebar navigation entry — pill highlight + 3 px accent bar when active.
// `sidebarHovered` lights a 2px accent border when the cursor has been moved
// INTO the sidebar (L1 from the pad — the only entry path); this is a distinct
// visual from the steady `active` state, so the user always sees which sidebar
// row the cursor is on while the grid sits unfocused to its right.
Rectangle {
    id: root
    property string label
    property string glyph: ""          // small unicode glyph stands in for icons pre-1.0
    property string iconSource: ""
    property string trailingText: ""
    property string trailingGlyph: ""
    property color trailingGlyphColor: Theme.accent
    readonly property real gameIconSize: Theme.s12 * 1.2
    property bool active: false
    property bool sidebarHovered: false
    // Collapsed-rail presentation: only the icon/glyph stays, centred, and the
    // label moves into a tooltip. A game without an icon falls back to its
    // initial so every row keeps a visible target.
    property bool compact: false
    readonly property string shownGlyph: root.glyph !== "" ? root.glyph
                                       : root.compact && root.iconSource === "" && root.label !== ""
                                         ? root.label.charAt(0).toUpperCase() : ""
    readonly property real leadingSize: root.iconSource !== "" ? root.gameIconSize
                                      : root.shownGlyph !== "" ? Theme.s24 : 0
    // Rows that carry their own accent (Support) override these; the defaults
    // keep every navigation row on the standard muted/active palette.
    property color labelColor: active ? Theme.text : Theme.textMuted
    property color glyphColor: active ? Theme.accent : Theme.textMuted
    readonly property alias hovered: mouse.containsMouse
    // Game rows: a pin button at the trailing edge, shown on mouse or pad
    // hover and kept visible while the game is pinned.
    property bool pinnable: false
    property bool pinned: false
    readonly property bool pinShown: root.pinnable && !root.compact
                                     && (root.pinned || rowHover.hovered || root.sidebarHovered)
    signal clicked()
    signal pinToggled()

    implicitWidth: 200
    implicitHeight: Theme.s32
    height: implicitHeight
    radius: Theme.radiusS
    color: active || sidebarHovered ? Theme.surfaceAlt
         : mouse.containsMouse || root.activeFocus ? Theme.hoverTint
         : "transparent"

    // No color Behavior: it animated the active-tab highlight too, so
    // switching tabs (especially via L1/R1 pad quick-step) left the
    // previously-active item visibly fading its highlight out for ~140ms
    // instead of clearing immediately.

    activeFocusOnTab: true
    Keys.onReturnPressed: root.clicked()
    Keys.onSpacePressed: root.clicked()

    Rectangle { // active accent bar
        visible: root.active
        width: 3; height: 18; radius: 2
        anchors.verticalCenter: parent.verticalCenter
        color: Theme.accent
    }

    Row {
        id: contentRow
        anchors.verticalCenter: parent.verticalCenter
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: root.compact ? Math.max(0, (root.width - root.leadingSize) / 2) : Theme.s8
        anchors.rightMargin: root.compact ? 0 : Theme.s8 + (root.pinnable ? pinButton.width + Theme.s4 : 0)
        spacing: Theme.s8

        Rectangle {
            id: gameIconFrame
            visible: root.iconSource !== ""
            width: root.gameIconSize
            height: root.gameIconSize
            radius: Theme.s4
            color: "transparent"
            anchors.verticalCenter: parent.verticalCenter

            Item {
                id: gameIconLayer
                anchors.fill: parent
                layer.enabled: true
                layer.effect: MultiEffect {
                    maskEnabled: true
                    maskSource: gameIconMask
                }

                Image {
                    anchors.fill: parent
                    source: root.iconSource
                    sourceSize.width: root.gameIconSize
                    sourceSize.height: root.gameIconSize
                    fillMode: Image.PreserveAspectCrop
                    smooth: true
                }
            }

            Rectangle {
                id: gameIconMask
                anchors.fill: gameIconLayer
                radius: gameIconFrame.radius
                visible: false
                layer.enabled: true
            }
        }

        Item {
            visible: root.shownGlyph !== "" && root.iconSource === ""
            width: Theme.s24
            height: Theme.s24
            anchors.verticalCenter: parent.verticalCenter

            Text {
                anchors.centerIn: parent
                text: root.shownGlyph
                color: root.glyphColor
                font.pixelSize: Theme.fontBody
            }
        }
        Text {
            id: labelText
            visible: !root.compact
            text: root.label
            readonly property real leadingWidth: root.iconSource !== ""
                                                 ? root.gameIconSize + contentRow.spacing
                                                 : root.shownGlyph !== ""
                                                   ? Theme.s24 + contentRow.spacing
                                                   : 0
            readonly property real trailingWidth: trailing.visible
                                                  ? trailing.implicitWidth + contentRow.spacing
                                                  : 0
            width: Math.max(0, contentRow.width - leadingWidth - trailingWidth)
            elide: Text.ElideRight
            color: root.labelColor
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontBody
            anchors.verticalCenter: parent.verticalCenter
        }

        Row {
            id: trailing
            visible: !root.compact && (root.trailingText !== "" || root.trailingGlyph !== "")
            spacing: Theme.s8
            anchors.verticalCenter: parent.verticalCenter

            Text {
                visible: root.trailingText !== ""
                text: root.trailingText
                color: Theme.textFaint
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                visible: root.trailingGlyph !== ""
                text: root.trailingGlyph
                color: root.trailingGlyphColor
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // Focus ring — also lights when the cursor has been moved INTO the sidebar
    // (sidebarHovered) so the highlighted row is unambiguous even though no
    // QML item actually holds activeFocus while the grid keeps keyboard focus.
    Rectangle {
        anchors.fill: parent
        radius: parent.radius
        color: "transparent"
        border.width: (root.activeFocus || root.sidebarHovered) ? 2 : 0
        border.color: Theme.accent
    }

    // Compact rows keep their unread/update dot as a corner badge.
    Text {
        visible: root.compact && root.trailingGlyph !== ""
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: Theme.s4 / 2
        text: root.trailingGlyph
        color: root.trailingGlyphColor
        font.family: Theme.fontFamily
        font.pixelSize: Theme.fontCaption
    }

    ToolTip.text: root.label
    ToolTip.visible: root.compact && mouse.containsMouse
    ToolTip.delay: 400

    HoverHandler { id: rowHover }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        onClicked: root.clicked()
    }

    Rectangle {                         // pin button, above the row's click area
        id: pinButton
        objectName: "sidebarPinButton"
        visible: root.pinnable && !root.compact
        opacity: root.pinShown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
        width: Theme.s24
        height: Theme.s24
        radius: Theme.radiusS
        anchors.right: parent.right
        anchors.rightMargin: Theme.s4
        anchors.verticalCenter: parent.verticalCenter
        color: pinMouse.containsMouse ? Theme.hoverTint : "transparent"
        //% "Pin game"
        readonly property string pinLabel: qsTrId("gamehq.navigation.game.pin")
        //% "Unpin game"
        readonly property string unpinLabel: qsTrId("gamehq.navigation.game.unpin")
        Accessible.role: Accessible.Button
        Accessible.name: root.pinned ? unpinLabel : pinLabel

        PinGlyph {
            anchors.centerIn: parent
            width: Theme.s16
            height: Theme.s16
            filled: root.pinned
            color: root.pinned || pinMouse.containsMouse ? Theme.accent : Theme.textMuted
        }

        MouseArea {
            id: pinMouse
            anchors.fill: parent
            enabled: root.pinShown
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.pinToggled()
        }

        ToolTip.text: root.pinned ? unpinLabel : pinLabel
        ToolTip.visible: pinMouse.containsMouse
        ToolTip.delay: 400
    }
}
