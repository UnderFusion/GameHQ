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
        spacing: Theme.s4 / 2

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Theme.s8
            spacing: Theme.s8

            RowLayout {
                id: brandHome
                objectName: "sidebarBrandHome"
                Layout.fillWidth: true
                spacing: Theme.s8
                Accessible.role: Accessible.Button
                //% "All"
                Accessible.name: Brand.name + " · " + qsTrId("gamehq.navigation.category.all")

                Image {
                    source: "qrc:/icons/gamehq.svg"
                    Layout.preferredWidth: Theme.fontTitle
                    Layout.preferredHeight: Theme.fontTitle
                    sourceSize.width: Theme.fontTitle
                    sourceSize.height: Theme.fontTitle
                }

                Text {
                    // fillWidth + elide: a non-fill Text cannot shrink, which made
                    // this header row's minimum width exceed the column and pushed
                    // every fillWidth row past the right padding.
                    Layout.fillWidth: true
                    text: Brand.name
                    elide: Text.ElideRight
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                }

                HoverHandler {
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: {
                        root.pageClosed()
                        app.setGameCategory("all", -1)
                    }
                }
            }

            // Version pill doubles as the About launcher; it turns green and
            // gains an arrow once an update is known, and the same About
            // dialog then opens on the update release with its install action.
            Rectangle {
                id: versionPill
                objectName: "sidebarVersionPill"
                //% "v%1"
                readonly property string versionLabel: qsTrId("gamehq.format.version_short").arg(app.version)
                // Reuse the reviewed update-status string shipped by every locale.
                // Each call keeps its own source comment so extraction stays exact.
                //% "Update available"
                readonly property string updateStatusLabel: qsTrId("gamehq.update.status.update_available")
                //% "v%1"
                readonly property string availableVersionLabel: qsTrId("gamehq.format.version_short").arg(root.availableVersion)
                readonly property string updateLabel: updateStatusLabel + " · " + availableVersionLabel
                //% "About"
                readonly property string aboutLabel: qsTrId("gamehq.navigation.about")
                implicitWidth: headerVersionRow.implicitWidth + Theme.s8 * 2
                implicitHeight: Theme.s24 - Theme.s4
                radius: Theme.radiusPill
                color: root.updateAvailable
                       ? (versionPillHover.hovered ? Theme.success : Theme.successSoft)
                       : (versionPillHover.hovered
                          ? Qt.tint(Theme.surfaceElevated, Theme.hoverTint)
                          : Theme.surfaceElevated)
                border.width: root.updateAvailable ? Theme.borderWidth : 0
                border.color: Theme.success
                Behavior on color { ColorAnimation { duration: Theme.durFast } }

                Row {
                    id: headerVersionRow
                    anchors.centerIn: parent
                    spacing: Theme.s4

                    Text {
                        visible: root.updateAvailable
                        text: "\u2191"
                        color: versionPillHover.hovered ? Theme.bg0 : Theme.success
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                        font.weight: Font.Bold
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        id: headerVersionText
                        text: versionPill.versionLabel
                        color: root.updateAvailable
                               ? (versionPillHover.hovered ? Theme.bg0 : Theme.success)
                               : Theme.textFaint
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                        font.weight: root.updateAvailable ? Font.DemiBold : Font.Normal
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Accessible.role: Accessible.Button
                Accessible.name: root.updateAvailable ? versionPill.updateLabel : versionPill.aboutLabel

                // Keep hover observation separate from click handling. A
                // MouseArea can briefly lose containsMouse during pointer-grab
                // transitions even while the cursor remains inside the pill.
                HoverHandler {
                    id: versionPillHover
                    cursorShape: Qt.PointingHandCursor
                }

                TapHandler {
                    onTapped: root.aboutRequested()
                }
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

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.s8
            Layout.rightMargin: Theme.s8
            Layout.topMargin: Theme.s8
            Layout.bottomMargin: Theme.s4
            Layout.preferredHeight: Math.max(1, Theme.borderWidth)
            color: Theme.divider
        }

        Text {
            //% "Games"
            text: qsTrId("gamehq.navigation.games").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
            Layout.margins: Theme.s8
        }

        // Only this section scrolls: the games list grows with the library
        // while the category and tools groups above/below stay pinned. The
        // scrollbar appears only once the rows no longer fit.
        ListView {
            id: gamesList
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: Theme.s4
            boundsBehavior: Flickable.StopAtBounds
            model: app.games
            ScrollBar.vertical: AppScrollBar {
                id: gamesScrollBar
                anchors.right: parent.right
            }
            delegate: SidebarItem {
                width: ListView.view.width
                        - (gamesScrollBar.visible && gamesScrollBar.size < 1
                           ? gamesScrollBar.width + Theme.s4 : 0)
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

        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.s8
            Layout.rightMargin: Theme.s8
            Layout.topMargin: Theme.s4
            Layout.bottomMargin: Theme.s4
            Layout.preferredHeight: Math.max(1, Theme.borderWidth)
            color: Theme.divider
        }

        Text {
            // Reuse the reviewed Tools translation already shipped by every locale.
            //% "Tools"
            text: qsTrId("gamehq.settings.advanced.diagnostics.title").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
            Layout.leftMargin: Theme.s8
            Layout.rightMargin: Theme.s8
            Layout.bottomMargin: Theme.s4
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

        SidebarItem {
            id: supportButton
            objectName: "supportGameHqButton"
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.s32

            //% "Support GameHQ"
            readonly property string localizedLabel: qsTrId("gamehq.navigation.support_gamehq")
            label: localizedLabel
            // Kept on the danger accent so the support row still reads as the
            // one optional, non-navigational action in the tools group.
            glyph: "\u2665"
            glyphColor: Theme.danger
            labelColor: Theme.danger
            active: false
            Accessible.name: localizedLabel
            ToolTip.text: localizedLabel
            ToolTip.visible: supportButton.hovered
            ToolTip.delay: 500
            onClicked: root.externalUrlOpener(Brand.supportUrl)
        }

        SettingsCombo {
            id: sidebarLanguageCombo
            objectName: "sidebarLanguageSelector"
            Layout.fillWidth: true
            Layout.leftMargin: Theme.s4
            Layout.rightMargin: Theme.s4
            Layout.topMargin: Theme.s8
            frameVisible: false
            defaultValue: languageManager.requestedLanguage
            options: [{
                //% "System language"
                label: qsTrId("gamehq.settings.language.system"),
                value: "system"
            }].concat(languageManager.availableLanguages.map(function (locale) {
                return { label: locale.nativeName, value: locale.tag }
            }))
            onValueCommitted: function(value) {
                languageManager.requestedLanguage = value
                sidebarLanguageCombo.refresh()
            }
        }
    }

    function focusAboutLauncher() {
        aboutRow.forceActiveFocus()
    }
}
