import QtQuick
import QtQuick.Controls.Basic
import Yanami

Item {
    id: root

    property string title
    property string subtitle
    property url imageUrl
    property color fallbackColor: "#39465D"
    signal activated()

    Accessible.role: Accessible.ListItem
    Accessible.name: root.title + ", " + root.subtitle
    implicitWidth: 286
    implicitHeight: 148

    Rectangle {
        id: card
        anchors.fill: parent
        radius: 22
        color: root.fallbackColor
        border.width: 1
        border.color: hover.hovered ? Theme.outlineStrong : Theme.outline
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(root.fallbackColor, 1.28) }
            GradientStop { position: 1; color: Qt.darker(root.fallbackColor, 1.3) }
        }

        RoundedImage {
            id: artwork
            anchors.fill: parent
            source: root.imageUrl
            radius: card.radius
            opacity: status === Image.Ready ? 0.78 : 0

            Behavior on opacity { NumberAnimation { duration: 220 } }
        }

        Rectangle {
            anchors.fill: parent
            radius: card.radius
            gradient: Gradient {
                orientation: Gradient.Vertical
                GradientStop { position: 0; color: "#10000000" }
                GradientStop { position: 1; color: "#D20A0C12" }
            }
        }

        Column {
            anchors.left: parent.left
            anchors.right: arrow.left
            anchors.bottom: parent.bottom
            anchors.margins: 18
            spacing: 4

            Text {
                width: parent.width
                text: root.title
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 18
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: root.subtitle
                color: "#BBC3D1"
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        Text {
            id: arrow
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 18
            anchors.bottomMargin: 18
            text: "›"
            color: hover.hovered ? Theme.accent : "#B8FFFFFF"
            font.family: Theme.fontFamily
            font.pixelSize: 28
            font.weight: Font.Medium

            Behavior on color { ColorAnimation { duration: 130 } }
        }

        HoverHandler { id: hover }
        TapHandler { onTapped: root.activated() }

        scale: hover.hovered ? 1.012 : 1
        Behavior on scale { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
    }
}
