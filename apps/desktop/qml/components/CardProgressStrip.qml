import QtQuick
import QtQuick.Effects
import Yanami.Ui

Item {
    id: root

    objectName: "media-card-progress-strip"
    property real progress: 0
    property real radius: 0
    property real stripHeight: 4
    property color trackColor: "#60FFFFFF"

    visible: opacity > 0
    opacity: root.progress > 0 && root.progress < 100 ? 1 : 0

    Behavior on opacity { NumberAnimation { duration: 150 } }

    Rectangle {
        id: cardMask
        anchors.fill: parent
        radius: root.radius
        color: "white"
        visible: false
        layer.enabled: true
    }

    Item {
        anchors.fill: parent
        layer.enabled: root.visible
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: cardMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }

        Rectangle {
            objectName: "media-card-progress-track"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: root.stripHeight
            color: root.trackColor

            Rectangle {
                width: parent.width
                    * Math.min(1, Math.max(0, root.progress / 100))
                height: parent.height
                radius: root.stripHeight / 2
                color: Theme.accent

                Behavior on width {
                    NumberAnimation {
                        duration: 210
                        easing.type: Easing.OutCubic
                    }
                }
            }
        }
    }
}
