import QtQuick
import QtQuick.Layouts
import GameHQ
import "../components"

SettingsPage {
    id: root
    //% "Input"
    pageTitle: qsTrId("gamehq.settings.category.input")
    //% "Configure controller, keyboard, and mouse shortcuts without changing navigation behavior."
    pageDescription: qsTrId("gamehq.settings.input.description")
    readonly property var editor: input.bindingEditor

    // Whichever modal is up owns the pad (ordered by stacking: the conflict
    // and compatibility prompts sit above the assignment editor).
    padOverlay: compatibilityDialog.visible ? compatibilityDialog
              : conflictDialog.visible ? conflictDialog
              : resetProfileDialog.visible ? resetProfileDialog
              : assignmentDialog.visible ? assignmentDialog
              : null

    SettingsSection {
        //% "Devices"
        eyebrow: qsTrId("gamehq.settings.input.devices.eyebrow")
        //% "Input devices"
        title: qsTrId("gamehq.settings.input.devices.title")
        //% "Choose a device type, then select either assignment slot to capture a new input."
        description: qsTrId("gamehq.settings.input.devices.description")
        SettingsRow {
            //% "Device type"
            label: qsTrId("gamehq.settings.input.devices.type")
            description: {
                if (editor.deviceGroup === "controller")
                    return input.controllerStatus
                if (editor.deviceGroup === "keyboard") {
                    //% "Focused shortcuts and global key combinations"
                    return qsTrId("gamehq.settings.input.devices.keyboard_description")
                }
                //% "Middle, Back, and Forward mouse buttons"
                return qsTrId("gamehq.settings.input.devices.mouse_description")
            }
            SettingsSegmentedControl {
                currentValue: editor.deviceGroup
                options: [
                    //% "Controller"
                    { label: qsTrId("gamehq.settings.input.devices.controller"), value: "controller" },
                    //% "Keyboard"
                    { label: qsTrId("gamehq.settings.input.devices.keyboard"), value: "keyboard" },
                    //% "Mouse"
                    { label: qsTrId("gamehq.settings.input.devices.mouse"), value: "mouse" }
                ]
                onActivated: function(value) { editor.deviceGroup = value }
            }
        }
        SettingsRow {
            visible: editor.deviceGroup === "controller"
            //% "Controller profile"
            label: qsTrId("gamehq.settings.input.profile.label")
            description: {
                if (editor.controllerSpecific) {
                    //% "Changes apply only to %1."
                    return qsTrId("gamehq.settings.input.profile.specific_description").arg(editor.controllerName)
                }
                //% "Position-based assignments work across PlayStation, Xbox, Nintendo, and generic pads."
                return qsTrId("gamehq.settings.input.profile.shared_description")
            }
            SettingsSegmentedControl {
                currentValue: editor.controllerSpecific ? "specific" : "shared"
                options: editor.controllerSpecificAvailable
                    ? [
                        //% "All controllers"
                        { label: qsTrId("gamehq.settings.input.profile.all_controllers"), value: "shared" },
                        { label: editor.controllerName.length > 0
                            ? editor.controllerName
                            //% "This controller"
                            : qsTrId("gamehq.settings.input.profile.this_controller"), value: "specific" }
                      ]
                    : [
                        //% "All controllers"
                        { label: qsTrId("gamehq.settings.input.profile.all_controllers"), value: "shared" }
                      ]
                onActivated: function(value) { editor.controllerSpecific = value === "specific" }
            }
        }
    }

    SettingsSection {
        visible: input.controllerWarning.length > 0
        //% "Attention"
        eyebrow: qsTrId("gamehq.settings.input.hidden.eyebrow")
        //% "Controller hidden"
        title: qsTrId("gamehq.settings.input.hidden.title")
        description: input.controllerWarning
        variant: "warning"
        headerAction: Component {
            AccentButton {
                visible: input.controllerFixAvailable
                //% "Fix automatically"
                label: qsTrId("gamehq.settings.input.hidden.fix")
                primary: true
                onClicked: input.fixHiddenController()
            }
        }
    }

    // Non-blocking result of the last assignment. Deliberately its own section
    // on its own property: input.controllerWarning above is reserved for
    // HidHide/cloaked-pad state, and a routine binding notice must never
    // overwrite the one warning the user cannot diagnose on their own.
    // HardConflict never lands here — it takes over the modal dialog instead.
    SettingsSection {
        visible: editor.relationNotice.length > 0
                 && editor.relationKind !== "none"
                 && editor.relationKind !== "hard_conflict"
        // Three different failures, three different words. "Button not
        // reported" is only ever the controller/backend case — a chord Windows
        // owns and a failed write are separate kinds with their own copy.
        eyebrow: {
            switch (editor.relationKind) {
            case "context_override":
                //% "Context"
                return qsTrId("gamehq.settings.input.relation.context")
            case "conversion_required":
                //% "Compatibility"
                return qsTrId("gamehq.settings.input.relation.compatibility")
            case "unsupported_input":
                //% "Not available"
                return qsTrId("gamehq.settings.input.relation.not_available")
            case "hotkey_unavailable":
                //% "In use"
                return qsTrId("gamehq.settings.input.relation.in_use")
            case "persistence_error":
                //% "Not saved"
                return qsTrId("gamehq.settings.input.relation.not_saved")
            case "redundant":
                //% "Duplicate"
                return qsTrId("gamehq.settings.input.relation.duplicate")
            default:
                //% "Shared button"
                return qsTrId("gamehq.settings.input.relation.shared_button")
            }
        }
        title: {
            switch (editor.relationKind) {
            case "context_override":
                //% "This button changes meaning"
                return qsTrId("gamehq.settings.input.relation.context_title")
            case "conversion_required":
                //% "Assignment conversion required"
                return qsTrId("gamehq.settings.input.relation.compatibility_title")
            case "unsupported_input":
                //% "Button not reported"
                return qsTrId("gamehq.settings.input.relation.not_reported_title")
            case "hotkey_unavailable":
                //% "Shortcut already taken"
                return qsTrId("gamehq.settings.input.relation.in_use_title")
            case "persistence_error":
                //% "Could not save this binding"
                return qsTrId("gamehq.settings.input.relation.not_saved_title")
            case "redundant":
                //% "Already assigned"
                return qsTrId("gamehq.settings.input.relation.duplicate_title")
            default:
                //% "One button, several gestures"
                return qsTrId("gamehq.settings.input.relation.shared_button_title")
            }
        }
        description: editor.relationNotice
        // Context overrides and the three failure kinds are worth a second
        // look; shared gestures and duplicates are informational, so they stay
        // quiet.
        variant: editor.relationKind === "context_override"
                 || editor.relationKind === "conversion_required"
                 || editor.relationKind === "unsupported_input"
                 || editor.relationKind === "hotkey_unavailable"
                 || editor.relationKind === "persistence_error" ? "warning" : "status"
        headerAction: Component {
            AccentButton {
                //% "Dismiss"
                label: qsTrId("gamehq.action.dismiss")
                quiet: true
                onClicked: editor.dismissRelationNotice()
            }
        }
    }

    SettingsSection {
        //% "Profile"
        eyebrow: qsTrId("gamehq.settings.input.test.eyebrow")
        //% "Test and restore"
        title: qsTrId("gamehq.settings.input.test.title")
        variant: "compact"
        SettingsRow {
            //% "Last input"
            label: qsTrId("gamehq.settings.input.test.last_input")
            description: input.lastInput
            Text {
                text: editor.lastFiredAction
                color: Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
            }
        }
        SettingsRow {
            //% "Restore displayed bindings"
            label: qsTrId("gamehq.settings.input.test.restore_displayed")
            description: {
                if (editor.controllerSpecific) {
                    //% "Remove overrides for this controller only."
                    return qsTrId("gamehq.settings.input.test.restore_specific_description")
                }
                //% "Remove overrides for the selected device type and shared profile."
                return qsTrId("gamehq.settings.input.test.restore_shared_description")
            }
            AccentButton {
                //% "Restore defaults"
                label: qsTrId("gamehq.action.restore_defaults")
                quiet: true
                onClicked: resetProfileDialog.open()
            }
        }
        SettingsRow {
            visible: editor.legacyCopyAvailable
            //% "Adopt per-slot bindings"
            label: qsTrId("gamehq.settings.input.test.adopt_bindings")
            //% "Copy bindings saved for any controller in this slot to this specific controller. The originals are kept."
            description: qsTrId("gamehq.settings.input.test.adopt_bindings_description")
            AccentButton {
                //% "Copy to this controller"
                label: qsTrId("gamehq.settings.input.test.copy_to_controller")
                quiet: true
                onClicked: editor.copyLegacyOverridesToController()
            }
        }
        SettingsRow {
            //% "Identify a controller button"
            label: qsTrId("gamehq.settings.input.test.identify_button")
            description: input.probeRunning || input.probeStatus.length > 0
                         ? input.probeStatus
                         //% "Records the next 3 seconds of raw button changes — including buttons GameHQ does not recognize — into the diagnostics you can copy from Advanced."
                         : qsTrId("gamehq.settings.input.test.probe_description")
            AccentButton {
                //% "Start 3-second probe"
                label: qsTrId("gamehq.settings.input.test.start_probe")
                quiet: true
                enabled: !input.probeRunning
                onClicked: input.startButtonProbe()
            }
        }
    }

    SettingsSection {
        //% "Gestures"
        eyebrow: qsTrId("gamehq.settings.input.gestures.eyebrow")
        //% "Gesture timing"
        title: qsTrId("gamehq.settings.input.gestures.title")
        //% "How long GameHQ waits before it decides what a button press meant."
        description: qsTrId("gamehq.settings.input.gestures.description")
        SettingsRow {
            //% "Hold time"
            label: qsTrId("gamehq.settings.input.gestures.hold_time")
            //% "How long a button must be held for a hold action. A completed hold consumes the tap."
            description: qsTrId("gamehq.settings.input.gestures.hold_time_description")
            SettingsCombo {
                configKey: "input.default_hold_ms"; defaultValue: 2000
                options: [
                    //% "%1 seconds"
                    { label: qsTrId("gamehq.duration.decimal_seconds").arg(languageManager.formatDecimal(1.0, 1)), value: 1000 },
                    //% "%1 seconds"
                    { label: qsTrId("gamehq.duration.decimal_seconds").arg(languageManager.formatDecimal(1.5, 1)), value: 1500 },
                    //% "%1 seconds"
                    { label: qsTrId("gamehq.duration.decimal_seconds").arg(languageManager.formatDecimal(2.0, 1)), value: 2000 },
                    //% "%1 seconds"
                    { label: qsTrId("gamehq.duration.decimal_seconds").arg(languageManager.formatDecimal(3.0, 1)), value: 3000 }
                ]
            }
        }
        SettingsRow {
            //% "Multi-tap interval"
            label: qsTrId("gamehq.settings.input.gestures.multi_tap")
            //% "How long a single tap waits when the same button also has a double or triple tap."
            description: qsTrId("gamehq.settings.input.gestures.multi_tap_description")
            SettingsCombo {
                configKey: "input.multi_tap_interval_ms"; defaultValue: 300
                options: [
                    //% "%1 ms (fast)"
                    { label: qsTrId("gamehq.duration.milliseconds.fast").arg(languageManager.formatInteger(200)), value: 200 },
                    //% "%1 ms"
                    { label: qsTrId("gamehq.duration.milliseconds").arg(languageManager.formatInteger(300)), value: 300 },
                    //% "%1 ms"
                    { label: qsTrId("gamehq.duration.milliseconds").arg(languageManager.formatInteger(400)), value: 400 },
                    //% "%1 ms (relaxed)"
                    { label: qsTrId("gamehq.duration.milliseconds.relaxed").arg(languageManager.formatInteger(500)), value: 500 }
                ]
            }
        }
        SettingsRow {
            //% "Combination window"
            label: qsTrId("gamehq.settings.input.gestures.combination_window")
            //% "How long the first button of a combination waits for the second one."
            description: qsTrId("gamehq.settings.input.gestures.combination_window_description")
            SettingsCombo {
                configKey: "input.chord_window_ms"; defaultValue: 300
                options: [
                    //% "%1 ms (fast)"
                    { label: qsTrId("gamehq.duration.milliseconds.fast").arg(languageManager.formatInteger(200)), value: 200 },
                    //% "%1 ms"
                    { label: qsTrId("gamehq.duration.milliseconds").arg(languageManager.formatInteger(300)), value: 300 },
                    //% "%1 ms"
                    { label: qsTrId("gamehq.duration.milliseconds").arg(languageManager.formatInteger(400)), value: 400 },
                    //% "%1 ms (relaxed)"
                    { label: qsTrId("gamehq.duration.milliseconds.relaxed").arg(languageManager.formatInteger(500)), value: 500 }
                ]
            }
        }
    }

    SettingsSection {
        id: assignmentsSection
        //% "Bindings"
        eyebrow: qsTrId("gamehq.settings.input.bindings.eyebrow")
        //% "Assignments"
        title: qsTrId("gamehq.settings.input.bindings.title")
        //% "Primary and secondary slots are independent. Contexts can reuse the same input safely."
        description: qsTrId("gamehq.settings.input.bindings.description")

        Repeater {
            model: editor.rows
            delegate: Rectangle {
                id: actionCard
                property bool modified: Boolean(modelData.modified)

                Layout.fillWidth: true
                implicitHeight: actionLayout.implicitHeight + Theme.s32
                radius: Theme.radiusM
                color: modified
                       ? Qt.rgba(Theme.accent.r, Theme.accent.g, Theme.accent.b, 0.07)
                       : Theme.bg1
                border.width: Theme.borderWidth
                border.color: Theme.stroke

                Behavior on color { ColorAnimation { duration: Theme.durFast } }
                // Half-height and vertically centred so the accent bar never
                // pokes past the card's rounded left corners.
                Rectangle {
                    visible: actionCard.modified
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    width: 3
                    height: parent.height / 2
                    radius: width / 2
                    color: Theme.accent
                }

                ColumnLayout {
                    id: actionLayout
                    x: Theme.s16
                    y: Theme.s16
                    width: parent.width - Theme.s32
                    spacing: Theme.s12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s4
                            Text {
                                text: modelData.label
                                color: Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontH3
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                            Text {
                                //% "%1 · %2"
                                text: qsTrId("gamehq.settings.input.bindings.action_description")
                                    .arg(modelData.scope).arg(modelData.description)
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                        AccentButton {
                            visible: modelData.bindable
                            //% "Restore defaults"
                            label: qsTrId("gamehq.action.restore_defaults")
                            quiet: true
                            enabled: modified
                            labelColor: modified ? Theme.accent : Theme.textMuted
                            quietIdleBorderColor: modified ? Theme.accent : Theme.borderLight
                            onClicked: editor.resetAction(modelData.actionId)
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: assignmentLayout.implicitHeight + Theme.s24
                        radius: Theme.radiusM
                        color: Theme.surfaceAlt
                        border.width: Theme.borderWidth
                        border.color: Theme.stroke

                        ColumnLayout {
                            id: assignmentLayout
                            x: Theme.s12
                            y: Theme.s12
                            width: parent.width - Theme.s24
                            spacing: Theme.s8

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.s12
                                Text {
                                    //% "Input assignments"
                                    text: qsTrId("gamehq.settings.input.bindings.assignment_heading").toUpperCase()
                                    color: Theme.textFaint
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    font.letterSpacing: Theme.letterSpacingWide
                                }
                                Text {
                                    Layout.fillWidth: true
                                    //% "Both slots can be active. Select one to edit."
                                    text: qsTrId("gamehq.settings.input.bindings.assignment_hint")
                                    color: Theme.textMuted
                                    font.family: Theme.fontFamily
                                    font.pixelSize: Theme.fontCaption
                                    horizontalAlignment: Text.AlignRight
                                    wrapMode: Text.WordWrap
                                }
                            }

                            GridLayout {
                                id: slotGrid
                                Layout.fillWidth: true
                                columns: width < 640 ? 1 : 2
                                columnSpacing: Theme.s8
                                rowSpacing: Theme.s8

                                BindingCard {
                                    Layout.fillWidth: true
                                    //% "Primary"
                                    slotLabel: qsTrId("gamehq.settings.input.bindings.primary")
                                    assigned: modelData.primaryAssigned
                                    triggerLabel: modelData.primaryTrigger
                                    badgeLabel: modelData.bindable
                                        ? modelData.primaryGesture
                                        //% "Fixed"
                                        : qsTrId("gamehq.settings.input.bindings.fixed")
                                    editable: modelData.bindable
                                    changeState: modelData.primaryChangeState
                                    statusLabel: modelData.primaryStatusLabel
                                    onEditRequested: editor.openAssignmentEditor(modelData.actionId, 1)
                                    onClearRequested: editor.clearBinding(modelData.actionId, 1)
                                    onResetRequested: editor.resetBinding(modelData.actionId, 1)
                                }
                                BindingCard {
                                    Layout.fillWidth: true
                                    //% "Secondary"
                                    slotLabel: qsTrId("gamehq.settings.input.bindings.secondary")
                                    assigned: modelData.secondaryAssigned
                                    triggerLabel: modelData.secondaryTrigger
                                    badgeLabel: modelData.bindable
                                        ? modelData.secondaryGesture
                                        //% "Fixed"
                                        : qsTrId("gamehq.settings.input.bindings.fixed")
                                    editable: modelData.bindable
                                    changeState: modelData.secondaryChangeState
                                    statusLabel: modelData.secondaryStatusLabel
                                    onEditRequested: editor.openAssignmentEditor(modelData.actionId, 2)
                                    onClearRequested: editor.clearBinding(modelData.actionId, 2)
                                    onResetRequested: editor.resetBinding(modelData.actionId, 2)
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        parent: root
        anchors.fill: parent
        z: 200
        visible: editor.captureActive
        color: Theme.scrim
        focus: visible
        onVisibleChanged: if (visible) forceActiveFocus()
        Keys.onPressed: (event) => {
            event.accepted = input.handleKeyPressed(event.key, event.modifiers, event.isAutoRepeat)
        }
        Keys.onReleased: (event) => {
            event.accepted = input.handleKeyReleased(event.key, event.modifiers)
        }
        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - Theme.s48, 560)
            height: captureColumn.implicitHeight + Theme.s24 * 2
            radius: Theme.radiusL
            color: Theme.surface
            border.width: 2
            border.color: Theme.accent
            ColumnLayout {
                id: captureColumn
                anchors.fill: parent
                anchors.margins: Theme.s24
                spacing: Theme.s16
                Text {
                    //% "Waiting for input"
                    text: qsTrId("gamehq.settings.input.capture.waiting")
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                }
                Text {
                    text: editor.capturePrompt
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontBody
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                AccentButton {
                    //% "Cancel"
                    label: qsTrId("gamehq.action.cancel")
                    onClicked: editor.cancelCapture()
                }
            }
        }
    }

    BindingAssignmentDialog {
        id: assignmentDialog
        parent: root
        anchors.fill: parent
        z: 205
        model: editor
    }

    BindingConflictDialog {
        id: conflictDialog
        parent: root
        anchors.fill: parent
        z: 210
        message: editor.conflictMessage
        onReplaced: editor.confirmConflict()
        // Re-arms capture on the same action and slot, so the user can pick a
        // different button without hunting for the row again.
        onRetried: editor.retryConflictCapture()
        onCanceled: editor.dismissConflict()
    }
    Connections {
        target: editor
        function onConflictChanged() {
            if (editor.conflictPending) conflictDialog.open()
            else conflictDialog.close()
        }
    }

    SettingsSection {
        //% "Modern controllers"
        eyebrow: qsTrId("gamehq.settings.input.modern.eyebrow")
        //% "GameInput support"
        title: qsTrId("gamehq.settings.input.modern.title")
        description: input.modernLayoutWarning
                     //% "A controller layout changed. Review its extra-button assignments before using them."
                     ? qsTrId("gamehq.settings.input.modern.layout_warning")
                     : input.modernControllerSummary
        variant: input.modernLayoutWarning ? "warning" : "status"
        SettingsRow {
            //% "Modern controller support"
            label: qsTrId("gamehq.settings.input.modern.support.label")
            //% "Auto uses app-local GameInput with safe legacy fallback; Off keeps only the legacy providers."
            description: qsTrId("gamehq.settings.input.modern.support.description")
            SettingsCombo {
                configKey: "input.modern_controller_support"; defaultValue: "auto"
                options: [
                    //% "Auto"
                    { label: qsTrId("gamehq.settings.input.modern.auto"), value: "auto" },
                    //% "Off"
                    { label: qsTrId("gamehq.settings.input.modern.off"), value: "off" }
                ]
            }
        }
        Repeater {
            model: input.modernLayoutWarnings
            delegate: SettingsRow {
                required property var modelData
                //% "%1 button layout changed"
                label: qsTrId("gamehq.settings.input.modern.device_layout_changed").arg(modelData.displayName)
                description: modelData.description
                AccentButton {
                    //% "Review buttons"
                    label: qsTrId("gamehq.settings.input.modern.review_buttons")
                    quiet: true
                    onClicked: input.startButtonProbe()
                }
                AccentButton {
                    //% "Use current layout"
                    label: qsTrId("gamehq.settings.input.modern.use_current_layout")
                    onClicked: input.confirmModernControllerLayout(modelData.logicalId)
                }
            }
        }
        SettingsRow {
            //% "GameInput runtime"
            label: qsTrId("gamehq.settings.input.modern.runtime")
            description: input.modernControllerStatus
            Text {
                text: {
                    if (input.modernControllerStatus.indexOf("fallback") >= 0) {
                        //% "Legacy fallback"
                        return qsTrId("gamehq.settings.input.modern.legacy_fallback")
                    }
                    //% "Ready"
                    return qsTrId("gamehq.settings.advanced.status.ready")
                }
                color: input.modernControllerStatus.indexOf("fallback") >= 0 ? Theme.warning : Theme.accent
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
            }
        }
        SettingsRow {
            //% "Compatibility report"
            label: qsTrId("gamehq.settings.input.modern.report.label")
            //% "Copies anonymous identity, providers, Share/Guide availability, extra buttons, and layout state—never serials or full device paths."
            description: qsTrId("gamehq.settings.input.modern.report.description")
            AccentButton {
                //% "Copy report"
                label: qsTrId("gamehq.settings.input.modern.report.copy")
                quiet: true
                onClicked: input.copyControllerCompatibilityReport()
            }
        }
        SettingsLinkRow {
            icon: "?"
            //% "Controller compatibility guide"
            label: qsTrId("gamehq.settings.input.modern.guide.label")
            //% "View versus true Share, probe results, reconnects, gestures, and combinations"
            description: qsTrId("gamehq.settings.input.modern.guide.description")
            showDivider: false
            onClicked: Qt.openUrlExternally(Brand.repositoryUrl + "/blob/dev/docs/controller-compatibility.md")
        }
    }

    BindingCompatibilityDialog {
        id: compatibilityDialog
        parent: root
        anchors.fill: parent
        z: 211
        message: editor.compatibilityMessage
        onConverted: editor.confirmCompatibility()
        onRetried: editor.retryCompatibilityCapture()
        onCanceled: editor.dismissCompatibility()
    }
    Connections {
        target: editor
        function onCompatibilityChanged() {
            if (editor.compatibilityPending) compatibilityDialog.open()
            else compatibilityDialog.close()
        }
    }

    ConfirmDialog {
        id: resetProfileDialog
        parent: root
        anchors.fill: parent
        z: 210
        //% "Restore displayed bindings?"
        title: qsTrId("gamehq.settings.input.restore_displayed.title")
        //% "Only the currently displayed device/profile overrides will be removed."
        message: qsTrId("gamehq.settings.input.restore_displayed.message")
        //% "Restore defaults"
        confirmLabel: qsTrId("gamehq.action.restore_defaults")
        onConfirmed: editor.resetCurrentProfile()
    }
}
