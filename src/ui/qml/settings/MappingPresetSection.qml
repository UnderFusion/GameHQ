import QtQuick
import QtQuick.Layouts
import GameHQ
import "../components"

// Mapping-preset library + assignment controls (cpo-p06, docs/mapping-presets.md
// section 7) for the Input settings page.
//
// The screen answers two separate questions, and the rows keep them apart:
// - "Mappings used by <target>" is the assignment (follow fallback / Built-in
//   defaults / a named preset). Every choice applies immediately through the
//   model's refresh seam, at a safe input boundary.
// - "Preset" is the library (create, rename, duplicate, delete). A rename or an
//   unused duplicate never disturbs a running gesture.
//
// Dialogs live on the page (SettingsPage.padOverlay), so the section only
// raises requests; it never owns a modal.
SettingsSection {
    id: root
    //% "Mappings"
    eyebrow: qsTrId("gamehq.settings.presets.eyebrow")
    //% "Mapping presets"
    title: qsTrId("gamehq.settings.presets.title")
    //% "Presets are global: a game never owns mappings, it only has an assignment."
    description: qsTrId("gamehq.settings.presets.description")

    readonly property var presets: input.mappingPresets
    signal newRequested()
    signal renameRequested()
    signal duplicateRequested()
    signal duplicateForTargetRequested()
    signal deleteRequested()

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

    SettingsRow {
        visible: root.presets.targetAvailable
        //% "Mappings used by this device"
        label: qsTrId("gamehq.settings.presets.assignment.label")
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
        visible: root.presets.gameAvailable
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

    SettingsRow {
        visible: !root.presets.targetAvailable
        tone: "warning"
        //% "Mappings used by this device"
        label: qsTrId("gamehq.settings.presets.assignment.label")
        description: root.unavailableReasonText()
    }

    SettingsRow {
        visible: root.presets.selectedShared
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

    // The library picker. It is the only way to reach an unassigned preset for
    // renaming, duplicating or deleting, and it is deliberately separate from
    // the assignment picker above: choosing here must not change what the device
    // runs, so it never asks the model for a runtime refresh.
    SettingsRow {
        visible: root.presets.presets.length > 0
        //% "Preset"
        label: qsTrId("gamehq.settings.presets.select.label")
        description: {
            if (root.presets.pendingEdit) {
                //% "Finish or cancel the current edit before changing presets."
                return qsTrId("gamehq.settings.presets.library.pending_edit")
            }
            if (root.presets.selectedPresetId === root.presets.assignedPresetId) {
                //% "This device uses it, so the editor saves its changes here."
                return qsTrId("gamehq.settings.presets.select.assigned")
            }
            //% "Selected for renaming, duplicating or deleting. The editor still saves to the preset this device uses."
            return qsTrId("gamehq.settings.presets.select.managing")
        }
        SettingsCombo {
            id: libraryCombo
            objectName: "presetLibraryCombo"
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

    SettingsRow {
        //% "Preset library"
        label: qsTrId("gamehq.settings.presets.library.label")
        description: {
            if (root.presets.pendingEdit) {
                //% "Finish or cancel the current edit before changing presets."
                return qsTrId("gamehq.settings.presets.library.pending_edit")
            }
            if (!root.presets.selectedEditable) {
                //% "Create a preset to save a custom mapping set."
                return qsTrId("gamehq.settings.presets.library.empty")
            }
            return root.presets.selectedPresetName
        }
        Row {
            spacing: Theme.s8
            AccentButton {
                //% "New"
                label: qsTrId("gamehq.settings.presets.action.new")
                onClicked: root.newRequested()
            }
            AccentButton {
                //% "Rename"
                label: qsTrId("gamehq.settings.presets.action.rename")
                enabled: root.presets.selectedEditable
                onClicked: root.renameRequested()
            }
            AccentButton {
                //% "Duplicate"
                label: qsTrId("gamehq.settings.presets.action.duplicate")
                enabled: root.presets.selectedEditable
                onClicked: root.duplicateRequested()
            }
            AccentButton {
                //% "Delete"
                label: qsTrId("gamehq.settings.presets.action.delete")
                quiet: true
                enabled: root.presets.selectedEditable && !root.presets.pendingEdit
                onClicked: root.deleteRequested()
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
