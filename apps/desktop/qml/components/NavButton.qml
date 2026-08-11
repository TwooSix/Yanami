import QtQuick
import QtQuick.Controls.Basic
import Yanami

Button {
    id: root
    property string iconName
    property bool selected: false

    implicitWidth: 48
    implicitHeight: 48
    hoverEnabled: true

    contentItem: Item {
        AppIcon {
            anchors.centerIn: parent
            width: 22
            height: 22
            name: root.iconName
            color: root.selected ? Theme.text : (root.hovered ? "#DCE1EA" : Theme.textMuted)

            Behavior on color { ColorAnimation { duration: 140 } }
        }
    }
    background: Item {
        Rectangle {
            anchors.fill: parent
            radius: 15
            color: root.selected ? Theme.accentSoft : (root.hovered ? "#18FFFFFF" : "transparent")
            border.width: root.selected ? 1 : 0
            border.color: "#45FF8FA7"
            scale: root.down ? 0.94 : 1

            Behavior on color { ColorAnimation { duration: 140 } }
            Behavior on scale { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
        }

        Rectangle {
            visible: root.selected
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            width: 3
            height: 16
            radius: 2
            color: Theme.accent
        }
    }
}
