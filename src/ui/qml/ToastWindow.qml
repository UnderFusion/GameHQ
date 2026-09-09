import QtQuick
import GameHQ
import "components"

// Bottom-right toast stack window (docs/notifications.md). Frameless, topmost,
// click-through and non-activating — flags + placement live in NotificationCenter.
// The C++ model bounds only visible cards; operation outcomes update one row.
Window {
    id: win
    LayoutMirroring.enabled: languageManager.layoutDirection === Qt.RightToLeft
    LayoutMirroring.childrenInherit: true
    objectName: "gamehqToasts"
    width: 520
    height: 700
    visible: false
    color: "transparent"
    //% "%1 Notifications"
    title: qsTrId("gamehq.notifications.window_title").arg(Brand.name)

    Binding { target: notifications; property: "visibleLimit"; value: Theme.toastVisibleLimit }

    Column {
        id: stack
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.s16
        width: 460
        spacing: Theme.s12

        move: Transition {
            NumberAnimation { properties: "y"; duration: Theme.durNormal; easing.type: Easing.OutCubic }
        }

        Repeater {
            model: notifications.visibleToasts
            delegate: Toast {
                width: stack.width
                title: model.title
                body: model.body
                imageUrl: model.imageUrl
                kind: model.kind
                when: model.when
                isVideo: model.isVideo
                pending: model.pending
                contentRevision: model.revision
                onDismissed: notifications.dismiss(model.toastKey, model.revision)
            }
        }
    }
}
