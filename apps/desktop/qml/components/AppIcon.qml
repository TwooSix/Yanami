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
            } else if (root.name === "info") {
                context.beginPath()
                context.arc(12, 12, 8, 0, Math.PI * 2)
                context.stroke()
                context.beginPath()
                context.arc(12, 8, 1.15, 0, Math.PI * 2)
                context.fill()
                context.beginPath()
                context.moveTo(12, 11.3)
                context.lineTo(12, 16.8)
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
            } else if (root.name === "gear") {
                for (let index = 0; index < 8; ++index) {
                    const angle = index * Math.PI / 4
                    context.beginPath()
                    context.moveTo(12 + Math.cos(angle) * 6.7,
                                   12 + Math.sin(angle) * 6.7)
                    context.lineTo(12 + Math.cos(angle) * 9.1,
                                   12 + Math.sin(angle) * 9.1)
                    context.stroke()
                }
                context.beginPath()
                context.arc(12, 12, 6.8, 0, Math.PI * 2)
                context.stroke()
                context.beginPath()
                context.arc(12, 12, 2.5, 0, Math.PI * 2)
                context.stroke()
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
            } else if (root.name === "open") {
                context.beginPath()
                context.moveTo(8.5, 5.5)
                context.lineTo(15, 12)
                context.lineTo(8.5, 18.5)
                context.stroke()
            } else if (root.name === "restart") {
                context.beginPath()
                context.arc(12, 12, 7, -0.55, Math.PI * 1.55)
                context.stroke()
                context.beginPath()
                context.moveTo(5.1, 7.3)
                context.lineTo(5.4, 12)
                context.lineTo(9.8, 10.3)
                context.stroke()
            } else if (root.name === "check") {
                context.beginPath()
                context.moveTo(5.5, 12.3)
                context.lineTo(10, 16.6)
                context.lineTo(18.7, 7.4)
                context.stroke()
            } else if (root.name === "danmaku-scroll") {
                // Two compact comment rows mirror the familiar danmaku-list
                // metaphor while keeping the symbol legible at 19 px.
                context.beginPath()
                context.roundedRect(4.5, 5, 15, 14, 2.6, 2.6)
                context.stroke()

                const rows = [9.4, 14.6]
                for (let index = 0; index < rows.length; ++index) {
                    context.beginPath()
                    context.arc(7.5, rows[index], 0.72, 0, Math.PI * 2)
                    context.fill()
                    context.beginPath()
                    context.moveTo(9.6, rows[index])
                    context.lineTo(16.5, rows[index])
                    context.stroke()
                }
            } else if (root.name === "danmaku-top"
                       || root.name === "danmaku-bottom") {
                const top = root.name === "danmaku-top"
                context.beginPath()
                context.roundedRect(4.5, 5, 15, 14, 2.6, 2.6)
                context.stroke()

                const markerY = top ? 9 : 15
                for (let index = 0; index < 5; ++index) {
                    context.beginPath()
                    context.arc(7.8 + index * 2.1, markerY, 0.72, 0, Math.PI * 2)
                    context.fill()
                }
            } else if (root.name === "circle") {
                context.beginPath()
                context.arc(12, 12, 7.2, 0, Math.PI * 2)
                context.stroke()
            } else if (root.name === "heart" || root.name === "heart-filled") {
                context.beginPath()
                context.moveTo(12, 19)
                context.bezierCurveTo(10, 17.2, 5.2, 13.7, 5.2, 9.6)
                context.bezierCurveTo(5.2, 5.8, 9.8, 4.4, 12, 7.5)
                context.bezierCurveTo(14.2, 4.4, 18.8, 5.8, 18.8, 9.6)
                context.bezierCurveTo(18.8, 13.7, 14, 17.2, 12, 19)
                context.closePath()
                if (root.name === "heart-filled")
                    context.fill()
                else
                    context.stroke()
            } else if (root.name === "edit") {
                context.beginPath()
                context.moveTo(6, 18)
                context.lineTo(8.2, 12.8)
                context.lineTo(15.9, 5.1)
                context.lineTo(18.9, 8.1)
                context.lineTo(11.2, 15.8)
                context.closePath()
                context.stroke()
                context.beginPath()
                context.moveTo(14.5, 6.5)
                context.lineTo(17.5, 9.5)
                context.stroke()
            } else if (root.name === "image") {
                context.beginPath()
                context.roundedRect(4.5, 5.5, 15, 13, 2.4, 2.4)
                context.stroke()
                context.beginPath()
                context.arc(9, 9.4, 1.35, 0, Math.PI * 2)
                context.stroke()
                context.beginPath()
                context.moveTo(6.5, 16)
                context.lineTo(10.3, 12.1)
                context.lineTo(13, 14.5)
                context.lineTo(15.5, 11.8)
                context.lineTo(18, 14.4)
                context.stroke()
            } else if (root.name === "upload" || root.name === "download") {
                const downloading = root.name === "download"
                context.beginPath()
                context.moveTo(12, downloading ? 5 : 17)
                context.lineTo(12, downloading ? 15 : 7)
                context.moveTo(8.5, downloading ? 11.5 : 10.5)
                context.lineTo(12, downloading ? 15 : 7)
                context.lineTo(15.5, downloading ? 11.5 : 10.5)
                context.stroke()
                context.beginPath()
                context.moveTo(6, 18.5)
                context.lineTo(18, 18.5)
                context.stroke()
            } else if (root.name === "globe") {
                context.beginPath()
                context.arc(12, 12, 7.2, 0, Math.PI * 2)
                context.moveTo(4.8, 12)
                context.lineTo(19.2, 12)
                context.moveTo(12, 4.8)
                context.bezierCurveTo(8.7, 7.6, 8.7, 16.4, 12, 19.2)
                context.moveTo(12, 4.8)
                context.bezierCurveTo(15.3, 7.6, 15.3, 16.4, 12, 19.2)
                context.stroke()
            } else if (root.name === "trash") {
                context.beginPath()
                context.moveTo(6.5, 8)
                context.lineTo(7.5, 19)
                context.lineTo(16.5, 19)
                context.lineTo(17.5, 8)
                context.moveTo(5, 6.5)
                context.lineTo(19, 6.5)
                context.moveTo(9.5, 6.5)
                context.lineTo(10.2, 4.5)
                context.lineTo(13.8, 4.5)
                context.lineTo(14.5, 6.5)
                context.stroke()
            } else if (root.name === "scan") {
                context.beginPath()
                context.moveTo(5, 8)
                context.lineTo(5, 5)
                context.lineTo(8, 5)
                context.moveTo(16, 5)
                context.lineTo(19, 5)
                context.lineTo(19, 8)
                context.moveTo(19, 16)
                context.lineTo(19, 19)
                context.lineTo(16, 19)
                context.moveTo(8, 19)
                context.lineTo(5, 19)
                context.lineTo(5, 16)
                context.moveTo(7, 12)
                context.lineTo(17, 12)
                context.stroke()
            } else if (root.name === "queue") {
                // Compact queue glyph: three centered bullets with calm text
                // rows. Playback is already expressed by the surrounding dock,
                // so an extra forward marker only adds visual noise here.
                const rows = [7, 12, 17]
                for (let index = 0; index < rows.length; ++index) {
                    context.beginPath()
                    context.arc(7, rows[index], 0.85, 0, Math.PI * 2)
                    context.fill()
                    context.beginPath()
                    context.moveTo(10, rows[index])
                    context.lineTo(18, rows[index])
                    context.stroke()
                }
            } else if (root.name === "playlist") {
                for (let row = 0; row < 3; ++row) {
                    const y = 7 + row * 5
                    context.beginPath()
                    context.moveTo(5, y)
                    context.lineTo(14, y)
                    context.stroke()
                }
                context.beginPath()
                context.moveTo(18, 8.5)
                context.lineTo(18, 16.5)
                context.moveTo(14, 12.5)
                context.lineTo(22, 12.5)
                context.stroke()
            } else if (root.name === "more") {
                for (let index = 0; index < 3; ++index) {
                    context.beginPath()
                    context.arc(6.5 + index * 5.5, 12, 1.15, 0, Math.PI * 2)
                    context.fill()
                }
            }
        }
    }
}
