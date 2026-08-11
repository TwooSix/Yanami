import QtQuick
import Yanami

Item {
    id: root

    implicitWidth: 46
    implicitHeight: 46

    Rectangle {
        anchors.fill: parent
        radius: 15
        border.width: 1
        border.color: "#42FFFFFF"

        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "#FF6687" }
            GradientStop { position: 1; color: "#B46BFF" }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 1
            height: parent.height * 0.48
            radius: 14
            color: "#18FFFFFF"
        }

        Canvas {
            id: mark
            anchors.fill: parent
            anchors.margins: 9
            antialiasing: true

            onWidthChanged: requestPaint()
            onHeightChanged: requestPaint()

            onPaint: {
                const context = getContext("2d")
                context.reset()
                context.scale(width / 28, height / 28)
                context.strokeStyle = "white"
                context.lineWidth = 3.2
                context.lineCap = "round"
                context.lineJoin = "round"
                context.beginPath()
                context.moveTo(5.5, 5.5)
                context.lineTo(14, 14)
                context.lineTo(22.5, 5.5)
                context.moveTo(14, 14)
                context.lineTo(14, 23)
                context.stroke()
            }
        }
    }
}
