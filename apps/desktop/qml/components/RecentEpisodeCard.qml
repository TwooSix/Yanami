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
    readonly property bool artworkReady: episodeImage.status === Image.Ready
    signal playRequested()

    implicitWidth: 292
    implicitHeight: 176

    Rectangle {
        id: artwork
        anchors.fill: parent
        radius: 20
        clip: true
        color: "#202633"
        border.width: 1
        border.color: hover.containsMouse ? Theme.outlineStrong : Theme.outline

        RoundedImage {
            id: episodeImage
            anchors.fill: parent
            source: root.imageUrl
            radius: artwork.radius
            asynchronous: true
            cache: true
            fillMode: Image.PreserveAspectCrop
        }

        Rectangle {
            anchors.fill: parent
            radius: artwork.radius
            opacity: root.artworkReady ? 1 : 0
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0; color: "#08000000" }
                GradientStop { position: 0.55; color: "#28000000" }
                GradientStop { position: 1; color: "#E6080A0F" }
            }
            Behavior on opacity { NumberAnimation { duration: 180 } }
        }

        Column {
            anchors.left: parent.left
            anchors.right: playButton.left
            anchors.bottom: parent.bottom
            anchors.leftMargin: 18
            anchors.rightMargin: 12
            anchors.bottomMargin: 16
            spacing: 4
            opacity: root.artworkReady ? 1 : 0

            Behavior on opacity { NumberAnimation { duration: 180 } }

            Text {
                width: parent.width
                text: root.title
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 16
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.subtitle
                color: "#C1C8D5"
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        MouseArea {
            id: hover
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.playRequested()
        }

        AppButton {
            id: playButton
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 14
            anchors.bottomMargin: 14
            z: 2
            kind: "primary"
            iconOnly: true
            iconName: "play"
            controlSize: 42
            opacity: !root.artworkReady ? 0 : (hover.containsMouse || hovered ? 1 : 0.88)
            onClicked: root.playRequested()
        }

        Rectangle {
            visible: root.artworkReady && root.progress > 0 && root.progress < 100
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 3
            anchors.rightMargin: 3
            anchors.bottomMargin: 3
            height: 3
            radius: 2
            color: "#50FFFFFF"

            Rectangle {
                width: parent.width * Math.min(1, Math.max(0, root.progress / 100))
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        }
    }
}
