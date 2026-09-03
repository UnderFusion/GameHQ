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
    property var externalUrlOpener: function(url) { return Qt.openUrlExternally(url) }

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
            //% "About"
            label: qsTrId("gamehq.navigation.about")
            glyph: "\u24d8"
            trailingGlyph: root.updateAvailable || root.aboutUnread ? "\u25cf" : ""
            active: false
            sidebarHovered: root.sidebarFocused
                            && root.sidebarHoverIndex === root.categories.length + app.games.length + 2
            onClicked: root.aboutRequested()
        }

        Text {
            id: versionLabel
            Layout.fillWidth: true
            Layout.leftMargin: Theme.s8
            Layout.rightMargin: Theme.s8
            //% "v%1"
            text: qsTrId("gamehq.format.version_short").arg(app.version)
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        Button {
            id: supportButton
            objectName: "supportGameHqButton"
            Layout.fillWidth: true
            Layout.preferredHeight: 36
            Layout.topMargin: Theme.s4
            Layout.bottomMargin: Theme.s4
            leftPadding: Theme.s12
            rightPadding: Theme.s12
            activeFocusOnTab: true

            //% "Support GameHQ"
            readonly property string localizedLabel: qsTrId("gamehq.navigation.support_gamehq")
            Accessible.name: localizedLabel
            ToolTip.text: localizedLabel
            ToolTip.visible: hovered
            ToolTip.delay: 500
            onClicked: root.externalUrlOpener(Brand.supportUrl)

            background: Rectangle {
                radius: Theme.radiusS
                color: supportButton.down
                       ? Qt.darker(Theme.danger, 1.18)
                       : supportButton.hovered || supportButton.activeFocus
                         ? Qt.lighter(Theme.danger, 1.08)
                         : Theme.danger
                border.width: supportButton.activeFocus ? 2 : 0
                border.color: Theme.text
            }

            contentItem: Row {
                spacing: Theme.s8

                Text {
                    id: supportGlyph
                    text: "\u2615"
                    color: Theme.textOnAccent
                    font.pixelSize: Theme.fontBody
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    width: Math.max(0, supportButton.availableWidth
                                    - supportGlyph.implicitWidth - parent.spacing)
                    text: supportButton.localizedLabel
                    color: Theme.textOnAccent
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                }
            }
        }
    }

    function focusAboutLauncher() {
        aboutRow.forceActiveFocus()
    }
}
