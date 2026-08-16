import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Item {
    id: root
    property string title
    property string subtitle
    property string itemType
    property url posterUrl
    property real progress: 0
    property int unplayedCount: 0
    property var mediaItem: ({})
    property color posterColor: "#33405B"
    property bool playable: true
    signal activated()
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
    Accessible.onPressAction: root.activated()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            root.activated()
            event.accepted = true
        } else if (event.key === Qt.Key_Space) {
            if (root.playable)
                root.playRequested()
            else
                root.activated()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu
                   || (event.key === Qt.Key_F10
                       && (event.modifiers & Qt.ShiftModifier))) {
            root.requestContextMenu()
            event.accepted = true
        }
    }

    implicitWidth: 178
    implicitHeight: 302

    Rectangle {
        id: poster
        anchors.left: parent.left
        anchors.right: parent.right
        height: 246
        radius: Theme.radius
        color: root.posterColor
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent : "#24FFFFFF"
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
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            onPressed: root.forceActiveFocus(Qt.MouseFocusReason)
            onClicked: event => {
                if (event.button === Qt.RightButton)
                    root.contextMenuRequested(root.mediaItem, root,
                                              event.x, event.y, false)
                else
                    root.activated()
            }
        }

        AppButton {
            id: playButton
            anchors.centerIn: parent
            z: 2
            kind: "primary"
            iconOnly: true
            iconName: "play"
            accessibleName: qsTr("Play")
            iconSize: 23
            controlSize: 44
            focusPolicy: Qt.NoFocus
            visible: root.playable
            opacity: mouse.containsMouse || hovered || root.activeFocus ? 1 : 0
            scale: opacity > 0 ? 1 : 0.86
            onClicked: root.playRequested()

            Behavior on opacity { NumberAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
        }

        Rectangle {
            id: unreadBadge
            visible: opacity > 0
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 6
            z: 3
            width: Math.max(height, Math.ceil(unreadMetrics.tightBoundingRect.width) + 12)
            height: 26
            radius: height / 2
            color: Theme.accent
            border.width: 1
            border.color: "#55FFFFFF"
            opacity: root.unplayedCount > 0 ? 1 : 0
            scale: root.unplayedCount > 0 ? 1 : 0.84

            Behavior on opacity { NumberAnimation { duration: 150 } }
            Behavior on scale { NumberAnimation { duration: 180; easing.type: Easing.OutBack } }
            Behavior on width { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

            TextMetrics {
                id: unreadMetrics
                font: unreadLabel.font
                renderType: unreadLabel.renderType
                text: unreadLabel.text
            }

            Text {
                id: unreadLabel
                x: (unreadBadge.width - unreadMetrics.tightBoundingRect.width) / 2
                    - unreadMetrics.tightBoundingRect.x
                y: (unreadBadge.height - unreadMetrics.tightBoundingRect.height) / 2
                    - baselineOffset - unreadMetrics.tightBoundingRect.y
                text: root.unplayedCount > 99 ? "99+" : String(root.unplayedCount)
                color: "white"
                font.family: Theme.fontFamily
                font.pixelSize: 11
                font.weight: Font.Bold
            }
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
            color: "#70FFFFFF"

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
