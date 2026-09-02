import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import GameHQ
import "../helpers/SidebarCategories.js" as SidebarCategories

Rectangle {
    id: root

    property var categories: []
    property bool settingsOpen: false
    property bool helpOpen: false
    property bool aboutUnread: false
    property bool updateAvailable: false
    property string availableVersion: ""
    property bool sidebarFocused: false
    property int sidebarHoverIndex: 0

    signal settingsRequested()
    signal helpRequested()
    signal aboutRequested()
    signal pageClosed()

    // The games list is the sidebar's only clipped region: keep the pad
    // cursor visible while it walks rows that sit outside the viewport.
    onSidebarHoverIndexChanged: {
        const gameIndex = sidebarHoverIndex - categories.length
        if (sidebarFocused && gameIndex >= 0 && gameIndex < app.games.length)
            gamesList.positionViewAtIndex(gameIndex, ListView.Contain)
    }

    Layout.preferredWidth: 220
    Layout.fillHeight: true
    radius: Theme.radiusL
    color: Theme.surface
    border.width: 1
    border.color: Theme.stroke

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.s12
        spacing: Theme.s4

        RowLayout {
            Layout.margins: Theme.s8
            spacing: Theme.s8

            Image {
                source: "qrc:/icons/gamehq.svg"
                Layout.preferredWidth: Theme.fontTitle
                Layout.preferredHeight: Theme.fontTitle
                sourceSize.width: Theme.fontTitle
                sourceSize.height: Theme.fontTitle
            }

            Text {
                text: Brand.name
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontTitle
                font.weight: Font.DemiBold
            }
        }

        Repeater {
            model: root.categories
            delegate: SidebarItem {
                Layout.fillWidth: true
                label: modelData.label
                glyph: modelData.glyph
                active: !root.settingsOpen && !root.helpOpen
                        && ((modelData.key === "game" && app.currentGameAvailable
                                && app.gameId === app.currentGameId && app.category !== "favorites")
                            || (modelData.key === "game_favorites" && app.currentGameAvailable
                                && app.gameId === app.currentGameId && app.category === "favorites")
                            || (modelData.key !== "game" && app.category === modelData.key && app.gameId < 0))
                sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === index
                onClicked: {
                    root.pageClosed()
                    const f = SidebarCategories.resolveFilter(modelData.key, app.currentGameId)
                    app.setGameCategory(f.category, f.gameId)
                }
            }
        }

        Text {
            //% "Games"
            text: qsTrId("gamehq.navigation.games").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
            Layout.margins: Theme.s8
            Layout.topMargin: Theme.s16
        }

        ListView {
            id: gamesList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.s4
            model: app.games
            delegate: SidebarItem {
                width: ListView.view.width
                label: modelData.name
                iconSource: modelData.iconPath ? ("file:///" + modelData.iconPath.replace(/\\/g, "/")) : ""
                active: !root.settingsOpen && !root.helpOpen && app.gameId === modelData.id
                        && !(app.currentGameAvailable && app.currentGameId === modelData.id)
                sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.categories.length + index
                onClicked: {
                    root.pageClosed()
                    app.setGame(modelData.id)
                }
            }
        }

        SidebarItem {
            Layout.fillWidth: true
            //% "Settings"
            label: qsTrId("gamehq.navigation.settings")
            glyph: "\u2699"
            active: root.settingsOpen
            sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.categories.length + app.games.length
            onClicked: root.settingsRequested()
        }

        SidebarItem {
            Layout.fillWidth: true
            //% "Help"
            label: qsTrId("gamehq.navigation.help")
            glyph: "?"
            active: root.helpOpen
            sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.categories.length + app.games.length + 1
            onClicked: root.helpRequested()
        }

        SidebarItem {
            id: aboutRow
            Layout.fillWidth: true
            Layout.bottomMargin: Theme.s4
            //% "About"
            label: qsTrId("gamehq.navigation.about")
            glyph: "\u24d8"
            trailingText: "v" + app.version
            trailingGlyph: root.updateAvailable || root.aboutUnread ? "\u25cf" : ""
            active: false
            sidebarHovered: root.sidebarFocused
                            && root.sidebarHoverIndex === root.categories.length + app.games.length + 2
            onClicked: root.aboutRequested()
        }
    }

    function focusAboutLauncher() {
        aboutRow.forceActiveFocus()
    }
}
