import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Item {
    id: root
    property string title
    property string subtitle
    property url imageUrl
    property real progress: 0
    property var mediaItem: ({})
    readonly property bool artworkReady: episodeImage.status === Image.Ready
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
    implicitHeight: 176

    Rectangle {
        id: artwork
        anchors.fill: parent
        radius: 20
        clip: true
        color: "#202633"
        border.width: root.activeFocus ? 2 : 1
        border.color: root.activeFocus ? Theme.accent
            : (hover.containsMouse ? Theme.outlineStrong : Theme.outline)

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
            opacity: 1
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
            id: playButton
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 14
            anchors.bottomMargin: 14
            z: 2
            kind: "primary"
            iconOnly: true
            iconName: "play"
            accessibleName: qsTr("Play")
            controlSize: 44
            iconSize: 23
            focusPolicy: Qt.NoFocus
            opacity: hover.containsMouse || hovered ? 1 : 0.88
            onClicked: root.playRequested()
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
            height: 3
            radius: 2
            color: "#50FFFFFF"

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
}
