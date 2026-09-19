import QtQuick
import QtQuick.Layouts
import QtQuick.Controls.Basic as QC
import GameHQ
import "../helpers/PadNav.js" as PadNav

Item {
    id: root

    property string pageTitle: ""
    property string pageDescription: ""
    property Component pageAction
    // Zero means fill the available page area. Individual pages may still set
    // a positive cap when a deliberately narrow reading column is useful.
    property int maxContentWidth: 0
    // The page's currently open modal, if any. SettingsView routes every pad
    // event to it while it is visible, so directions and Cross/Circle stay
    // trapped inside the dialog instead of driving the page underneath.
    property Item padOverlay: null
    default property alias contentData: contentColumn.data

    // When a modal takes over, remember which page control had focus so
    // closing it puts the pad cursor back where the user left off.
    property Item _padReturnItem: null
    onPadOverlayChanged: {
        if (padOverlay) {
            const active = Window.window ? Window.window.activeFocusItem : null
            if (!_padReturnItem && active && containsItem(active)
                    && !PadNav.isInside(active, padOverlay))
                _padReturnItem = active
        } else if (_padReturnItem) {
            const item = _padReturnItem
            _padReturnItem = null
            try {
                if (item.visible && item.enabled)
                    item.forceActiveFocus()
            } catch (err) {
                // The control was destroyed while the modal was up (its dialog
                // rewrote the bindings); the next pad move re-enters the page
                // from its first control.
            }
        }
    }

    function containsItem(item) {
        for (let probe = item; probe; probe = probe.parent)
            if (probe === root)
                return true
        return false
    }

    function revealFocusedItem() {
        const item = Window.window ? Window.window.activeFocusItem : null
        if (!item || item === pageContainer || !PadNav.isInside(item, pageContainer))
            return
        // A modal scrolls its own viewport; the page must not chase it.
        if (padOverlay && PadNav.isInside(item, padOverlay))
            return
        const point = item.mapToItem(pageContainer, 0, 0)
        const top = Math.max(0, point.y - Theme.s16)
        const bottom = point.y + item.height + Theme.s16
        if (top < flick.contentY)
            flick.contentY = top
        else if (bottom > flick.contentY + flick.height)
            flick.contentY = Math.min(Math.max(0, flick.contentHeight - flick.height),
                                      bottom - flick.height)
    }

    Connections {
        // Attached-property trap: a bare `Window.window` here would attach to
        // the Connections object — which is not an Item — and stay null
        // forever, silently never connecting. Qualify through the page item.
        target: root.Window.window
        function onActiveFocusItemChanged() {
            const item = root.Window.window.activeFocusItem
            // Pointer interaction already targets the visible area. Revealing
            // its focus can fight scrolling; only navigation needs assistance.
            if (!item || item.focusReason === Qt.MouseFocusReason)
                return
            Qt.callLater(function() {
                if (root.Window.window && root.Window.window.activeFocusItem === item)
                    root.revealFocusedItem()
            })
        }
    }

    // True while the focused control still overlaps the visible band. The right
    // stick scrolls without moving focus, so after a long scroll the pad cursor
    // can sit far outside the viewport.
    function focusInViewport() {
        const item = Window.window ? Window.window.activeFocusItem : null
        if (!item || !containsItem(item))
            return false
        if (padOverlay && PadNav.isInside(item, padOverlay))
            return true   // the modal owns its own viewport; never re-enter the page
        const point = item.mapToItem(pageContainer, 0, 0)
        return point.y + item.height > flick.contentY
                && point.y < flick.contentY + flick.height
    }

    // Control a direction should land on when focus is off screen: whatever the
    // user is actually looking at, rather than the row they left behind.
    function viewportEntryTarget(direction) {
        return PadNav.viewportTarget(root, pageContainer, flick.contentY,
                                     flick.contentY + flick.height, direction)
    }

    // Wheel-like pad scroll (right stick): moves the viewport, not the focus.
    function scrollBy(direction) {
        const maximum = Math.max(0, flick.contentHeight - flick.height)
        flick.contentY = Math.max(0, Math.min(maximum,
            flick.contentY + direction * Math.max(80, flick.height * 0.28)))
    }

    Flickable {
        id: flick
        anchors.fill: parent
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        contentWidth: width
        contentHeight: pageContainer.implicitHeight + Theme.s24
        flickableDirection: Flickable.VerticalFlick

        QC.ScrollBar.vertical: AppScrollBar {}

        MouseArea {
            width: flick.contentWidth
            height: Math.max(flick.height, flick.contentHeight)
            z: -1
            onClicked: root.forceActiveFocus()
        }

        ColumnLayout {
            id: pageContainer
            x: Math.max(0, (flick.width - width) / 2)
            width: Math.max(0, root.maxContentWidth > 0
                                  ? Math.min(root.maxContentWidth, flick.width - Theme.s16 * 2)
                                  : flick.width - Theme.s16 * 2)
            spacing: Theme.s24

            SettingsPageHeader {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                visible: root.pageTitle.length > 0 || root.pageDescription.length > 0
                         || root.pageAction !== null
                title: root.pageTitle
                description: root.pageDescription
                action: root.pageAction
            }

            ColumnLayout {
                id: contentColumn
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                spacing: Theme.s16
            }
        }
    }
}
