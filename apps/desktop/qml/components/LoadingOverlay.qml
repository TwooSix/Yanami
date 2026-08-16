import QtQuick
import Yanami.Ui

Item {
    id: root

    property bool active: false
    property int showDelay: 180
    property int minimumVisibleTime: 250
    property bool shown: false
    property double shownAt: 0

    visible: active || shown || opacity > 0
    enabled: active
    opacity: shown ? 1 : 0

    onActiveChanged: {
        if (active) {
            hideTimer.stop()
            showTimer.restart()
        } else {
            showTimer.stop()
            const remaining = root.minimumVisibleTime - (Date.now() - root.shownAt)
            if (shown && remaining > 0) {
                hideTimer.interval = remaining
                hideTimer.restart()
            } else {
                shown = false
            }
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: root.active
        acceptedButtons: Qt.AllButtons
    }

    WheelHandler {
        enabled: root.active
        blocking: true
        onWheel: event => event.accepted = true
    }

    GlassPanel {
        id: hud
        anchors.centerIn: parent
        width: 62
        height: 62
        radius: 21
        color: "#D91B1E27"
        border.color: "#24FFFFFF"
        scale: root.shown ? 1 : 0.94

        LoadingIndicator {
            anchors.centerIn: parent
            width: 28
            height: 28
            running: root.shown || root.opacity > 0
        }

        Behavior on scale {
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }
    }

    Timer {
        id: showTimer
        interval: root.showDelay
        onTriggered: {
            if (root.active) {
                root.shownAt = Date.now()
                root.shown = true
            }
        }
    }

    Timer {
        id: hideTimer
        onTriggered: root.shown = false
    }

    Behavior on opacity {
        NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
    }
}
