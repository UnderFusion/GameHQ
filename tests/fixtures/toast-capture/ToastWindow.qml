import QtQuick

// Stand-in for src/ui/qml/ToastWindow.qml in tst_toastcaptureexclusion: the
// real NotificationCenter loads it by the same module/name and drives its HWND,
// while one opaque, unmistakable colour makes captured pixels easy to judge.
Window {
    objectName: "gamehqToasts"
    width: 160
    height: 96
    visible: false
    color: "#ff00ff"
}
