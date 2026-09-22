import QtQuick
import QtQuick.Layouts
import GameHQ
import "../components"

// Mapping-preset library + assignment controls (cpo-p06, docs/mapping-presets.md
// section 7) for the Input settings page.
//
// The section answers three separate questions, and each one owns a card, so a
// user never has to guess which dropdown does what:
// - "Assigned preset" is what this device (and, in session, this game) runs.
//   Every choice applies immediately through the model's refresh seam, at a
//   safe input boundary.
// - "Editing preset" is where the Assignments below are written. It is a
//   library selection, not an assignment: choosing here never changes what the
//   device runs.
// - "Preset library" creates and manages presets. Only "New preset" is a
//   first-class button; rename / duplicate / delete live behind "More actions",
//   so the area carries one primary action instead of four equal ones.
//
// Dialogs live on the page (SettingsPage.padOverlay), so the section only
// raises requests; it never owns a modal.
SettingsSection {
    id: root
    titlePixelSize: Theme.fontTitle
    contentSpacing: Theme.s12
    //% "Mappings"
    eyebrow: qsTrId("gamehq.settings.presets.eyebrow")
    //% "Mapping presets"
    title: qsTrId("gamehq.settings.presets.title")
    //% "Presets are reusable mapping profiles. A device or game can be assigned to a preset."
    description: qsTrId("gamehq.settings.presets.description")

    readonly property var presets: input.mappingPresets
    signal newRequested()
    signal renameRequested()
    signal duplicateRequested()
    signal duplicateForTargetRequested()
    signal deleteRequested()
    signal moreActionsRequested(Item anchorItem)

    component PresetRow: SettingsRow {
        showDivider: false
        iconSize: Theme.s48
        iconPixelSize: Theme.s24
        iconFontFamily: "Segoe Fluent Icons"
        iconColor: Theme.text
        labelPixelSize: Theme.fontH3
        descriptionPixelSize: Theme.fontBody
    }

    // True while the preset being edited is not the one this device runs. The
    // library selection is deliberately free to roam, so the editing card says
    // so and offers the one-click way to make it the assignment.
    readonly property bool editingUnassigned: presets.selectedEditable
                                              && presets.targetAvailable
                                              && presets.selectedPresetId !== presets.assignedPresetId

    // The assignment picker: the two virtual choices first, then the library.
    // Values are the model's own tokens, so QML never handles the reserved id.
    property var assignmentOptions: []

    function rebuildOptions() {
        var list = [
            {
                //% "Follow fallback (no preset)"
                label: qsTrId("gamehq.settings.presets.assignment.fallback"),
                value: ""
            },
            {
                //% "Built-in defaults"
                label: qsTrId("gamehq.settings.presets.assignment.builtin"),
                value: "@builtin"
            }
        ]
        const entries = root.presets.presets
        for (var i = 0; i < entries.length; ++i)
            list.push({ label: entries[i].name, value: entries[i].id })
        // An assignment can point at something this group no longer lists (it was
        // deleted, or it belongs to another group). Show that state instead of
        // displaying a different option as if it were the assignment.
        const assigned = root.presets.assignedPresetId
        if (assigned.length > 0 && !root.hasOptionValue(list, assigned)) {
            list.push({
                label: root.presets.assignedPresetName.length > 0
                       ? root.presets.assignedPresetName : assigned,
                value: assigned
            })
        }
        // The picker is bound to this property, so the array must be REPLACED:
        // building a list and only returning it leaves the control empty.
        root.assignmentOptions = list
    }

    function hasOptionValue(list, value) {
        for (var i = 0; i < list.length; ++i)
            if (list[i].value === value)
                return true
        return false
    }

    // The library picker. Selecting here manages a preset (rename, duplicate,
    // delete) - it never changes what this device runs and never invalidates
    // input: only applyAssignment() does that.
    property var libraryOptions: []

    function rebuildLibraryOptions() {
        var list = []
        const entries = root.presets.presets
        for (var i = 0; i < entries.length; ++i)
            list.push({ label: entries[i].name, value: entries[i].id })
        root.libraryOptions = list
    }

    // The game picker (cpo-p07): the same three kinds of choice as the device
    // picker, but scoped to the game the user is in right now. Only visible while
    // a game is in session - a game assignment without a game would have no key
    // to write, and the model refuses it rather than guessing.
    property var gameOptions: []

    function rebuildGameOptions() {
        var list = [
            {
                //% "Follow the chain below the game"
                label: qsTrId("gamehq.settings.presets.game.fallback"),
                value: ""
            },
            {
                //% "Built-in defaults"
                label: qsTrId("gamehq.settings.presets.assignment.builtin"),
                value: "@builtin"
            }
        ]
        const entries = root.presets.presets
        for (var i = 0; i < entries.length; ++i)
            list.push({ label: entries[i].name, value: entries[i].id })
        const assigned = root.presets.gameAssignedPresetId
        if (assigned.length > 0 && !root.hasOptionValue(list, assigned)) {
            list.push({
                //% "%1 (not in this group)"
                label: qsTrId("gamehq.settings.presets.game.missing_option").arg(assigned),
                value: assigned
            })
        }
        root.gameOptions = list
    }

    function unavailableReasonText() {
        if (root.presets.targetUnavailableReason === "weak_identity") {
            //% "This controller has no stable identity yet, so a per-controller preset cannot be saved. Connect it again or edit all controllers."
            return qsTrId("gamehq.settings.presets.assignment.unavailable_weak")
        }
        //% "Choose the controller to edit before saving a per-controller preset."
        return qsTrId("gamehq.settings.presets.assignment.unavailable_unknown")
    }

    function sharingText() {
        const controllers = root.presets.controllerUses
        const games = root.presets.gameUses
        const defaults = root.presets.groupDefaultUses
        if (games > 0 && (controllers + defaults) > 0) {
            //% "Used by %1 target(s) and %2 game(s). Editing it changes all of them."
            return qsTrId("gamehq.settings.presets.sharing.both").arg(controllers + defaults).arg(games)
        }
        if (games > 0) {
            //% "Used by %1 game(s). Editing it changes all of them."
            return qsTrId("gamehq.settings.presets.sharing.games").arg(games)
        }
        //% "Used by %1 target(s). Editing it changes all of them, or duplicate it for this controller."
        return qsTrId("gamehq.settings.presets.sharing.targets").arg(controllers + defaults)
    }

    Component.onCompleted: {
        rebuildOptions()
        rebuildLibraryOptions()
        rebuildGameOptions()
        // All three pickers read their current value from `defaultValue` at
        // refresh time, so they must be pointed at the model once the options
        // exist.
        assignmentCombo.refresh()
        libraryCombo.refresh()
        gameCombo.refresh()
    }

    // Deterministic, collision-free suggestions for the name dialogs: a gamepad
    // user has no keyboard, so "accept the suggested name" must always work.
    function suggestedName(base) {
        const entries = root.presets.presets
        const taken = {}
        for (var i = 0; i < entries.length; ++i)
            taken[entries[i].name.trim().toLowerCase()] = true
        if (!taken[base.trim().toLowerCase()])
            return base
        for (var n = 2; n < 100; ++n) {
            const candidate = base + " " + n
            if (!taken[candidate.trim().toLowerCase()])
                return candidate
        }
        return base
    }

    // Every other preset of this group, for the delete dialog's reassign list.
    function reassignCandidates() {
        const entries = root.presets.presets
        const list = []
        for (var i = 0; i < entries.length; ++i) {
            if (entries[i].id === root.presets.selectedPresetId)
                continue
            list.push({ id: entries[i].id, name: entries[i].name })
        }
        return list
    }

    Connections {
        target: root.presets
        function onPresetsChanged() {
            root.rebuildOptions()
            root.rebuildLibraryOptions()
            root.rebuildGameOptions()
            // Replacing the model can reset a control's current row, so all
            // pickers are re-pointed at the model once it has consumed the list.
            Qt.callLater(function() {
                assignmentCombo.refresh()
                libraryCombo.refresh()
                gameCombo.refresh()
            })
        }
        function onAssignmentChanged() {
            root.rebuildOptions()
            assignmentCombo.refresh()
        }
        function onGameTargetChanged() {
            root.rebuildGameOptions()
            Qt.callLater(function() { gameCombo.refresh() })
        }
        function onGameAssignmentChanged() {
            root.rebuildGameOptions()
            gameCombo.refresh()
        }
        function onSelectionChanged() { libraryCombo.refresh() }
    }

    // ───────────────────────────── Assigned preset ─────────────────────────────
    // What this device - and, while a game is in session, this game - actually
    // runs. Changing anything here re-resolves the effective table.
    SettingsSection {
        objectName: "presetAssignedCard"
        showHeaderDivider: false

        PresetRow {
            objectName: "presetAssignmentRow"
            icon: "\uE7FC"
            //% "Assigned preset"
            label: qsTrId("gamehq.settings.presets.assignment.label")
            badge: root.presets.targetAvailable && !root.presets.assignedToFallback
                   //% "Used by this device"
                   ? qsTrId("gamehq.settings.presets.badge.this_device") : ""
            visible: root.presets.targetAvailable
            compact: true
            tone: root.presets.assignedPresetMissing ? "warning" : "normal"
            description: {
                if (root.presets.assignedToFallback) {
                    //% "No preset is assigned: this device follows the fallback chain."
                    return qsTrId("gamehq.settings.presets.assignment.fallback_description")
                }
                if (root.presets.assignedToBuiltin) {
                    //% "Built-in defaults replace every other layer for this device."
                    return qsTrId("gamehq.settings.presets.assignment.builtin_description")
                }
                if (root.presets.assignedPresetMissing) {
                    //% "The assigned preset no longer exists. Choose another one."
                    return qsTrId("gamehq.settings.presets.assignment.missing_description")
                }
                //% "This device uses the preset \"%1\"."
                return qsTrId("gamehq.settings.presets.assignment.preset_description")
                       .arg(root.presets.assignedPresetName)
            }
            SettingsCombo {
                id: assignmentCombo
                objectName: "presetAssignmentCombo"
                implicitWidth: Theme.s48 * 5
                implicitHeight: (Theme.s32 + Theme.s8)
                configKey: ""
                options: root.assignmentOptions
                defaultValue: root.presets.assignedPresetId
                onValueCommitted: function(value) {
                    root.presets.applyAssignment(value)
                    // A refused change (open draft, no persistable target) leaves
                    // the assignment where it was, so snap back to the truth.
                    Qt.callLater(function() { assignmentCombo.refresh() })
                }
            }
        }

        SettingsRow {
            // The game row (cpo-p07). It sits BELOW the device assignment because it
            // answers a different question - what this game runs - and it only exists
            // while a game is in session.
            objectName: "presetGameRow"
            showDivider: false
            visible: root.presets.gameAvailable
            compact: true
            //% "Mappings used by this game"
            label: qsTrId("gamehq.settings.presets.game.label")
            // Which of the five assignment states the row is in: it drives the tone
            // and the sentence below, and it is what the QML suite asserts (a state,
            // not a translated sentence).
            readonly property string assignmentState:
                root.presets.gameAssignedToFallback ? "fallback"
                : root.presets.gameAssignedToBuiltin ? "builtin"
                : root.presets.gameAssignedPresetMissing ? "missing"
                : root.presets.gameAssignedPresetWrongGroup ? "wrong_group"
                : "preset"
            // A broken row (the preset is gone, or it is another device group's
            // preset) is a warning: the chain below the game is serving, not this row.
            tone: assignmentState === "missing" || assignmentState === "wrong_group"
                  ? "warning" : "normal"
            description: {
                if (assignmentState === "fallback") {
                    //% "No preset is assigned to \"%1\": it follows this device's mappings."
                    return qsTrId("gamehq.settings.presets.game.fallback_description")
                           .arg(root.presets.gameLabel)
                }
                if (assignmentState === "builtin") {
                    //% "Built-in defaults replace every other layer while \"%1\" is running."
                    return qsTrId("gamehq.settings.presets.game.builtin_description")
                           .arg(root.presets.gameLabel)
                }
                if (assignmentState === "missing") {
                    //% "The preset assigned to this game no longer exists. Choose another one."
                    return qsTrId("gamehq.settings.presets.game.missing_description")
                }
                if (assignmentState === "wrong_group") {
                    //% "The preset assigned to this game is for a different device type. Choose another one."
                    return qsTrId("gamehq.settings.presets.game.wrong_group_description")
                }
                //% "\"%1\" uses the preset \"%2\" while it is running."
                return qsTrId("gamehq.settings.presets.game.preset_description")
                       .arg(root.presets.gameLabel)
                       .arg(root.presets.gameAssignedPresetName)
            }
            SettingsCombo {
                id: gameCombo
                objectName: "presetGameCombo"
                configKey: ""
                options: root.gameOptions
                defaultValue: root.presets.gameAssignedPresetId
                onValueCommitted: function(value) {
                    root.presets.applyGameAssignment(value)
                    // A refused change (open draft, no game in session) leaves the
                    // assignment where it was, so snap back to the truth.
                    Qt.callLater(function() { gameCombo.refresh() })
                }
            }
        }

        PresetRow {
            visible: !root.presets.targetAvailable
            icon: "\uE7FC"
            showDivider: false
            //% "Assigned preset"
            label: qsTrId("gamehq.settings.presets.assignment.label")
            compact: true
            tone: "warning"
            description: root.unavailableReasonText()
        }
    }

    // ───────────────────────────── Editing preset ─────────────────────────────
    // Where the Assignments below are written. Selecting here is a library
    // choice, never a runtime one.
    SettingsSection {
        objectName: "presetEditingCard"
        showHeaderDivider: false
        border.color: Theme.accent

        PresetRow {
            objectName: "presetEditingRow"
            icon: "\uE70F"
            iconColor: Theme.accent
            //% "Editing preset"
            label: qsTrId("gamehq.settings.presets.select.label")
            //% "Shared"
            badge: root.presets.selectedShared ? qsTrId("gamehq.settings.presets.badge.shared") : ""
            compact: true
            // An in-flight capture or an open assignment dialog is the one state
            // that refuses a preset switch, so it is called out rather than
            // letting the picker silently snap back.
            tone: root.presets.pendingEdit ? "warning" : "normal"
            description: {
                if (root.presets.presets.length === 0) {
                    //% "Create a preset to save a custom mapping set."
                    return qsTrId("gamehq.settings.presets.library.empty")
                }
                if (root.presets.pendingEdit) {
                    //% "Finish or cancel the current edit before changing presets."
                    return qsTrId("gamehq.settings.presets.library.pending_edit")
                }
                //% "Changes below will be saved to this preset."
                return qsTrId("gamehq.settings.presets.editing.helper")
            }
            SettingsCombo {
                id: libraryCombo
                objectName: "presetLibraryCombo"
                visible: root.presets.presets.length > 0
                implicitWidth: Theme.s48 * 5
                implicitHeight: (Theme.s32 + Theme.s8)
                configKey: ""
                options: root.libraryOptions
                defaultValue: root.presets.selectedPresetId
                onValueCommitted: function(value) {
                    root.presets.selectPreset(value)
                    // A refused switch (open draft) leaves the selection where it
                    // was; the picker follows the model, not the click.
                    Qt.callLater(function() { libraryCombo.refresh() })
                }
            }
        }

        // The edited preset is not the one this device runs. Saying so beats
        // leaving the user to compare two dropdown labels, and the button is the
        // one-click way to make the edit take effect.
        SettingsRow {
            objectName: "presetAssignEditedRow"
            showDivider: false
            visible: root.editingUnassigned
            compact: true
            //% "This preset is not assigned to this device yet."
            description: qsTrId("gamehq.settings.presets.editing.unassigned")
            AccentButton {
                //% "Assign to this device"
                label: qsTrId("gamehq.settings.presets.action.assign_to_device")
                primary: true
                enabled: !root.presets.pendingEdit
                onClicked: root.presets.applyAssignment(root.presets.selectedPresetId)
            }
        }

        SettingsRow {
            visible: root.presets.selectedShared
            showDivider: false
            compact: true
            //% "Shared preset"
            label: qsTrId("gamehq.settings.presets.sharing.label")
            description: root.sharingText()
            AccentButton {
                //% "Duplicate for this controller"
                label: qsTrId("gamehq.settings.presets.sharing.duplicate")
                enabled: root.presets.targetAvailable && !root.presets.pendingEdit
                onClicked: root.duplicateForTargetRequested()
            }
        }
    }

    // ───────────────────────────── Preset library ─────────────────────────────
    // One primary action. Rename / duplicate / delete are management, so they sit
    // behind "More actions" instead of competing with it.
    SettingsSection {
        objectName: "presetLibraryCard"
        showHeaderDivider: false

        PresetRow {
            objectName: "presetLibraryRow"
            icon: "\uE8F1"
            //% "Preset library"
            label: qsTrId("gamehq.settings.presets.library.label")
            compact: true
            description: {
                if (!root.presets.selectedEditable) {
                    //% "Create a preset to save a custom mapping set."
                    return qsTrId("gamehq.settings.presets.library.empty")
                }
                //% "Create and manage reusable presets."
                return qsTrId("gamehq.settings.presets.library.helper")
            }
            Row {
                spacing: Theme.s8
                AccentButton {
                    objectName: "presetNewButton"
                    //% "New preset"
                    label: qsTrId("gamehq.settings.presets.action.new")
                    icon: "+"
                    iconColor: Theme.accent
                    implicitHeight: (Theme.s32 + Theme.s8)
                    onClicked: root.newRequested()
                }
                Column {
                    spacing: Theme.s8
                    AccentButton {
                        id: moreButton
                        objectName: "presetMoreButton"
                        //% "More actions"
                        label: qsTrId("gamehq.settings.presets.action.more") + "   \u2304"
                        icon: "\u2026"
                        implicitHeight: (Theme.s32 + Theme.s8)
                        quiet: true
                        enabled: root.presets.selectedEditable
                        onClicked: root.moreActionsRequested(moreButton)
                    }
                    Text {
                        width: moreButton.width
                        text: [
                            //% "Rename"
                            qsTrId("gamehq.settings.presets.action.rename"),
                            //% "Duplicate"
                            qsTrId("gamehq.settings.presets.action.duplicate"),
                            //% "Delete"
                            qsTrId("gamehq.settings.presets.action.delete")
                        ].join(" · ")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                    }
                }
            }
        }
    }

    // Failures and refusals surface here instead of failing silently: the model
    // reports a kind plus a localized sentence.
    SettingsRow {
        visible: root.presets.notice.length > 0
        tone: root.presets.noticeKind === "write_failed"
              || root.presets.noticeKind === "rename_failed"
              || root.presets.noticeKind === "delete_failed"
              || root.presets.noticeKind === "delete_in_use" ? "warning" : "normal"
        //% "Preset"
        label: qsTrId("gamehq.settings.presets.notice.label")
        description: root.presets.notice
    }
}
