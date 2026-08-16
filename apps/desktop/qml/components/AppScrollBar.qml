import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

ScrollBar {
    id: control

    readonly property real visualThickness: 5

    hoverEnabled: true
    minimumSize: 0.08
    implicitWidth: orientation === Qt.Vertical ? Theme.scrollBarGutter : 96
    implicitHeight: orientation === Qt.Horizontal ? 9 : 96
    leftPadding: orientation === Qt.Vertical
        ? Math.max(2, (width - visualThickness) / 2) : 2
    rightPadding: leftPadding
    topPadding: orientation === Qt.Horizontal
        ? Math.max(2, (height - visualThickness) / 2) : 2
    bottomPadding: topPadding

    contentItem: Rectangle {
        implicitWidth: control.visualThickness
        implicitHeight: control.visualThickness
        radius: Math.min(width, height) / 2
        color: control.pressed ? "#DCFFFFFF"
            : (control.hovered ? "#B8FFFFFF" : "#64FFFFFF")

        Behavior on color { ColorAnimation { duration: 130 } }
    }

    background: Rectangle {
        radius: Math.min(width, height) / 2
        color: control.pressed || control.hovered ? "#0FFFFFFF" : "transparent"

        Behavior on color { ColorAnimation { duration: 130 } }
    }
}
