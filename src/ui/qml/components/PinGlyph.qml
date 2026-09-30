import QtQuick
import GameHQ

// Minimal outline push-pin (24-unit line-icon grid). Unpinned it leans over
// and stays an outline; pinned it stands upright and fills in, both animated.
Item {
    id: root
    property bool filled: false
    property color color: Theme.textMuted

    implicitWidth: Theme.s16
    implicitHeight: Theme.s16

    property real fillAmount: root.filled ? 1 : 0
    Behavior on fillAmount { NumberAnimation { duration: Theme.durFast } }

    rotation: root.filled ? 0 : 45
    Behavior on rotation { NumberAnimation { duration: Theme.durNormal; easing.type: Easing.OutBack } }

    onFillAmountChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const s = Math.min(width, height) / 24
            ctx.scale(s, s)
            ctx.lineWidth = 1.75
            ctx.lineCap = "round"
            ctx.lineJoin = "round"
            ctx.strokeStyle = root.color
            // Body: rounded cap, tapered shaft, flared collar.
            ctx.beginPath()
            ctx.moveTo(9, 3.5)
            ctx.lineTo(15, 3.5)
            ctx.lineTo(14, 10)
            ctx.lineTo(17.5, 14.5)
            ctx.lineTo(6.5, 14.5)
            ctx.lineTo(10, 10)
            ctx.closePath()
            if (root.fillAmount > 0) {
                ctx.globalAlpha = root.fillAmount
                ctx.fillStyle = root.color
                ctx.fill()
                ctx.globalAlpha = 1
            }
            ctx.stroke()
            // Needle.
            ctx.beginPath()
            ctx.moveTo(12, 14.5)
            ctx.lineTo(12, 20.5)
            ctx.stroke()
        }
    }
}
