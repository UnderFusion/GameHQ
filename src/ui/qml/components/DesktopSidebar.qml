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
    // Presentation: the persisted mode drives the toggle label; `expanded` is
    // the effective state Main resolves from the mode, focus and hover.
    property string mode: "expanded"
    property bool expanded: true
    // Tools group: the divider toggle collapses Help/About/Support and the
    // language selector; Settings and the sidebar mode toggle stay
    // (Main owns and persists the state).
    property bool toolsCollapsed: false
    // Keep outgoing rows in the layout until their slide/fade has finished.
    property real toolsReveal: toolsCollapsed ? 0 : 1
    Behavior on toolsReveal {
        NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic }
    }
    // Flat pad rows after the games: the tools toggle, Settings, Help, About,
    // Support, the mode toggle and last the language selector.
    readonly property int toolsToggleIndex: root.categories.length + app.games.length
    readonly property int supportRowIndex: root.toolsToggleIndex + 4
    readonly property int modeRowIndex: root.supportRowIndex + 1
    // Last controller row: the language selector below the mode toggle.
    readonly property int languageRowIndex: root.modeRowIndex + 1
    readonly property alias languageCombo: sidebarLanguageCombo
    // Labels follow the animated width, so they appear once there is room
    // for them and vanish before the rail clips them.
    readonly property bool compact: root.width < Theme.sidebarCompactBelow
    // Expanded width, mouse-resizable (ui.main_sidebar_width).
    property int expandedWidth: root.savedWidth()
    function savedWidth() {
        const v = Number(app.config("ui.main_sidebar_width", Theme.sidebarWidth))
        return isFinite(v) ? Math.round(Math.max(Theme.sidebarMinWidth, Math.min(Theme.sidebarMaxWidth, v)))
                           : Theme.sidebarWidth
    }
    // A drag that leaves the sidebar still counts as inside, so Auto never
    // collapses under the pointer mid-resize.
    readonly property bool pointerInside: sidebarHover.hovered || resizeHandle.pressed
    property var externalUrlOpener: function(url) { return Qt.openUrlExternally(url) }

    signal settingsRequested()
    signal helpRequested()
    signal aboutRequested()
    signal pageClosed()
    signal modeCycleRequested()
    signal toolsToggleRequested()

    // app.games as a ListModel updated with move/insert/remove, so ListView
    // can animate a pinned game sliding to its new place. Order and indexes
    // stay identical to app.games, which Main's pad navigation relies on.
    ListModel { id: gamesModel }
    function syncGames() {
        const games = app.games
        for (let i = 0; i < games.length; ++i) {
            const g = games[i]
            let j = i
            while (j < gamesModel.count && gamesModel.get(j).gameId !== g.id) ++j
            const row = { gameId: g.id, name: g.name, iconPath: g.iconPath || "", pinned: g.pinned === true }
            if (j >= gamesModel.count) {
                gamesModel.insert(i, row)
            } else {
                if (j !== i) gamesModel.move(j, i, 1)
                gamesModel.set(i, row)
            }
        }
        if (gamesModel.count > games.length)
            gamesModel.remove(games.length, gamesModel.count - games.length)
    }
    Component.onCompleted: syncGames()
    Connections {
        target: app
        function onGamesChanged() { root.syncGames() }
    }

    // The games list is the sidebar's only clipped region: keep the pad
    // cursor visible while it walks rows that sit outside the viewport.
    onSidebarHoverIndexChanged: {
        const gameIndex = sidebarHoverIndex - categories.length
        if (sidebarFocused && gameIndex >= 0 && gameIndex < app.games.length)
            gamesList.positionViewAtIndex(gameIndex, ListView.Contain)
    }

    Layout.preferredWidth: root.expanded ? root.expandedWidth : Theme.sidebarRailWidth
    Layout.fillHeight: true
    Behavior on Layout.preferredWidth {
        enabled: !resizeHandle.pressed
        NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic }
    }
    clip: true
    radius: Theme.radiusL
    color: Theme.surface
    border.width: 1
    border.color: Theme.stroke

    HoverHandler { id: sidebarHover }

    // Mouse-only resize of the expanded width; saved on release, restored to
    // the default by a double-click. Hidden in the rail.
    SidebarResizeHandle {
        id: resizeHandle
        visible: root.expanded
        currentWidth: root.expandedWidth
        pointerAtEdge: sidebarHover.hovered
                       && (root.LayoutMirroring.enabled
                           ? sidebarHover.point.position.x <= Theme.sidebarResizeHandleWidth
                           : sidebarHover.point.position.x >= root.width - Theme.sidebarResizeHandleWidth)
        minimumWidth: Theme.sidebarMinWidth
        maximumWidth: Theme.sidebarMaxWidth
        onWidthDragged: function(width) { root.expandedWidth = width }
        onWidthCommitted: function(width) { app.setConfig("ui.main_sidebar_width", width) }
        onResetRequested: {
            root.expandedWidth = Theme.sidebarWidth
            app.setConfig("ui.main_sidebar_width", Theme.sidebarWidth)
        }
    }

    Connections {
        target: app
        function onConfigChanged(key, value) {
            if (key === "ui.main_sidebar_width" && !resizeHandle.pressed)
                root.expandedWidth = root.savedWidth()
        }
        function onConfigGroupReset(prefix) {
            root.expandedWidth = root.savedWidth()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: root.compact ? Theme.s8 : Theme.s12
        spacing: Theme.s4 / 2

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: root.compact ? 0 : Theme.s8
            Layout.topMargin: Theme.s8
            Layout.bottomMargin: Theme.s8
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
                    Layout.alignment: root.compact ? Qt.AlignHCenter : Qt.AlignLeft
                    Layout.fillWidth: root.compact
                    fillMode: Image.PreserveAspectFit
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
                    visible: !root.compact
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
                visible: !root.compact
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
                compact: root.compact
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
            visible: !root.compact
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
            model: gamesModel
            // Pinning moves a row: slide it and the rows it displaces
            // instead of snapping the whole list.
            move: Transition {
                NumberAnimation { properties: "y"; duration: Theme.durNormal; easing.type: Easing.OutCubic }
            }
            moveDisplaced: Transition {
                NumberAnimation { properties: "y"; duration: Theme.durNormal; easing.type: Easing.OutCubic }
            }
            add: Transition {
                NumberAnimation { property: "opacity"; from: 0; to: 1; duration: Theme.durFast }
            }
            addDisplaced: Transition {
                NumberAnimation { properties: "y"; duration: Theme.durNormal; easing.type: Easing.OutCubic }
            }
            removeDisplaced: Transition {
                NumberAnimation { properties: "y"; duration: Theme.durNormal; easing.type: Easing.OutCubic }
            }
            ScrollBar.vertical: AppScrollBar {
                id: gamesScrollBar
                anchors.right: parent.right
            }
            delegate: SidebarItem {
                width: ListView.view.width
                        - (gamesScrollBar.visible && gamesScrollBar.size < 1
                           ? gamesScrollBar.width + Theme.s4 : 0)
                label: model.name
                iconSource: model.iconPath ? ("file:///" + model.iconPath.replace(/\\/g, "/")) : ""
                active: !root.settingsOpen && !root.helpOpen && app.gameId === model.gameId
                        && !(app.currentGameAvailable && app.currentGameId === model.gameId)
                sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.categories.length + index
                compact: root.compact
                pinnable: true
                pinned: model.pinned
                onClicked: {
                    root.pageClosed()
                    app.setGame(model.gameId)
                }
                onPinToggled: {
                    sounds.play("nav_tick")
                    app.setGamePinned(model.gameId, !model.pinned)
                }
            }
        }

        // Divider with the tools toggle in its middle: a flat chevron that
        // points up while the tools are hidden and down while they show.
        Item {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.s8
            Layout.rightMargin: Theme.s8
            Layout.topMargin: Theme.s4
            Layout.bottomMargin: Theme.s4
            Layout.preferredHeight: toolsToggle.height

            Rectangle {
                anchors.left: parent.left
                anchors.right: toolsToggle.left
                anchors.rightMargin: Theme.s4
                anchors.verticalCenter: parent.verticalCenter
                height: Math.max(1, Theme.borderWidth)
                color: Theme.divider
            }
            Rectangle {
                anchors.left: toolsToggle.right
                anchors.leftMargin: Theme.s4
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                height: Math.max(1, Theme.borderWidth)
                color: Theme.divider
            }

            Rectangle {
                id: toolsToggle
                objectName: "sidebarToolsToggle"
                readonly property bool padHovered: root.sidebarFocused
                                                   && root.sidebarHoverIndex === root.toolsToggleIndex
                //% "Show tools"
                readonly property string showLabel: qsTrId("gamehq.navigation.tools.show")
                //% "Hide tools"
                readonly property string hideLabel: qsTrId("gamehq.navigation.tools.hide")
                anchors.centerIn: parent
                width: Theme.s24
                height: Theme.s16 + Theme.s4
                radius: height / 2
                color: toolsToggleMouse.containsMouse || toolsToggle.padHovered
                       ? Theme.surfaceAlt : "transparent"
                border.width: toolsToggle.padHovered ? 2 : 1
                border.color: toolsToggle.padHovered ? Theme.accent : Theme.divider
                Accessible.role: Accessible.Button
                Accessible.name: root.toolsCollapsed ? toolsToggle.showLabel : toolsToggle.hideLabel

                Text {
                    anchors.centerIn: parent
                    text: "\u25be"
                    rotation: root.toolsCollapsed ? 180 : 0
                    Behavior on rotation {
                        NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutCubic }
                    }
                    color: toolsToggleMouse.containsMouse || toolsToggle.padHovered
                           ? Theme.text : Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                    Behavior on rotation { NumberAnimation { duration: Theme.durFast } }
                }

                MouseArea {
                    id: toolsToggleMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.toolsToggleRequested()
                }

                ToolTip.text: toolsToggle.Accessible.name
                ToolTip.visible: toolsToggleMouse.containsMouse
                ToolTip.delay: 400
            }
        }

        Text {
            visible: !root.compact && root.toolsReveal > 0
            opacity: root.toolsReveal
            Layout.preferredHeight: implicitHeight * root.toolsReveal
            Layout.maximumHeight: Layout.preferredHeight
            clip: true
            // Reuse the reviewed Tools translation already shipped by every locale.
            //% "Tools"
            text: qsTrId("gamehq.settings.advanced.diagnostics.title").toUpperCase()
            color: Theme.textFaint
            font.family: Theme.fontFamily
            font.pixelSize: Theme.fontCaption
            font.letterSpacing: Theme.letterSpacingWide
            Layout.leftMargin: Theme.s8
            Layout.rightMargin: Theme.s8
            Layout.bottomMargin: Theme.s4 * root.toolsReveal
        }

        SidebarItem {
            Layout.fillWidth: true
            //% "Settings"
            label: qsTrId("gamehq.navigation.settings")
            glyph: "\u2699"
            active: root.settingsOpen
            sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.toolsToggleIndex + 1
            compact: root.compact
            onClicked: root.settingsRequested()
        }

        SidebarItem {
            Layout.fillWidth: true
            //% "Help"
            label: qsTrId("gamehq.navigation.help")
            glyph: "?"
            visible: root.toolsReveal > 0
            enabled: !root.toolsCollapsed
            opacity: root.toolsReveal
            Layout.preferredHeight: implicitHeight * root.toolsReveal
            Layout.maximumHeight: Layout.preferredHeight
            clip: true
            active: root.helpOpen
            sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.toolsToggleIndex + 2
            compact: root.compact
            onClicked: root.helpRequested()
        }

        SidebarItem {
            id: aboutRow
            Layout.fillWidth: true
            //% "About"
            label: qsTrId("gamehq.navigation.about")
            glyph: "\u24d8"
            trailingGlyph: root.updateAvailable || root.aboutUnread ? "\u25cf" : ""
            visible: root.toolsReveal > 0
            enabled: !root.toolsCollapsed
            opacity: root.toolsReveal
            Layout.preferredHeight: implicitHeight * root.toolsReveal
            Layout.maximumHeight: Layout.preferredHeight
            clip: true
            active: false
            sidebarHovered: root.sidebarFocused
                            && root.sidebarHoverIndex === root.toolsToggleIndex + 3
            compact: root.compact
            onClicked: root.aboutRequested()
        }

        SidebarItem {
            id: supportButton
            objectName: "supportGameHqButton"
            visible: root.toolsReveal > 0
            enabled: !root.toolsCollapsed
            opacity: root.toolsReveal
            Layout.preferredHeight: implicitHeight * root.toolsReveal
            Layout.maximumHeight: Layout.preferredHeight
            clip: true
            Layout.fillWidth: true

            //% "Support GameHQ"
            readonly property string localizedLabel: qsTrId("gamehq.navigation.support_gamehq")
            label: localizedLabel
            // Kept on the danger accent so the support row still reads as the
            // one optional, non-navigational action in the tools group.
            glyph: "\u2665"
            glyphColor: Theme.danger
            labelColor: Theme.danger
            active: false
            compact: root.compact
            Accessible.name: localizedLabel
            ToolTip.text: localizedLabel
            ToolTip.visible: supportButton.hovered
            ToolTip.delay: 500
            sidebarHovered: root.sidebarFocused && root.sidebarHoverIndex === root.supportRowIndex
            onClicked: root.externalUrlOpener(Brand.supportUrl)
        }

        SidebarModeButton {
            id: sidebarModeButton
            objectName: "sidebarModeButton"
            Layout.fillWidth: true
            Layout.topMargin: Theme.s8
            mode: root.mode
            compact: root.compact
            padHovered: root.sidebarFocused && root.sidebarHoverIndex === root.modeRowIndex
            onClicked: root.modeCycleRequested()
        }

        SettingsCombo {
            id: sidebarLanguageCombo
            // Stays in place in the rail as a language icon (so nothing jumps
            // when the sidebar narrows); the list opens at the full width.
            visible: root.toolsReveal > 0
            enabled: !root.toolsCollapsed
            opacity: root.toolsReveal
            Layout.preferredHeight: implicitHeight * root.toolsReveal
            Layout.maximumHeight: Layout.preferredHeight
            clip: true
            iconOnly: root.compact
            padFocused: root.sidebarFocused && root.sidebarHoverIndex === root.languageRowIndex
            iconText: "\u6587A"
            popupMinimumWidth: Theme.sidebarWidth - Theme.s24
            objectName: "sidebarLanguageSelector"
            Layout.fillWidth: true
            Layout.leftMargin: root.compact ? 0 : Theme.s4
            Layout.rightMargin: root.compact ? 0 : Theme.s4
            Layout.topMargin: Theme.s8 * root.toolsReveal
            frameVisible: false
            ToolTip.text: sidebarLanguageCombo.displayText
            ToolTip.visible: root.compact && sidebarLanguageCombo.hovered
                                && !sidebarLanguageCombo.popup.visible
            ToolTip.delay: 400
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

    // Pad Cross on the Support row: same link as a click.
    function openSupport() {
        root.externalUrlOpener(Brand.supportUrl)
    }

    function focusAboutLauncher() {
        aboutRow.forceActiveFocus()
    }
}
