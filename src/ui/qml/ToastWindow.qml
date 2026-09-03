import QtQuick
import GameHQ
import "components"

// Bottom-right toast stack window (docs/notifications.md). Frameless, topmost,
// click-through and non-activating — flags + placement live in NotificationCenter.
// Listens to `notifications.posted` and stacks Toast cards that remove themselves
// on dismiss; hides the window once the stack is empty.
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

    ListModel { id: toastModel }

    // Removal goes through a root function: a delegate can reliably resolve the
    // root object's id (win) but not always a sibling id (toastModel) under the
    // QML AOT cache.
    function dismissToast(idx) {
        toastModel.remove(idx)
        if (toastModel.count === 0)
            notifications.hideWindow()
    }

    Connections {
        target: notifications
        function onPosted(title, body, imageUrl, kind, when, isVideo) {
            toastModel.append({ "title": title, "body": body,
                                "imageUrl": imageUrl, "kind": kind,
                                "when": when, "isVideo": isVideo })
        }
    }

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
            model: toastModel
            delegate: Toast {
                width: stack.width
                title: model.title
                body: model.body
                imageUrl: model.imageUrl
                kind: model.kind
                when: model.when
                isVideo: model.isVideo
                onDismissed: win.dismissToast(index)
            }
        }
    }
}
