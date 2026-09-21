import QtQuick
import GameHQ
import "../helpers/PadNav.js" as PadNav

// "More actions" for the mapping-preset library.
//
// Rename, duplicate and delete are management actions: they are rare, they act
// on the preset being edited, and showing them as four equal buttons next to
// "New preset" made the library area read as a toolbar. They live here instead,
// one row each, so the library card carries a single primary action.
//
// The dialog decides nothing. It raises the request the section already
// declares, and the model keeps every refusal (an open draft, a preset that
// cannot be deleted) exactly where it was.
//
// Pad contract (see SettingsPage.padOverlay): Up/Down moves between the rows,
// Cross runs the focused one, Circle closes.
Item {
    id: root

    property string title: ""
    property string message: ""
    property Item anchorItem: null
    property point anchorPosition: Qt.point(0, 0)
    // Delete is refused while an edit is open, so the row is disabled rather
    // than firing an action the model would only reject.
    property bool deleteEnabled: true
    //% "Rename"
    property string renameLabel: qsTrId("gamehq.settings.presets.action.rename")
    //% "Duplicate"
    property string duplicateLabel: qsTrId("gamehq.settings.presets.action.duplicate")
    //% "Delete"
    property string deleteLabel: qsTrId("gamehq.settings.presets.action.delete")
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")

    signal renameRequested()
    signal duplicateRequested()
    signal deleteRequested()
    signal canceled()

    visible: false
    opacity: 0

    function open(anchor) {
        anchorItem = anchor || null
        anchorPosition = anchorItem ? anchorItem.mapToItem(root, 0, 0) : Qt.point(0, 0)
        visible = true
        Qt.callLater(function() {
            if (!root.visible)
                return
            renameButton.forceActiveFocus()
        })
    }
    function close() { visible = false }
    function padStep(direction) { padVertical(direction) }
    function padVertical(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!active || !PadNav.isInside(active, root)) {
            renameButton.forceActiveFocus()
            sounds.play("nav_tick")
            return
        }
        const target = PadNav.verticalTarget(root, active, direction)
        if (target) {
            target.forceActiveFocus()
            sounds.play("nav_tick")
        }
    }
    function padHorizontal(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!active || !PadNav.isInside(active, root)) {
            renameButton.forceActiveFocus()
            sounds.play("nav_tick")
            return
        }
        const target = PadNav.horizontalTarget(root, active, direction)
        if (target) {
            target.forceActiveFocus()
            sounds.play("nav_tick")
        }
    }
    function padConfirm() {
        const active = Window.window ? Window.window.activeFocusItem : null
        // A disabled row (delete while an edit is open) must not fire through
        // the pad either.
        if (active && active.enabled !== false && PadNav.isInside(active, root) && active.clicked)
            active.clicked()
    }
    function padBack() {
        root.canceled()
        root.close()
    }

    Keys.onEscapePressed: padBack()
    Keys.onUpPressed: padVertical(-1)
    Keys.onDownPressed: padVertical(1)
    onWidthChanged: if (visible) close()
    onHeightChanged: if (visible) close()

    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    states: State {
        when: root.visible
        PropertyChanges { target: root; opacity: 1 }
    }

    Rectangle {
        anchors.fill: parent
        color: "transparent"
        MouseArea {
            anchors.fill: parent
            onClicked: { root.canceled(); root.close() }
        }
    }

    Rectangle {
        id: dialog
        x: root.anchorItem
           ? Math.max(Theme.s8, Math.min(root.width - width - Theme.s8,
                                        root.anchorPosition.x + root.anchorItem.width - width))
           : Math.max(Theme.s8, (root.width - width) / 2)
        y: root.anchorItem
           ? Math.max(Theme.s8,
                      root.anchorPosition.y + root.anchorItem.height + Theme.s4 + height < root.height - Theme.s8
                      ? root.anchorPosition.y + root.anchorItem.height + Theme.s4
                      : root.anchorPosition.y - height - Theme.s4)
           : Math.max(Theme.s8, (root.height - height) / 2)
        width: Math.min(parent.width - Theme.s16, Theme.s48 * 5)
        implicitHeight: card.implicitHeight + Theme.s8 * 2
        radius: Theme.radiusM
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.borderLight

        MouseArea { anchors.fill: parent }

        Column {
            id: card
            anchors.fill: parent
            anchors.margins: Theme.s8
            spacing: Theme.s4

            Column {
                width: parent.width
                spacing: Theme.s8
                AccentButton {
                    id: renameButton
                    objectName: "presetActionRename"
                    width: parent.width
                    quiet: true
                    quietIdleBorderColor: "transparent"
                    borderColor: "transparent"
                    label: root.renameLabel
                    onClicked: { root.close(); root.renameRequested() }
                }
                AccentButton {
                    objectName: "presetActionDuplicate"
                    width: parent.width
                    quiet: true
                    quietIdleBorderColor: "transparent"
                    borderColor: "transparent"
                    label: root.duplicateLabel
                    onClicked: { root.close(); root.duplicateRequested() }
                }
                AccentButton {
                    objectName: "presetActionDelete"
                    width: parent.width
                    quiet: true
                    quietIdleBorderColor: "transparent"
                    borderColor: "transparent"
                    enabled: root.deleteEnabled
                    label: root.deleteLabel
                    onClicked: {
                        if (!enabled)
                            return
                        root.close()
                        root.deleteRequested()
                    }
                }
            }

        }
    }
}
