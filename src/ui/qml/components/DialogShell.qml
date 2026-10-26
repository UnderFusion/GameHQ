import QtQuick
import GameHQ
import "../helpers/PadNav.js" as PadNav

// Scaffolding shared by the in-window button dialogs (ConfirmDialog, the
// binding conflict/compatibility prompts, the mapping-preset dialogs): scrim,
// card, fade-in, and the pad contract their hosts route to while one is up
// (Main.qml, OverlayWindow.qml, SettingsPage.padOverlay).
//
// Pad contract: directions move only between the dialog's own controls, Cross
// activates the focused one (a button clicks, a text field accepts), Circle
// cancels. When focus has left the dialog, the first direction returns it to
// landingItem. A dialog declares its content as children (laid out in a
// Column inside the card), names its landing control, and calls present()
// from its own open().
FocusScope {
    id: shell

    // Where open() lands and where the pad re-enters the dialog. Use the
    // safe choice (Cancel) for anything that destroys or replaces data.
    property Item landingItem: null
    // What padStep (Up/Down on hosts that route it) moves along: a row of
    // buttons ("horizontal") or stacked rows ("vertical").
    property string padAxis: "horizontal"
    // Esc cancels like Circle. Off by default: most hosts never routed Esc
    // into their dialogs, and their key handling stays as it was.
    property bool escapeCancels: false

    property color scrimColor: Theme.scrim
    property real cardWidth: Theme.dialogWidth
    // Centred by default; an anchored dialog (a menu under its button)
    // binds its own position.
    property real cardX: Math.round((shell.width - card.width) / 2)
    property real cardY: Math.round((shell.height - card.height) / 2)
    readonly property real cardHeight: card.height
    property real cardPadding: Theme.s24
    property int cardRadius: Theme.radiusL
    property color cardBorderColor: Theme.stroke
    property alias contentSpacing: body.spacing
    default property alias content: body.data

    signal canceled()

    visible: false
    opacity: 0

    // Shows the dialog and moves focus to `target` (landingItem by default)
    // once it is on screen.
    function present(target) {
        visible = true
        const item = target || shell.landingItem
        Qt.callLater(function() {
            if (shell.visible && item)
                item.forceActiveFocus()
        })
    }
    function close() { visible = false }
    function cancel() {
        shell.canceled()
        shell.close()
    }

    function focusedItem() {
        const active = Window.window ? Window.window.activeFocusItem : null
        return active && PadNav.isInside(active, shell) ? active : null
    }
    function moveFocus(direction, vertical) {
        const active = focusedItem()
        if (!active) {
            if (shell.landingItem)
                shell.landingItem.forceActiveFocus()
            sounds.play("nav_tick")
            return
        }
        const target = vertical ? PadNav.verticalTarget(shell, active, direction)
                                : PadNav.horizontalTarget(shell, active, direction)
        if (target) {
            target.forceActiveFocus()
            sounds.play("nav_tick")
        }
    }

    function padStep(direction) { moveFocus(direction, shell.padAxis === "vertical") }
    function padHorizontal(direction) { moveFocus(direction, false) }
    function padVertical(direction) { moveFocus(direction, true) }
    function padConfirm() {
        const active = focusedItem()
        // A disabled control (a delete that would orphan its users) must not
        // fire through the pad either.
        if (!active || active.enabled === false)
            return
        if (active.clicked)
            active.clicked()
        else if (active.accepted)
            active.accepted()
    }
    function padBack() { shell.cancel() }

    Keys.onEscapePressed: function(event) {
        event.accepted = shell.escapeCancels
        if (shell.escapeCancels)
            shell.cancel()
    }

    Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    states: State {
        when: shell.visible
        PropertyChanges { target: shell; opacity: 1 }
    }

    // Clicking outside the card cancels.
    Rectangle {
        anchors.fill: parent
        color: shell.scrimColor
        MouseArea {
            anchors.fill: parent
            onClicked: shell.cancel()
        }
    }

    Rectangle {
        id: card
        x: shell.cardX
        y: shell.cardY
        width: shell.cardWidth
        height: body.implicitHeight + shell.cardPadding * 2
        radius: shell.cardRadius
        color: Theme.surface
        border.width: Theme.borderWidth
        border.color: shell.cardBorderColor

        MouseArea { anchors.fill: parent }   // swallow clicks so the scrim does not cancel

        Column {
            id: body
            x: shell.cardPadding
            y: shell.cardPadding
            width: parent.width - shell.cardPadding * 2
            spacing: Theme.s16
        }
    }
}
