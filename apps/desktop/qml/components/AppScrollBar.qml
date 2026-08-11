import QtQuick
import QtQuick.Controls.Basic
import Yanami

ScrollBar {
    id: control

    hoverEnabled: true
    padding: 2
    minimumSize: 0.08
    implicitWidth: orientation === Qt.Vertical ? 9 : 96
    implicitHeight: orientation === Qt.Horizontal ? 9 : 96

    contentItem: Rectangle {
        implicitWidth: 5
        implicitHeight: 5
        radius: Math.min(width, height) / 2
        color: control.pressed || control.hovered ? "#B8FFFFFF" : "#68FFFFFF"

        Behavior on color { ColorAnimation { duration: 130 } }
    }

    background: Item { }
}
