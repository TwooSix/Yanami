import QtQuick
import QtQuick.Controls.Basic
import Yanami

Item {
    id: root
    Accessible.role: Accessible.ListItem
    Accessible.name: root.title + (root.subtitle.length > 0 ? ", " + root.subtitle : "")
    property string title
    property string subtitle
    property string itemType
    property url posterUrl
    property real progress: 0
    property int unplayedCount: 0
    property color posterColor: "#33405B"
    signal activated()
    signal playRequested()

    implicitWidth: 178
    implicitHeight: 302

    Rectangle {
        id: poster
        anchors.left: parent.left
        anchors.right: parent.right
        height: 246
        radius: Theme.radius
        color: root.posterColor
        border.color: "#24FFFFFF"
        clip: true

        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(root.posterColor, 1.35) }
            GradientStop { position: 1; color: Qt.darker(root.posterColor, 1.45) }
        }

        RoundedImage {
            id: posterImage
            anchors.fill: parent
            source: root.posterUrl
            radius: poster.radius
            asynchronous: true
            cache: true
            fillMode: Image.PreserveAspectCrop
        }

        Rectangle {
            anchors.fill: parent
            radius: poster.radius
            color: mouse.containsMouse ? "#18FFFFFF" : "transparent"
            Behavior on color { ColorAnimation { duration: 130 } }
        }

        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.activated()
        }

        AppButton {
            id: playButton
            anchors.centerIn: parent
            z: 2
            kind: "primary"
            iconOnly: true
            iconName: "play"
            iconSize: 19
            controlSize: 50
            opacity: mouse.containsMouse || hovered ? 1 : 0
            scale: opacity > 0 ? 1 : 0.86
            onClicked: root.playRequested()

            Behavior on opacity { NumberAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
        }

        Rectangle {
            visible: root.unplayedCount > 0
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 10
            anchors.rightMargin: 10
            z: 3
            width: Math.max(25, unreadLabel.implicitWidth + 12)
            height: 25
            radius: 13
            color: Theme.accent
            border.width: 1
            border.color: "#55FFFFFF"

            Text {
                id: unreadLabel
                anchors.centerIn: parent
                text: root.unplayedCount > 99 ? "99+" : String(root.unplayedCount)
                color: "white"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
            }
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
            color: "#70FFFFFF"

            Rectangle {
                width: parent.width * Math.min(1, Math.max(0, root.progress / 100))
                height: parent.height
                radius: parent.radius
                color: Theme.accent
            }
        }
    }

    Text {
        anchors.top: poster.bottom
        anchors.topMargin: 12
        width: parent.width
        text: root.title
        color: Theme.text
        elide: Text.ElideRight
        font.family: Theme.fontForText(root.title)
        font.pixelSize: 15
        font.weight: Font.DemiBold
    }
    Text {
        anchors.bottom: parent.bottom
        width: parent.width
        text: root.subtitle
        color: Theme.textMuted
        elide: Text.ElideRight
        font.family: Theme.fontForText(root.subtitle)
        font.pixelSize: 12
    }
}
