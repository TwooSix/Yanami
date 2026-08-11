import QtQuick
import QtQuick.Controls.Basic
import Yanami

Item {
    id: root
    Accessible.role: Accessible.ListItem
    Accessible.name: root.title + (root.subtitle.length > 0 ? ", " + root.subtitle : "")

    property string title
    property string subtitle
    property url imageUrl
    property real progress: 0
    property string overview
    signal playRequested()

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
        border.width: 1
        border.color: hover.containsMouse ? Theme.outlineStrong : Theme.outline

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
            onClicked: root.playRequested()
        }

        AppButton {
            anchors.centerIn: parent
            z: 2
            kind: "primary"
            iconOnly: true
            iconName: "play"
            controlSize: 48
            opacity: hover.containsMouse || hovered ? 1 : 0
            scale: opacity > 0 ? 1 : 0.86
            onClicked: root.playRequested()

            Behavior on opacity { NumberAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
        }

        Rectangle {
            visible: root.progress > 0 && root.progress < 100
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 3
            anchors.rightMargin: 3
            anchors.bottomMargin: 3
            height: 4
            radius: 2
            color: "#60FFFFFF"

            Rectangle {
                width: parent.width * Math.min(1, Math.max(0, root.progress / 100))
                height: parent.height
                radius: parent.radius
                color: Theme.accent
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
