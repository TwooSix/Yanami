import QtQuick

Item {
    id: root

    property string name: "home"
    property color color: "white"
    property real strokeWidth: 1.8

    implicitWidth: 24
    implicitHeight: 24

    onNameChanged: canvas.requestPaint()
    onColorChanged: canvas.requestPaint()
    onStrokeWidthChanged: canvas.requestPaint()

    Canvas {
        id: canvas
        anchors.fill: parent
        antialiasing: true

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            const context = getContext("2d")
            context.reset()
            context.scale(width / 24, height / 24)
            context.strokeStyle = root.color
            context.fillStyle = root.color
            context.lineWidth = root.strokeWidth
            context.lineCap = "round"
            context.lineJoin = "round"

            if (root.name === "home") {
                context.beginPath()
                context.moveTo(4.5, 10.5)
                context.lineTo(12, 4.5)
                context.lineTo(19.5, 10.5)
                context.stroke()

                context.beginPath()
                context.moveTo(6.5, 9.5)
                context.lineTo(6.5, 19)
                context.lineTo(17.5, 19)
                context.lineTo(17.5, 9.5)
                context.stroke()

                context.beginPath()
                context.moveTo(10, 19)
                context.lineTo(10, 14)
                context.lineTo(14, 14)
                context.lineTo(14, 19)
                context.stroke()
            } else if (root.name === "search") {
                context.beginPath()
                context.arc(10.5, 10.5, 5.6, 0, Math.PI * 2)
                context.stroke()
                context.beginPath()
                context.moveTo(14.7, 14.7)
                context.lineTo(19.4, 19.4)
                context.stroke()
            } else if (root.name === "play") {
                context.beginPath()
                context.moveTo(8.3, 5.8)
                context.lineTo(18.2, 12)
                context.lineTo(8.3, 18.2)
                context.closePath()
                context.fill()
            } else if (root.name === "pause") {
                context.beginPath()
                context.roundedRect(7, 5.8, 3.6, 12.4, 1, 1)
                context.roundedRect(13.4, 5.8, 3.6, 12.4, 1, 1)
                context.fill()
            } else if (root.name === "previous-track"
                       || root.name === "next-track") {
                const mirrored = root.name === "previous-track"
                context.save()
                if (mirrored) {
                    context.translate(24, 0)
                    context.scale(-1, 1)
                }
                context.beginPath()
                context.roundedRect(17.2, 5.5, 2.2, 13, 0.8, 0.8)
                context.fill()
                context.beginPath()
                context.moveTo(6.2, 6.2)
                context.lineTo(16.2, 12)
                context.lineTo(6.2, 17.8)
                context.closePath()
                context.fill()
                context.restore()
            } else if (root.name === "back") {
                context.beginPath()
                context.moveTo(19, 12)
                context.lineTo(5, 12)
                context.moveTo(10.5, 6.5)
                context.lineTo(5, 12)
                context.lineTo(10.5, 17.5)
                context.stroke()
            } else if (root.name === "refresh") {
                context.beginPath()
                context.arc(12, 12, 7, Math.PI * 0.22, Math.PI * 1.82)
                context.stroke()
                context.beginPath()
                context.moveTo(17.8, 5.2)
                context.lineTo(18.6, 9.7)
                context.lineTo(14.2, 8.3)
                context.stroke()
            } else if (root.name === "settings") {
                const lines = [[5, 19, 8], [5, 19, 15], [5, 19, 10]]
                for (let index = 0; index < lines.length; ++index) {
                    const y = 6 + index * 6
                    const knob = lines[index][2]
                    context.beginPath()
                    context.moveTo(lines[index][0], y)
                    context.lineTo(lines[index][1], y)
                    context.stroke()
                    context.beginPath()
                    context.arc(knob, y, 2.1, 0, Math.PI * 2)
                    context.fill()
                }
            } else if (root.name === "window-minimize") {
                context.beginPath()
                context.moveTo(5.5, 12)
                context.lineTo(18.5, 12)
                context.stroke()
            } else if (root.name === "window-maximize") {
                context.strokeRect(5.5, 5.5, 13, 13)
            } else if (root.name === "window-restore") {
                context.strokeRect(7.5, 5.5, 11, 11)
                context.beginPath()
                context.moveTo(16.5, 16.5)
                context.lineTo(16.5, 18.5)
                context.lineTo(5.5, 18.5)
                context.lineTo(5.5, 7.5)
                context.lineTo(7.5, 7.5)
                context.stroke()
            } else if (root.name === "window-close") {
                context.beginPath()
                context.moveTo(6.5, 6.5)
                context.lineTo(17.5, 17.5)
                context.moveTo(17.5, 6.5)
                context.lineTo(6.5, 17.5)
                context.stroke()
            } else if (root.name === "fullscreen-enter") {
                context.beginPath()
                context.moveTo(9.5, 5.5)
                context.lineTo(5.5, 5.5)
                context.lineTo(5.5, 9.5)
                context.moveTo(14.5, 5.5)
                context.lineTo(18.5, 5.5)
                context.lineTo(18.5, 9.5)
                context.moveTo(18.5, 14.5)
                context.lineTo(18.5, 18.5)
                context.lineTo(14.5, 18.5)
                context.moveTo(9.5, 18.5)
                context.lineTo(5.5, 18.5)
                context.lineTo(5.5, 14.5)
                context.stroke()
            } else if (root.name === "fullscreen-exit") {
                context.beginPath()
                context.moveTo(5.5, 9.5)
                context.lineTo(9.5, 9.5)
                context.lineTo(9.5, 5.5)
                context.moveTo(18.5, 9.5)
                context.lineTo(14.5, 9.5)
                context.lineTo(14.5, 5.5)
                context.moveTo(14.5, 18.5)
                context.lineTo(14.5, 14.5)
                context.lineTo(18.5, 14.5)
                context.moveTo(9.5, 18.5)
                context.lineTo(9.5, 14.5)
                context.lineTo(5.5, 14.5)
                context.stroke()
            } else if (root.name === "volume-muted"
                       || root.name === "volume-low"
                       || root.name === "volume-high") {
                context.beginPath()
                context.moveTo(4.5, 10)
                context.lineTo(8, 10)
                context.lineTo(12.2, 6.5)
                context.lineTo(12.2, 17.5)
                context.lineTo(8, 14)
                context.lineTo(4.5, 14)
                context.closePath()
                context.stroke()

                if (root.name === "volume-muted") {
                    context.beginPath()
                    context.moveTo(15, 9)
                    context.lineTo(20, 15)
                    context.moveTo(20, 9)
                    context.lineTo(15, 15)
                    context.stroke()
                } else {
                    context.beginPath()
                    context.arc(12.5, 12, 4.2, -0.72, 0.72)
                    context.stroke()
                    if (root.name === "volume-high") {
                        context.beginPath()
                        context.arc(12.5, 12, 7.2, -0.72, 0.72)
                        context.stroke()
                    }
                }
            }
        }
    }
}
