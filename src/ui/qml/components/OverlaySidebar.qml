import QtQuick
import GameHQ

Rectangle {
    id: root

    property var categories: []
    property int sidebarIndex: 0
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

    width: 260
    radius: Theme.radiusL
    color: Theme.panelTint
    border.width: 1
    border.color: Theme.stroke

    MouseArea { anchors.fill: parent }

    Column {
        anchors.fill: parent
        anchors.margins: Theme.s12
        spacing: Theme.s4

        Repeater {
            model: root.categories
            delegate: SidebarItem {
                width: parent.width
                label: root.categoryLabels[modelData.key] || modelData.label
                glyph: modelData.glyph
                active: root.sidebarIndex === index
                onClicked: root.entrySelected(index)
            }
        }

        Text {
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
                        && !(app.currentGameAvailable && app.currentGameId === modelData.id)
                onClicked: root.entrySelected(root.categories.length + index)
            }
        }
    }

    Item {
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
