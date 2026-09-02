import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC
import GameHQ
import "../helpers/PadNav.js" as PadNav

// A draft editor for the complete assignment. Stored values are presented as
// fields; only the small Change/Record controls start live input capture.
FocusScope {
    id: root

    property var model: null   // BindingEditorModel

    visible: model && model.editorOpen
    opacity: visible ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }

    readonly property bool capturing: model && model.editorCaptureStep !== "idle"
    readonly property bool combination: model && model.editorTriggerKind === "combination"
    readonly property string noticeKind: model ? model.editorNoticeKind : "none"
    readonly property bool noticeDanger: noticeKind === "hard_conflict"
                                          || noticeKind === "invalid_pattern"
    readonly property bool noticeWarning: noticeKind === "unsupported_input"
                                           || noticeKind === "conversion_required"

    function gestureValue() {
        if (!model)
            return ""
        if (model.editorGestureKind === "tap")
            return "tap:" + model.editorTapCount
        return model.editorGestureKind + ":1"
    }

    function selectGesture(value) {
        const parts = value.split(":")
        const kind = parts[0]
        const taps = Number(parts[1])
        model.setEditorGesture(kind, taps, kind === "hold" ? model.editorHoldMs : 0)
    }

    onVisibleChanged: {
        if (!visible)
            return
        dialogViewport.contentY = 0
        Qt.callLater(function() {
            const controls = PadNav.focusables(root)
            if (controls.length > 0)
                controls[0].forceActiveFocus()
        })
    }

    Keys.onEscapePressed: function(event) {
        if (root.capturing)
            root.model.cancelTriggerCapture()
        else
            root.model.closeAssignmentEditor()
        event.accepted = true
    }

    // ───────────────── Pad navigation ─────────────────
    // While the dialog is up it owns the pad completely (routed here through
    // SettingsPage.padOverlay): directions move only between the dialog's own
    // controls, Cross activates the focused one, and Circle is the only way
    // out — cancel a running capture first, then close the draft.
    function revealFocused() {
        const item = Window.window ? Window.window.activeFocusItem : null
        if (!item || !PadNav.isInside(item, root))
            return
        const point = item.mapToItem(dialogContent, 0, 0)
        const top = Math.max(0, point.y - Theme.s16)
        const bottom = point.y + item.height + Theme.s16
        if (top < dialogViewport.contentY)
            dialogViewport.contentY = top
        else if (bottom > dialogViewport.contentY + dialogViewport.height)
            dialogViewport.contentY = Math.min(
                Math.max(0, dialogViewport.contentHeight - dialogViewport.height),
                bottom - dialogViewport.height)
    }

    function padStep(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        const target = PadNav.verticalTarget(root, active, direction)
        if (target) {
            target.forceActiveFocus()
            revealFocused()
            sounds.play("nav_tick")
        }
    }

    function padHorizontal(direction) {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (!active || !PadNav.isInside(active, root)) {
            padStep(1)
            return
        }
        const target = PadNav.horizontalTarget(root, active, direction)
        if (target) {
            target.forceActiveFocus()
            revealFocused()
            sounds.play("nav_tick")
        }
    }

    function padConfirm() {
        const active = Window.window ? Window.window.activeFocusItem : null
        if (active && PadNav.isInside(active, root) && active.clicked) {
            active.clicked()
            sounds.play("confirm")
        }
    }

    function padBack() {
        if (root.capturing)
            root.model.cancelTriggerCapture()
        else
            root.model.closeAssignmentEditor()
    }

    // Wheel-like pad scroll (right stick) of the dialog's own viewport.
    function scrollBy(direction) {
        const maximum = Math.max(0, dialogViewport.contentHeight - dialogViewport.height)
        dialogViewport.contentY = Math.max(0, Math.min(maximum,
            dialogViewport.contentY + direction * Math.max(80, dialogViewport.height * 0.28)))
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            // Clicking away discards only the draft; saved bindings stay put.
            onClicked: root.model.closeAssignmentEditor()
        }
    }

    Rectangle {
        id: dialogPanel
        anchors.centerIn: parent
        width: Math.min(parent.width - Theme.s16 * 2,
                        Theme.dialogWidth + Theme.s48 + Theme.s32)
        height: Math.min(parent.height - Theme.s16 * 2,
                         dialogContent.implicitHeight + Theme.s48)
        radius: Theme.radiusL
        color: Theme.surface
        border.width: root.capturing ? Theme.borderWidth + 1 : Theme.borderWidth
        border.color: root.capturing ? Theme.accent : Theme.stroke

        MouseArea { anchors.fill: parent }   // swallow clicks behind the panel

        Flickable {
            id: dialogViewport
            anchors.fill: parent
            anchors.margins: Theme.s24
            contentWidth: width
            contentHeight: dialogContent.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            flickableDirection: Flickable.VerticalFlick
            QC.ScrollBar.vertical: AppScrollBar {}

            ColumnLayout {
                id: dialogContent
                width: dialogViewport.width
                spacing: Theme.s16

                Text {
                    Layout.fillWidth: true
                    text: root.model ? root.model.editorActionLabel : ""
                    color: Theme.text
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontTitle
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Text {
                    Layout.fillWidth: true
                    text: {
                        if (!root.model)
                            return ""
                        //% "%1 · Slot %2"
                        return qsTrId("gamehq.input.assignment.scope_slot")
                            .arg(root.model.editorScopeLabel).arg(root.model.editorSlot)
                    }
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: Theme.fontCaption
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s8
                    visible: root.model && root.model.editorCombinationAvailable

                    Text {
                        //% "Pattern"
                        text: qsTrId("gamehq.input.assignment.pattern")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    BindingSegmentedControl {
                        Layout.fillWidth: true
                        currentValue: root.combination ? "combination" : "single"
                        options: [
                            //% "Single button"
                            { label: qsTrId("gamehq.input.assignment.pattern.single"), value: "single" },
                            //% "Combination"
                            { label: qsTrId("gamehq.input.assignment.pattern.combination"), value: "combination" }
                        ]
                        onActivated: function(value) { root.model.setEditorTriggerKind(value) }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: captureModeText.implicitHeight + Theme.s12
                    visible: root.capturing
                    radius: Theme.radiusS
                    color: Theme.accentSoft
                    border.width: Theme.borderWidth
                    border.color: Theme.accent

                    Text {
                        id: captureModeText
                        anchors.centerIn: parent
                        //% "Controller capture active · Dialog navigation is paused"
                        text: qsTrId("gamehq.input.assignment.capture_active").toUpperCase()
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                        font.letterSpacing: Theme.letterSpacingWide
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                // Single-button input: one value row with one compact action.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s8
                    visible: !root.combination

                    Text {
                        //% "Input"
                        text: qsTrId("gamehq.settings.category.input")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.s48
                        radius: Theme.radiusS
                        color: root.capturing ? Theme.accentSoft : Theme.bg1
                        border.width: root.capturing ? Theme.borderWidth + 1 : Theme.borderWidth
                        border.color: root.capturing ? Theme.accent : Theme.stroke

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s12
                            anchors.rightMargin: Theme.s8
                            spacing: Theme.s8

                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: {
                                    if (root.capturing) {
                                        //% "Listening… Press a controller button"
                                        return qsTrId("gamehq.input.assignment.listening_button")
                                    }
                                    if (root.model && root.model.editorFirstControlLabel !== "")
                                        return root.model.editorFirstControlLabel
                                    //% "Not set"
                                    return qsTrId("gamehq.input.assignment.not_set")
                                }
                                color: root.capturing ? Theme.accent : Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            AccentButton {
                                quiet: true
                                label: {
                                    if (root.capturing) {
                                        //% "Stop"
                                        return qsTrId("gamehq.action.stop")
                                    }
                                    if (root.model && root.model.editorFirstControlLabel !== "") {
                                        //% "Change"
                                        return qsTrId("gamehq.common.action.change")
                                    }
                                    //% "Record"
                                    return qsTrId("gamehq.action.record")
                                }
                                onClicked: {
                                    if (root.capturing) root.model.cancelTriggerCapture()
                                    else root.model.beginTriggerCapture(1)
                                }
                            }
                        }
                    }
                }

                // Combinations expose both ordered controls and make capture
                // progress explicit instead of flattening it into one label.
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s8
                    visible: root.combination

                    Text {
                        //% "First button"
                        text: qsTrId("gamehq.input.assignment.first_button")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.s48
                        radius: Theme.radiusS
                        color: root.model && root.model.editorCaptureStep === "first"
                               ? Theme.accentSoft : Theme.bg1
                        border.width: root.model && root.model.editorCaptureStep === "first"
                                      ? Theme.borderWidth + 1 : Theme.borderWidth
                        border.color: root.model && root.model.editorCaptureStep === "first"
                                      ? Theme.accent : Theme.stroke

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s12
                            anchors.rightMargin: Theme.s8
                            spacing: Theme.s8
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: {
                                    if (root.model && root.model.editorCaptureStep === "first") {
                                        //% "Listening… Hold the first button"
                                        return qsTrId("gamehq.input.assignment.listening_first")
                                    }
                                    if (root.model && root.model.editorFirstControlLabel !== "") {
                                        if (root.model.editorCaptureStep === "second") {
                                            //% "%1 detected"
                                            return qsTrId("gamehq.input.assignment.detected")
                                                .arg(root.model.editorFirstControlLabel)
                                        }
                                        return root.model.editorFirstControlLabel
                                    }
                                    //% "Not set"
                                    return qsTrId("gamehq.input.assignment.not_set")
                                }
                                color: root.model && root.model.editorCaptureStep === "first"
                                       ? Theme.accent : Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            AccentButton {
                                visible: !root.capturing
                                quiet: true
                                label: {
                                    if (root.model && root.model.editorFirstControlLabel !== "") {
                                        //% "Change"
                                        return qsTrId("gamehq.common.action.change")
                                    }
                                    //% "Record"
                                    return qsTrId("gamehq.action.record")
                                }
                                onClicked: root.model.beginTriggerCapture(1)
                            }
                        }
                    }

                    Text {
                        //% "Second button"
                        text: qsTrId("gamehq.input.assignment.second_button")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        implicitHeight: Theme.s48
                        radius: Theme.radiusS
                        color: root.model && root.model.editorCaptureStep === "second"
                               ? Theme.accentSoft : Theme.bg1
                        border.width: root.model && root.model.editorCaptureStep === "second"
                                      ? Theme.borderWidth + 1 : Theme.borderWidth
                        border.color: root.model && root.model.editorCaptureStep === "second"
                                      ? Theme.accent : Theme.stroke

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: Theme.s12
                            anchors.rightMargin: Theme.s8
                            spacing: Theme.s8
                            Text {
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: {
                                    if (root.model && root.model.editorCaptureStep === "second") {
                                        //% "Listening… Press the second button"
                                        return qsTrId("gamehq.input.assignment.listening_second")
                                    }
                                    if (root.model && root.model.editorSecondControlLabel !== "")
                                        return root.model.editorSecondControlLabel
                                    if (root.model && root.model.editorFirstControlLabel === "") {
                                        //% "Waiting for first button"
                                        return qsTrId("gamehq.input.assignment.waiting_first")
                                    }
                                    //% "Not set"
                                    return qsTrId("gamehq.input.assignment.not_set")
                                }
                                color: root.model && root.model.editorCaptureStep === "second"
                                       ? Theme.accent : Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            AccentButton {
                                visible: !root.capturing
                                quiet: true
                                enabled: root.model && root.model.editorFirstControlLabel !== ""
                                label: {
                                    if (root.model && root.model.editorSecondControlLabel !== "") {
                                        //% "Change"
                                        return qsTrId("gamehq.common.action.change")
                                    }
                                    //% "Record"
                                    return qsTrId("gamehq.action.record")
                                }
                                onClicked: root.model.beginTriggerCapture(2)
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: !root.capturing && root.model
                                 && root.model.editorTriggerHint !== ""
                        text: root.model ? root.model.editorTriggerHint : ""
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                        wrapMode: Text.WordWrap
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s8
                    visible: root.model && !root.model.editorGestureLocked

                    Text {
                        //% "Gesture"
                        text: qsTrId("gamehq.input.assignment.gesture")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    BindingSegmentedControl {
                        Layout.fillWidth: true
                        currentValue: root.gestureValue()
                        options: [
                            //% "Press"
                            { label: qsTrId("gamehq.input.gesture.press"), value: "press:1" },
                            //% "Tap"
                            { label: qsTrId("gamehq.input.gesture.tap"), value: "tap:1" },
                            //% "Double tap"
                            { label: qsTrId("gamehq.input.gesture.double_tap"), value: "tap:2" },
                            //% "Triple tap"
                            { label: qsTrId("gamehq.input.gesture.triple_tap"), value: "tap:3" },
                            //% "Hold"
                            { label: qsTrId("gamehq.input.gesture.hold"), value: "hold:1" }
                        ]
                        onActivated: function(value) { root.selectGesture(value) }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: root.model && root.model.editorGestureLocked
                    spacing: Theme.s8
                    Text {
                        //% "Gesture"
                        text: qsTrId("gamehq.input.assignment.gesture")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    Rectangle {
                        implicitWidth: lockedGestureText.implicitWidth + Theme.s12
                        implicitHeight: lockedGestureText.implicitHeight + Theme.s4
                        radius: Theme.radiusPill
                        color: Theme.bg1
                        border.width: Theme.borderWidth
                        border.color: Theme.stroke
                        Text {
                            id: lockedGestureText
                            anchors.centerIn: parent
                            //% "Press · fixed for combinations"
                            text: qsTrId("gamehq.input.assignment.combination_gesture")
                            color: Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontCaption
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s8
                    visible: root.model && root.model.editorGestureKind === "hold"

                    Text {
                        //% "Hold duration"
                        text: qsTrId("gamehq.input.assignment.hold_duration")
                        color: Theme.textMuted
                        font.family: Theme.fontFamily
                        font.pixelSize: Theme.fontCaption
                    }
                    BindingSegmentedControl {
                        Layout.fillWidth: true
                        currentValue: root.model ? String(root.model.editorHoldMs) : "0"
                        options: [
                            //% "Default"
                            { label: qsTrId("gamehq.input.assignment.default_duration"), value: "0" },
                            //% "%1 s"
                            { label: qsTrId("gamehq.duration.seconds_short").arg("1.0"), value: "1000" },
                            //% "%1 s"
                            { label: qsTrId("gamehq.duration.seconds_short").arg("1.5"), value: "1500" },
                            //% "%1 s"
                            { label: qsTrId("gamehq.duration.seconds_short").arg("2.0"), value: "2000" },
                            //% "%1 s"
                            { label: qsTrId("gamehq.duration.seconds_short").arg("3.0"), value: "3000" }
                        ]
                        onActivated: function(value) {
                            root.model.setEditorGesture("hold", 1, Number(value))
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: noticeRow.implicitHeight + Theme.s16 * 2
                    visible: root.model && root.model.editorNotice !== ""
                    radius: Theme.radiusS
                    color: root.noticeDanger ? Theme.dangerSoft
                         : root.noticeWarning ? Theme.warningSoft
                                                                  : Theme.surfaceAlt
                    border.width: Theme.borderWidth
                    border.color: root.noticeDanger ? Theme.danger
                                : root.noticeKind === "unsupported_input" ? Theme.warning
                                                                         : Theme.stroke

                    RowLayout {
                        id: noticeRow
                        x: Theme.s16
                        y: Theme.s16
                        width: parent.width - Theme.s16 * 2
                        spacing: Theme.s12

                        Text {
                            text: root.noticeDanger ? "!" : root.noticeWarning ? "⚠" : "i"
                            color: root.noticeDanger ? Theme.danger
                                 : root.noticeWarning ? Theme.warning : Theme.textMuted
                            font.family: Theme.fontFamily
                            font.pixelSize: Theme.fontH3
                            font.weight: Font.DemiBold
                            Layout.alignment: Qt.AlignTop
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s4
                            Text {
                                Layout.fillWidth: true
                                text: {
                                    if (root.noticeDanger) {
                                        //% "Assignment needs attention"
                                        return qsTrId("gamehq.input.assignment.notice.attention")
                                    }
                                    if (root.noticeKind === "unsupported_input") {
                                        //% "Button not verified this session"
                                        return qsTrId("gamehq.input.assignment.notice.not_verified")
                                    }
                                    if (root.noticeKind === "conversion_required") {
                                        //% "Compatibility change required"
                                        return qsTrId("gamehq.input.assignment.notice.compatibility")
                                    }
                                    //% "Assignment note"
                                    return qsTrId("gamehq.input.assignment.notice.default")
                                }
                                color: root.noticeDanger ? Theme.danger
                                     : root.noticeWarning ? Theme.warning : Theme.text
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                font.weight: Font.DemiBold
                                wrapMode: Text.WordWrap
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.model ? root.model.editorNotice : ""
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontCaption
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s12
                    Item { Layout.fillWidth: true }
                    AccentButton {
                        //% "Cancel"
                        label: qsTrId("gamehq.action.cancel")
                        onClicked: root.model.closeAssignmentEditor()
                    }
                    AccentButton {
                        primary: true
                        //% "Save"
                        label: qsTrId("gamehq.action.save")
                        enabled: root.model && root.model.editorCanSave
                        onClicked: root.model.saveAssignment()
                    }
                }
            }
        }
    }
}
