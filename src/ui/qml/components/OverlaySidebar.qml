import QtQuick
import GameHQ

Rectangle {
    id: root

    property var categories: []
    property int sidebarIndex: 0
    // Presentation: `mode` labels the toggle, `expanded` is the effective state
    // OverlayWindow resolves, `modeFocused` puts the pad cursor on the toggle.
    property string mode: "expanded"
    property bool expanded: true
    property bool modeFocused: false
    // Overlay options gear: `optionsOpen` while its panel shows,
    // `optionsFocused` when the pad cursor sits on it.
    property bool optionsOpen: false
    property bool optionsFocused: false
    // Labels follow the animated width (see DesktopSidebar.compact).
    readonly property bool compact: root.width < Theme.overlaySidebarWidth * 0.75
    readonly property alias pointerInside: sidebarHover.hovered
    readonly property var categoryLabels: ({
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

    signal entrySelected(int index)
    signal modeCycleRequested()
    signal optionsRequested()

    width: root.expanded ? Theme.overlaySidebarWidth : Theme.sidebarRailWidth
    Behavior on width { NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic } }
    clip: true
    radius: Theme.radiusL
    color: Theme.panelTint
    border.width: 1
    border.color: Theme.stroke

    MouseArea { anchors.fill: parent }
    HoverHandler { id: sidebarHover }

    Column {
        anchors.fill: parent
        anchors.margins: root.compact ? Theme.s8 : Theme.s12
        spacing: Theme.s4

        Repeater {
            model: root.categories
            delegate: SidebarItem {
                width: parent.width
                label: root.categoryLabels[modelData.key] || modelData.label
                glyph: modelData.glyph
                active: root.sidebarIndex === index
                compact: root.compact
                onClicked: root.entrySelected(index)
            }
        }

        Text {
            visible: !root.compact
            //% "Games"
            text: qsTrId("gamehq.navigation.games").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
            topPadding: Theme.s16
            leftPadding: Theme.s8
        }

        Repeater {
            model: app.games
            delegate: SidebarItem {
                width: parent.width
                label: modelData.name
                iconSource: modelData.iconPath ? ("file:///" + modelData.iconPath.replace(/\\/g, "/")) : ""
                active: root.sidebarIndex === (root.categories.length + index)
                compact: root.compact
                onClicked: root.entrySelected(root.categories.length + index)
            }
        }
    }

    // Options gear, then the mode toggle directly above the brand; in the
    // rail these two icons are the only bottom elements left.
    SidebarItem {
        id: optionsButton
        objectName: "overlayOptionsButton"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: modeButton.top
        anchors.leftMargin: root.compact ? Theme.s8 : Theme.s12
        anchors.rightMargin: root.compact ? Theme.s8 : Theme.s12
        anchors.bottomMargin: Theme.s4
        //% "Overlay options"
        label: qsTrId("gamehq.overlay.layout.title")
        glyph: "⚙"
        active: root.optionsOpen
        sidebarHovered: root.optionsFocused
        compact: root.compact
        onClicked: root.optionsRequested()
    }

    SidebarModeButton {
        id: modeButton
        objectName: "overlaySidebarModeButton"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: brandBlock.visible ? brandBlock.top : parent.bottom
        anchors.leftMargin: root.compact ? Theme.s8 : Theme.s12
        anchors.rightMargin: root.compact ? Theme.s8 : Theme.s12
        anchors.bottomMargin: brandBlock.visible ? Theme.s16 : Theme.s8
        mode: root.mode
        compact: root.compact
        padHovered: root.modeFocused
        onClicked: root.modeCycleRequested()
    }

    Item {
        id: brandBlock
        visible: !root.compact
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s12
        height: brandColumn.implicitHeight

        Column {
            id: brandColumn
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: Theme.s4

            Row {
                id: brandRow
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: Theme.s8

                Image {
                    id: brandIcon
                    anchors.verticalCenter: parent.verticalCenter
                    source: "qrc:/icons/gamehq.svg"
                    width: Theme.fontTitle
                    height: Theme.fontTitle
                    sourceSize.width: Theme.fontTitle
                    sourceSize.height: Theme.fontTitle
                }

                Text {
                    id: brandLabel
                    anchors.verticalCenter: parent.verticalCenter
                    text: Brand.name
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }
            }

            Text {
                id: brandVersion
                anchors.horizontalCenter: parent.horizontalCenter
                //% "v%1"
                text: qsTrId("gamehq.format.version_short").arg(app.version)
                color: Theme.textFaint
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                font.letterSpacing: Theme.letterSpacingWide
            }
        }
    }
}
