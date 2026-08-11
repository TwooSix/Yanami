import QtQuick
import Yanami

Item {
    id: root

    property bool active: false
    property int showDelay: 180
    property bool shown: false

    visible: shown || opacity > 0
    enabled: false
    opacity: shown ? 1 : 0

    onActiveChanged: {
        if (active) {
            showTimer.restart()
        } else {
            showTimer.stop()
            shown = false
        }
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
            running: root.active
        }

        Behavior on scale {
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }
    }

    Timer {
        id: showTimer
        interval: root.showDelay
        onTriggered: {
            if (root.active)
                root.shown = true
        }
    }

    Behavior on opacity {
        NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
    }
}
