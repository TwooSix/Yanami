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
    readonly property bool pointerMode:
        InputModality.modality === InputModality.Pointer
    readonly property bool pointerHovered:
        root.pointerMode && (mouse.containsMouse || playButton.hovered)
    readonly property bool pointerPressed:
        root.pointerMode && mouse.pressed
            && Boolean(mouse.pressedButtons & Qt.LeftButton)
    readonly property bool navigationFocusVisible:
        root.activeFocus && InputModality.focusNavigationActive
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

    // The visual surface can scale without shrinking the pointer hit target.
    MouseArea {
        id: mouse
        objectName: "media-card-hit-area"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 246
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

    Rectangle {
        id: poster
        objectName: "media-card-surface"
        anchors.left: parent.left
        anchors.right: parent.right
        height: 246
        radius: Theme.radius
        color: root.posterColor
        border.width: 1
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
            objectName: "media-card-pointer-scrim"
            anchors.fill: parent
            radius: poster.radius
            color: root.pointerPressed ? "#22000000" : "transparent"
            Behavior on color { ColorAnimation { duration: 110 } }
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
            hoverEnabled: root.pointerMode
            visible: root.playable
            opacity: root.pointerHovered || root.navigationFocusVisible ? 1 : 0
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

        CardProgressStrip {
            anchors.fill: parent
            z: 4
            progress: root.progress
            radius: poster.radius
        }

        Rectangle {
            objectName: "media-card-focus-frame"
            anchors.fill: parent
            z: 10
            radius: poster.radius
            color: "transparent"
            visible: root.navigationFocusVisible
            border.width: 2
            border.color: Theme.accent
        }

        scale: root.pointerPressed ? 0.985
            : (root.pointerHovered ? 1.012 : 1)
        Behavior on scale {
            NumberAnimation {
                duration: root.pointerPressed ? 75 : 155
                easing.type: root.pointerPressed
                    ? Easing.OutCubic : Easing.OutBack
                easing.overshoot: 0.55
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
