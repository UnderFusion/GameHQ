import QtQuick
import GameHQ

// Mouse-only drag strip on a sidebar's content edge (both the desktop and the
// overlay sidebar). Dragging reports clamped widths live; release commits the
// width once, and a double-click asks the host to restore its default. Deltas
// are measured in `reference`, the sidebar's static parent, so the growing
// sidebar never moves the coordinate frame under the pointer. Under RTL the
// strip mirrors to the other edge and the delta flips with it.
MouseArea {
    id: root

    property real currentWidth: 0
    property real minimumWidth: 0
    property real maximumWidth: 0
    property Item reference: parent ? parent.parent : null
    // Host confirmation that the pointer really is at this edge. The overlay
    // window can miss the MouseArea's hover-leave, which left the line lit;
    // the host's own HoverHandler reliably tracks the pointer there.
    property bool pointerAtEdge: true
    readonly property bool mirrored: LayoutMirroring.enabled

    signal widthDragged(real width)
    signal widthCommitted(real width)
    signal resetRequested()

    property real _startX: 0
    property real _startWidth: 0

    function clampWidth(value) {
        return Math.round(Math.max(root.minimumWidth, Math.min(root.maximumWidth, value)))
    }
    function referenceX(mouse) {
        return root.reference ? root.mapToItem(root.reference, mouse.x, 0).x : mouse.x
    }

    width: Theme.sidebarResizeHandleWidth
    anchors.top: parent ? parent.top : undefined
    anchors.bottom: parent ? parent.bottom : undefined
    anchors.right: parent ? parent.right : undefined
    hoverEnabled: true
    preventStealing: true
    cursorShape: Qt.SplitHCursor
    z: 100

    onPressed: function(mouse) {
        root._startX = root.referenceX(mouse)
        root._startWidth = root.currentWidth
    }
    onPositionChanged: function(mouse) {
        if (!root.pressed)
            return
        const dx = root.referenceX(mouse) - root._startX
        root.widthDragged(root.clampWidth(root._startWidth + (root.mirrored ? -dx : dx)))
    }
    onReleased: root.widthCommitted(root.currentWidth)
    onDoubleClicked: root.resetRequested()

    // Accent line on the edge while hovered or dragged, so the grip is findable.
    Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.topMargin: Theme.s12
        anchors.bottomMargin: Theme.s12
        width: 2
        radius: 1
        color: Theme.accent
        opacity: root.pressed ? 0.9 : root.containsMouse && root.pointerAtEdge ? 0.5 : 0
        Behavior on opacity { NumberAnimation { duration: Theme.durFast } }
    }
}
