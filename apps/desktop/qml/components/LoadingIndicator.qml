import QtQuick
import Yanami.Ui

Item {
    id: root

    property real indicatorSize: 28
    property bool running: visible
    property color accentColor: Theme.text
    property color outlineColor: "transparent"
    property real outlineWidth: 0

    implicitWidth: indicatorSize
    implicitHeight: indicatorSize

    Item {
        id: spinner
        anchors.centerIn: parent
        width: root.indicatorSize
        height: root.indicatorSize

        Item {
            id: orbit
            anchors.fill: parent

            Repeater {
                model: 12

                Rectangle {
                    required property int index
                    width: Math.max(2, root.indicatorSize * 0.072)
                    height: Math.max(6, root.indicatorSize * 0.21)
                    x: (orbit.width - width) / 2
                    y: 0
                    radius: width / 2
                    color: root.accentColor
                    border.width: root.outlineWidth
                    border.color: root.outlineColor
                    opacity: 0.12 + ((index + 1) / 12) * 0.8
                    transform: Rotation {
                        origin.x: width / 2
                        origin.y: orbit.height / 2
                        angle: index * 30
                    }
                }
            }

            RotationAnimator on rotation {
                from: 0
                to: 360
                duration: 980
                loops: Animation.Infinite
                running: root.running
            }
        }
    }
}
