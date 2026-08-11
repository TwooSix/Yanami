import QtQuick
import QtQuick.Controls.Basic
import Yanami

Slider {
    id: control

    property real bufferedValue: from
    readonly property real bufferedVisualPosition: Math.max(0, Math.min(1,
        (bufferedValue - from) / Math.max(0.000001, to - from)))

    implicitHeight: 24
    leftPadding: 0
    rightPadding: 0
    topPadding: 0
    bottomPadding: 0

    background: Item {
        x: control.leftPadding
        y: control.topPadding
        width: control.availableWidth
        height: control.availableHeight

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: "#28FFFFFF"
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(control.visualPosition, control.bufferedVisualPosition) * parent.width
            height: 4
            radius: 2
            color: "#70FFFFFF"
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: control.visualPosition * parent.width
            height: 4
            radius: 2
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition * (control.availableWidth - width)
        y: control.topPadding + (control.availableHeight - height) / 2
        width: control.pressed || control.hovered ? 18 : 14
        height: width
        radius: width / 2
        color: "white"
        border.width: 1
        border.color: "#33000000"

        Behavior on width { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }
    }
}
