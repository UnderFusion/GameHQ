import QtQuick
import GameHQ

// One Share flow for the desktop gallery, the lightbox and the overlay
// (docs/share-platform.md). Every window hosts its own instance, all driven
// by the single `shareService`, so the steps and wording never diverge.
//
// Modal: while open, the host routes ALL pad input here (padNavigate /
// padConfirm / padBack) and nothing underneath may act. Cross activates the
// highlighted row, Circle steps back (targets -> destinations -> closed) and,
// while sending, asks the service to cancel. Rows are keyed by stable ids,
// never by position.
Item {
    id: root

    // "destinations" | "targets" | "resend" | "sending" | "result" | "error"
    property string view: "destinations"
    property var rows: []
    property int currentIndex: 0
    property string message: ""
    property bool resultIsError: false
    property string providerId: ""
    property string providerName: ""
    property string pendingTargetId: ""

    signal closed()

    // Explicit state, not effective visibility: logic must not depend on
    // whether an ancestor happens to be shown.
    property bool isOpen: false

    anchors.fill: parent
    visible: root.isOpen
    z: 300

    // ── Public API ──────────────────────────────────────────────────────
    function openFor(filePath, gameName) {
        if (shareService.busy)
            return false
        root.isOpen = true
        root.resultIsError = false
        if (!shareService.open(filePath, gameName || "")) {
            root.showError(root.errorText(shareService.lastError))
            return true
        }
        root.showDestinations()
        sounds.play("nav_tick")
        return true
    }

    function close() {
        if (shareService.busy)
            return   // a send in flight must finish or be cancelled first
        shareService.close()
        root.isOpen = false
        root.view = "destinations"
        root.rows = []
        root.closed()
    }

    function padNavigate(direction) {
        if (root.rows.length === 0)
            return
        let next = root.currentIndex
        for (let step = 0; step < root.rows.length; ++step) {
            next = next + direction
            if (next < 0 || next >= root.rows.length)
                return   // clamp, no wrap
            if (root.rows[next].enabled !== false) {
                root.currentIndex = next
                sounds.play("nav_tick")
                return
            }
        }
    }

    function padConfirm() {
        if (root.currentIndex >= 0 && root.currentIndex < root.rows.length)
            root.activate(root.rows[root.currentIndex])
    }

    function padBack() {
        if (root.view === "sending") {
            shareService.cancel()
            return
        }
        if (root.view === "targets" || root.view === "resend") {
            root.showDestinations()
            sounds.play("nav_tick")
            return
        }
        root.close()
    }

    // ── Views ───────────────────────────────────────────────────────────
    function setRows(list) {
        root.rows = list
        root.currentIndex = 0
        for (let i = 0; i < list.length; ++i) {
            if (list[i].enabled !== false) {
                root.currentIndex = i
                break
            }
        }
    }

    function showDestinations() {
        root.view = "destinations"
        root.providerId = ""
        const list = []
        const providers = shareService.providers()
        for (let i = 0; i < providers.length; ++i) {
            const p = providers[i]
            list.push({ id: "provider:" + p.id, kind: "provider", providerId: p.id,
                        label: p.name, icon: p.icon, detail: p.available ? p.privacy : p.reason,
                        enabled: p.available })
        }
        //% "No share destinations are available yet."
        root.message = list.length === 0 ? qsTrId("gamehq.share.no_destinations") : ""
        root.setRows(list)
    }

    function showTargets() {
        root.view = "targets"
        const list = []
        const targets = shareService.targets()
        for (let i = 0; i < targets.length; ++i) {
            const t = targets[i]
            list.push({ id: "target:" + t.id, kind: "target", targetId: t.id,
                        label: t.name, detail: t.subtitle, enabled: true })
        }
        if (shareService.targetsLoading)
            //% "Loading…"
            root.message = qsTrId("gamehq.share.loading")
        else if (list.length === 0)
            //% "No recipients found."
            root.message = qsTrId("gamehq.share.no_targets")
        else
            root.message = ""
        root.setRows(list)
    }

    function showResend(targetId) {
        root.view = "resend"
        root.pendingTargetId = targetId
        //% "You already shared this capture here. Share it again?"
        root.message = qsTrId("gamehq.share.resend_question")
        root.setRows([
            //% "Share again"
            { id: "resend", kind: "resend", label: qsTrId("gamehq.share.resend_confirm"), enabled: true },
            //% "Cancel"
            { id: "back", kind: "back", label: qsTrId("gamehq.action.cancel"), enabled: true }
        ])
        // Land on Cancel: Cross must not send twice by accident.
        root.currentIndex = 1
    }

    function showError(text) {
        root.view = "error"
        root.resultIsError = true
        root.message = text
        root.setRows([
            //% "Close"
            { id: "done", kind: "done", label: qsTrId("gamehq.share.close"), enabled: true }
        ])
    }

    function showResult(result) {
        root.view = "result"
        const outcome = result.outcome || "failed"
        root.resultIsError = outcome === "failed" || outcome === "unconfirmed"
        root.message = root.outcomeText(outcome, result.errorCode || "", result.detail || "")
        root.setRows([
            //% "Done"
            { id: "done", kind: "done", label: qsTrId("gamehq.action.done"), enabled: true }
        ])
    }

    function outcomeText(outcome, code, detail) {
        switch (outcome) {
        case "sent":
            //% "Sent."
            return qsTrId("gamehq.share.outcome.sent")
        case "handed_off":
            if (detail === "paste")
                //% "Copied and opened %1. Paste it into a chat to send it."
                return qsTrId("gamehq.share.outcome.handed_off_paste").arg(root.providerName)
            //% "Opened in %1. Finish sending there."
            return qsTrId("gamehq.share.outcome.handed_off").arg(root.providerName)
        case "copied":
            //% "Copied. Paste it wherever you want to share it."
            return qsTrId("gamehq.share.outcome.copied")
        case "cancelled":
            //% "Sharing cancelled. Nothing was sent."
            return qsTrId("gamehq.share.outcome.cancelled")
        case "unconfirmed":
            //% "GameHQ couldn't confirm whether it was sent. Check before sharing again."
            return qsTrId("gamehq.share.outcome.unconfirmed")
        }
        return root.errorText(code)
    }

    function errorText(code) {
        switch (code) {
        case "capture_missing":
            //% "This capture is no longer on disk."
            return qsTrId("gamehq.share.error.capture_missing")
        case "capture_changed":
            //% "This capture changed on disk after you opened Share. Open Share again."
            return qsTrId("gamehq.share.error.capture_changed")
        case "not_connected":
            //% "Connect this account in Settings first."
            return qsTrId("gamehq.share.error.not_connected")
        case "rate_limited":
            //% "Discord is limiting uploads right now. Wait a moment and try again."
            return qsTrId("gamehq.share.error.rate_limited")
        case "too_large":
            //% "This capture is too large for that channel."
            return qsTrId("gamehq.share.error.too_large")
        case "webhook_revoked":
        case "webhook_missing":
            //% "That channel's webhook no longer works. Remove it in Settings and add it again."
            return qsTrId("gamehq.share.error.webhook_revoked")
        case "network_error":
            //% "GameHQ couldn't reach the service. Check your connection."
            return qsTrId("gamehq.share.error.network_error")
        }
        //% "This capture could not be shared."
        return qsTrId("gamehq.share.error.generic")
    }

    function startShare(targetId, confirmResend) {
        // Enter "sending" before the call: a provider may finish inside it.
        const previousView = root.view
        root.view = "sending"
        //% "Sharing…"
        root.message = qsTrId("gamehq.share.sending")
        root.rows = []
        const job = shareService.share(root.providerId, targetId, confirmResend)
        if (job !== "")
            return   // onFinished() shows the result, now or later
        root.view = previousView
        if (shareService.lastError === "resend_needs_confirmation")
            root.showResend(targetId)
        else if (shareService.lastError !== "busy")
            root.showError(root.errorText(shareService.lastError))
    }

    // A hand-off provider answers with its one app target: go straight to
    // it instead of asking the user to pick "the app".
    function targetsArrived() {
        const targets = shareService.targets()
        if (targets.length === 1 && targets[0].kind === "external")
            root.startShare(targets[0].id, false)
        else
            root.showTargets()
    }

    function activate(row) {
        if (!row || row.enabled === false)
            return
        sounds.play("confirm")
        switch (row.kind) {
        case "provider":
            root.providerId = row.providerId
            root.providerName = row.label
            // Switch first: a provider may answer synchronously, inside
            // requestTargets(), and that answer must find the targets view.
            root.view = "targets"
            root.rows = []
            if (!shareService.requestTargets(row.providerId))
                root.showError(root.errorText(shareService.lastError))
            else if (root.view === "targets" && !shareService.targetsLoading)
                root.targetsArrived()
            return
        case "target":
            root.startShare(row.targetId, false)
            return
        case "resend":
            root.startShare(root.pendingTargetId, true)
            return
        case "back":
            root.showDestinations()
            return
        case "done":
            root.close()
            return
        }
    }

    Connections {
        target: shareService
        enabled: root.isOpen
        function onTargetsChanged() {
            if (root.view !== "targets" || shareService.targetsProviderId !== root.providerId)
                return
            if (shareService.targetsLoading)
                root.showTargets()   // "Loading…"
            else
                root.targetsArrived()
        }
        function onFinished(result) {
            if (root.view === "sending")
                root.showResult(result)
        }
    }

    // ── Visuals ─────────────────────────────────────────────────────────
    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            onClicked: root.padBack()
        }
    }

    Rectangle {
        anchors.centerIn: parent
        width: 420
        height: column.implicitHeight + Theme.s24 * 2
        radius: Theme.radiusL
        color: Theme.surface
        border.width: 1
        border.color: Theme.stroke

        MouseArea { anchors.fill: parent }   // clicks on the card never reach the scrim

        Column {
            id: column
            x: Theme.s24
            y: Theme.s24
            width: parent.width - Theme.s24 * 2
            spacing: Theme.s8

            Text {
                width: parent.width
                text: root.view === "targets" || root.view === "resend"
                      ? root.providerName
                      //% "Share"
                      : qsTrId("gamehq.share.title")
                color: Theme.text
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontH3
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                visible: shareService.fileName !== ""
                text: shareService.fileName
                color: Theme.textFaint
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontCaption
                elide: Text.ElideMiddle
            }
            Text {
                width: parent.width
                visible: root.message !== ""
                text: root.message
                color: root.resultIsError ? Theme.danger : Theme.textMuted
                font.family: Theme.fontFamily
                font.pixelSize: Theme.fontBody
                wrapMode: Text.WordWrap
            }

            Repeater {
                model: root.rows
                delegate: Rectangle {
                    required property var modelData
                    required property int index
                    readonly property bool current: root.currentIndex === index
                    readonly property bool usable: modelData.enabled !== false
                    width: column.width
                    height: modelData.detail ? 52 : 40
                    radius: Theme.radiusS
                    color: current && usable ? Theme.surfaceAlt : "transparent"
                    border.width: current && usable ? 1 : 0
                    border.color: Theme.focusRing
                    opacity: usable ? 1 : 0.5

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: Theme.s12
                        anchors.right: parent.right
                        anchors.rightMargin: Theme.s12
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: Theme.s12

                        Text {
                            visible: !!modelData.icon
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.icon || ""
                            color: Theme.text
                            font.family: "Segoe Fluent Icons"
                            font.pixelSize: Theme.fontH3
                        }
                        Column {
                            anchors.verticalCenter: parent.verticalCenter
                            width: parent.width - Theme.s32
                            Text {
                                width: parent.width
                                text: modelData.label
                                color: current ? Theme.text : Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontBody
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                visible: !!modelData.detail
                                text: modelData.detail || ""
                                color: Theme.textFaint
                                font.family: Theme.fontFamily
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideRight
                            }
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        enabled: parent.usable
                        onEntered: root.currentIndex = index
                        onClicked: root.activate(modelData)
                    }
                }
            }

            AccentButton {
                visible: root.view === "sending"
                anchors.right: parent.right
                //% "Cancel"
                label: qsTrId("gamehq.action.cancel")
                onClicked: shareService.cancel()
            }
        }
    }
}
