import QtQuick
import GameHQ

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
// Pad contract (see SettingsPage.padOverlay and DialogShell): Up/Down moves
// between the rows, Cross runs the focused one, Circle or Esc closes.
DialogShell {
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

    landingItem: renameButton
    padAxis: "vertical"
    escapeCancels: true
    // A menu under its button rather than a centred modal: the page stays
    // undimmed and the card opens below the anchor (above it without room).
    scrimColor: "transparent"
    cardWidth: Math.min(width - Theme.s16, Theme.s48 * 5)
    cardPadding: Theme.s8
    cardRadius: Theme.radiusM
    cardBorderColor: Theme.borderLight
    contentSpacing: Theme.s4
    cardX: root.anchorItem
           ? Math.max(Theme.s8, Math.min(root.width - root.cardWidth - Theme.s8,
                                        root.anchorPosition.x + root.anchorItem.width - root.cardWidth))
           : Math.max(Theme.s8, (root.width - root.cardWidth) / 2)
    cardY: root.anchorItem
           ? Math.max(Theme.s8,
                      root.anchorPosition.y + root.anchorItem.height + Theme.s4 + root.cardHeight < root.height - Theme.s8
                      ? root.anchorPosition.y + root.anchorItem.height + Theme.s4
                      : root.anchorPosition.y - root.cardHeight - Theme.s4)
           : Math.max(Theme.s8, (root.height - root.cardHeight) / 2)

    function open(anchor) {
        anchorItem = anchor || null
        anchorPosition = anchorItem ? anchorItem.mapToItem(root, 0, 0) : Qt.point(0, 0)
        root.present()
    }

    Keys.onUpPressed: padVertical(-1)
    Keys.onDownPressed: padVertical(1)
    onWidthChanged: if (visible) close()
    onHeightChanged: if (visible) close()

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
