import QtQuick
import GameHQ

// Delete flow for the mapping-preset library (cpo-p06).
//
// A referenced preset is never half-deleted: the user either picks the preset
// that takes over every referencing target (controller, legacy slot, game,
// group default) and the delete + reassignment happen as ONE storage
// transaction, or the dialog refuses and nothing changes. An unreferenced
// preset simply confirms.
//
// Pad contract (DialogShell): Up/Down walks the candidates and buttons, Cross
// activates the focused one (never a disabled Delete), Circle cancels.
DialogShell {
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

    // The Delete button is only offered when the model can actually succeed: a
    // referenced preset needs a reassign target, and a preset with no candidate
    // (or a migration record) cannot be deleted at all.
    readonly property bool needsReassign: movableReferences > 0 && migrationReferences === 0
                                                   && candidates.length > 0
    readonly property bool blocked: migrationReferences > 0
                                    || (movableReferences > 0 && candidates.length === 0)
    readonly property string blockedLabel: migrationReferences > 0
                                           ? migrationBlockedLabel : noCandidateLabel

    landingItem: cancelButton
    padAxis: "vertical"
    cardWidth: Math.min(width - Theme.s32, Theme.s48 * 9)
    cardRadius: Theme.radiusM
    cardBorderColor: Theme.borderLight

    function open() {
        reassignToId = ""
        root.present()
    }

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
            onClicked: root.cancel()
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
