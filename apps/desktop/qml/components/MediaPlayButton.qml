import QtQuick
import QtQuick.Controls.Basic
import Yanami

Button {
    id: control

    property bool paused: false

    Accessible.name: control.paused ? qsTr("Play") : qsTr("Pause")

    implicitWidth: 46
    implicitHeight: 46
    hoverEnabled: true

    contentItem: Item {
        AppIcon {
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: control.paused ? 0.8 : 0
            width: 20
            height: 20
            name: control.paused ? "play" : "pause"
            color: "white"
        }
    }

    background: Rectangle {
        radius: width / 2
        color: control.down ? "#E65372" : (control.hovered ? Theme.accentHover : Theme.accent)
        scale: control.down ? 0.96 : 1

        Behavior on color { ColorAnimation { duration: 140 } }
        Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }
}
