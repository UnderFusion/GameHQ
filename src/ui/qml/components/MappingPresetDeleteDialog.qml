import QtQuick
import GameHQ
import "../helpers/PadNav.js" as PadNav

// Delete flow for the mapping-preset library (cpo-p06).
//
// A referenced preset is never half-deleted: the user either picks the preset
// that takes over every referencing target (controller, legacy slot, game,
// group default) and the delete + reassignment happen as ONE storage
// transaction, or the dialog refuses and nothing changes. An unreferenced
// preset simply confirms.
Item {
    id: root

    property string title: ""
    property string message: ""
    // [{ id, name }] - every other preset of the same device group.
    property var candidates: []
    property string reassignToId: ""
    // How many references can actually be moved: assignments (controller, legacy
    // slot, game, group default). A migration-source reference is recovery
    // evidence for one historical key and storage refuses to reassign it, so it
    // is counted separately instead of being offered as an ordinary user.
    property int movableReferences: 0
    property int migrationReferences: 0
    //% "Move its users to"
    property string candidatesLabel: qsTrId("gamehq.settings.presets.delete.move_label")
    //% "This preset records a migrated mapping set and cannot be deleted."
    property string migrationBlockedLabel: qsTrId("gamehq.settings.presets.delete.migration_blocked")
    //% "There is no other preset in this group to move its users to. Create one first."
    property string noCandidateLabel: qsTrId("gamehq.settings.presets.delete.no_candidate")
    //% "Cancel"
    property string cancelLabel: qsTrId("gamehq.action.cancel")
    //% "Delete"
    property string confirmLabel: qsTrId("gamehq.action.delete")
    signal accepted(string reassignToId)
    signal canceled()

    visible: false
    opacity: 0
    // The Delete button is only offered when the model can actually succeed: a
    // referenced preset needs a reassign target, and a preset with no candidate
    // (or a migration record) cannot be deleted at all.
    readonly property bool needsReassign: movableReferences > 0 && migrationReferences === 0
                                                   && candidates.length > 0
    readonly property bool blocked: migrationReferences > 0
                                    || (movableReferences > 0 && candidates.length === 0)
    readonly property string blockedLabel: migrationReferences > 0
                                           ? migrationBlockedLabel : noCandidateLabel

    function open() {
        reassignToId = ""
        visible = true
        Qt.callLater(function() {
            if (!root.visible)
                return
            cancelButton.forceActiveFocus()
        })
    }
    function close() { visible = false }
    function padStep(direction) { padVertical(direction) }
    function padVertical(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!active || !PadNav.isInside(active, root)) {
            cancelButton.forceActiveFocus()
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
            cancelButton.forceActiveFocus()
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
        // A disabled action (the delete that could orphan its users) must not
        // fire through the pad either.
        if (active && active.enabled !== false && PadNav.isInside(active, root) && active.clicked)
            active.clicked()
    }
    function padBack() {
        root.canceled()
        root.close()
    }

    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    states: State {
        when: root.visible
        PropertyChanges { target: root; opacity: 1 }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            onClicked: { root.canceled(); root.close() }
        }
    }

    Rectangle {
        id: dialog
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s32, Theme.s48 * 9)
        implicitHeight: card.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusM
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: Theme.borderLight

        Column {
            id: card
            anchors.fill: parent
            anchors.margins: Theme.s24
            spacing: Theme.s16

            Text {
                width: parent.width
                text: root.title
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontTitle
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                text: root.message
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            Text {
                width: parent.width
                visible: root.needsReassign
                text: root.candidatesLabel
                color: Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }
            Text {
                width: parent.width
                visible: root.blocked
                text: root.blockedLabel
                color: Theme.warning
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }
            // The reassign picker is a list of buttons, not a dropdown: a
            // modal already owns the pad, and one row per candidate keeps the
            // choice visible while it is being made.
            Column {
                width: parent.width
                visible: root.needsReassign
                spacing: Theme.s8
                Repeater {
                    model: root.candidates
                    delegate: AccentButton {
                        required property var modelData
                        width: parent.width
                        quiet: root.reassignToId !== modelData.id
                        primary: root.reassignToId === modelData.id
                        label: modelData.name
                        onClicked: {
                            root.reassignToId = modelData.id
                            sounds.play("nav_tick")
                        }
                    }
                }
            }
            Row {
                anchors.right: parent.right
                spacing: Theme.s12
                AccentButton {
                    id: cancelButton
                    label: root.cancelLabel
                    onClicked: { root.canceled(); root.close() }
                }
                AccentButton {
                    primary: true
                    // The confirm button stays disabled until a referenced
                    // preset has somewhere to move its users to: the transaction
                    // is all-or-nothing, so "delete and orphan" is not an option.
                    // A blocked delete (migration record, no candidate left)
                    // never enables it — the model would refuse anyway.
                    enabled: !root.blocked
                             && (!root.needsReassign || root.reassignToId.length > 0)
                    label: root.confirmLabel
                    onClicked: {
                        if (!enabled)
                            return
                        root.accepted(root.reassignToId)
                        root.close()
                    }
                }
            }
        }
    }
}
