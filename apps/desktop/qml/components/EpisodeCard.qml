import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Item {
    id: root
    property string title
    property string subtitle
    property url imageUrl
    property real progress: 0
    property bool played: false
    property string overview
    property var mediaItem: ({})
    signal playRequested()
    signal contextMenuRequested(var item, var sourceItem, real x, real y,
                                bool keyboardInvocation)

    function requestContextMenu() {
        root.contextMenuRequested(root.mediaItem, root,
                                  root.width / 2, root.height / 2, true)
    }

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.title + (root.subtitle.length > 0 ? ", " + root.subtitle : "")
        + (root.played ? ", " + qsTr("Played") : "")
    Accessible.onPressAction: root.playRequested()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            root.playRequested()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu
                   || (event.key === Qt.Key_F10
                       && (event.modifiers & Qt.ShiftModifier))) {
            root.requestContextMenu()
            event.accepted = true
        }
    }

    implicitWidth: 292
    implicitHeight: 264

    Rectangle {
        id: still
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 164
        radius: 18
        clip: true
        color: "#202633"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent
            : (hover.containsMouse ? Theme.outlineStrong : Theme.outline)

        RoundedImage {
            id: episodeImage
            anchors.fill: parent
            source: root.imageUrl
            radius: still.radius
            asynchronous: true
            cache: true
            fillMode: Image.PreserveAspectCrop
        }

        Rectangle {
            anchors.fill: parent
            radius: still.radius
            color: hover.containsMouse ? "#24000000" : "#08000000"
            Behavior on color { ColorAnimation { duration: 140 } }
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: root.forceActiveFocus(Qt.MouseFocusReason)
            onClicked: event => {
                if (event.button === Qt.RightButton)
                    root.contextMenuRequested(root.mediaItem, root,
                                              event.x, event.y, false)
                else
                    root.playRequested()
            }
        }

        AppButton {
            anchors.centerIn: parent
            z: 2
            kind: "primary"
            iconOnly: true
            iconName: "play"
            accessibleName: qsTr("Play")
            controlSize: 46
            iconSize: 23
            focusPolicy: Qt.NoFocus
            opacity: hover.containsMouse || hovered || root.activeFocus ? 1 : 0
            scale: opacity > 0 ? 1 : 0.86
            onClicked: root.playRequested()

            Behavior on opacity { NumberAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
        }

        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: Theme.spacing / 2
            width: 26
            height: 26
            radius: height / 2
            z: 3
            visible: opacity > 0
            opacity: root.played ? 1 : 0
            scale: root.played ? 1 : 0.82
            color: Theme.accent
            border.width: 1
            border.color: "#66FFFFFF"

            Canvas {
                id: playedCheck
                anchors.fill: parent
                antialiasing: true

                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()

                onPaint: {
                    const context = getContext("2d")
                    const side = Math.min(width, height)
                    const iconWidth = side * 0.51
                    const iconHeight = iconWidth * 0.70
                    const left = (width - iconWidth) / 2
                    const top = (height - iconHeight) / 2

                    context.reset()
                    context.strokeStyle = "white"
                    context.lineWidth = side * 0.092
                    context.lineCap = "round"
                    context.lineJoin = "round"
                    context.beginPath()
                    context.moveTo(left, top + iconHeight * 0.53)
                    context.lineTo(left + iconWidth * 0.34, top + iconHeight)
                    context.lineTo(left + iconWidth, top)
                    context.stroke()
                }
            }

            Behavior on opacity { NumberAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
        }

        Rectangle {
            visible: opacity > 0
            opacity: root.progress > 0 && root.progress < 100 ? 1 : 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 3
            anchors.rightMargin: 3
            anchors.bottomMargin: 3
            height: 4
            radius: 2
            color: "#60FFFFFF"

            Behavior on opacity { NumberAnimation { duration: 150 } }

            Rectangle {
                width: parent.width * Math.min(1, Math.max(0, root.progress / 100))
                height: parent.height
                radius: parent.radius
                color: Theme.accent
                Behavior on width { NumberAnimation { duration: 210; easing.type: Easing.OutCubic } }
            }
        }
    }

    Column {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: still.bottom
        anchors.topMargin: 11
        spacing: 3

        Text {
            width: parent.width
            text: root.title
            color: Theme.text
            font.family: Theme.fontForText(text)
            font.pixelSize: 14
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }

        Text {
            width: parent.width
            text: root.subtitle
            color: Theme.textMuted
            font.family: Theme.fontForText(text)
            font.pixelSize: 12
            elide: Text.ElideRight
        }

        Text {
            width: parent.width
            visible: root.overview.trim().length > 0
            text: root.overview
            color: "#7F899B"
            font.family: Theme.fontForText(text)
            font.pixelSize: 11
            lineHeight: 1.2
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
    }
}
