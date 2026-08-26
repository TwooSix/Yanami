import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Effects
import Yanami.Ui

Item {
    id: root
    property string title
    property string subtitle
    property url imageUrl
    property real progress: 0
    property var mediaItem: ({})
    readonly property bool artworkReady: episodeImage.status === Image.Ready
    readonly property bool pointerMode:
        InputModality.modality === InputModality.Pointer
    readonly property bool pointerHovered:
        root.pointerMode && (hover.containsMouse || playButton.hovered)
    readonly property bool pointerPressed:
        root.pointerMode && hover.pressed
            && Boolean(hover.pressedButtons & Qt.LeftButton)
    readonly property bool navigationFocusVisible:
        root.activeFocus && InputModality.focusNavigationActive
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

    // The hit target remains full-size while only artwork is transformed.
    MouseArea {
        id: hover
        objectName: "media-card-hit-area"
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

    Rectangle {
        id: artwork
        objectName: "media-card-surface"
        anchors.fill: parent
        radius: 20
        clip: true
        color: "#202633"
        border.width: root.pointerHovered ? 0 : 1
        border.color: Theme.outline
        readonly property real texturePadding: 1

        Item {
            id: artworkMask
            x: -artwork.texturePadding
            y: -artwork.texturePadding
            width: artwork.width + artwork.texturePadding * 2
            height: artwork.height + artwork.texturePadding * 2
            visible: false
            layer.enabled: true

            Rectangle {
                anchors.fill: parent
                anchors.margins: artwork.texturePadding
                radius: artwork.radius
                color: "white"
            }
        }

        // Flatten the image and its darkening overlays before the hover
        // transform. Scaling separately rendered rounded layers can sample an
        // undarkened edge texel and expose it as a bright one-pixel seam.
        Item {
            id: artworkBackdrop
            objectName: "media-card-backdrop-layer"
            x: -artwork.texturePadding
            y: -artwork.texturePadding
            width: artwork.width + artwork.texturePadding * 2
            height: artwork.height + artwork.texturePadding * 2
            layer.enabled: true
            layer.smooth: true
            layer.effect: MultiEffect {
                maskEnabled: true
                maskSource: artworkMask
                maskThresholdMin: 0.5
                maskSpreadAtMin: 1.0
            }

            RoundedImage {
                id: episodeImage
                anchors.fill: parent
                anchors.margins: artwork.texturePadding
                source: root.imageUrl
                radius: 0
                asynchronous: true
                cache: true
                fillMode: Image.PreserveAspectCrop
            }

            Rectangle {
                anchors.fill: parent
                anchors.margins: artwork.texturePadding
                opacity: 1
                gradient: Gradient {
                    orientation: Gradient.Vertical
                    GradientStop { position: 0; color: "#08000000" }
                    GradientStop { position: 0.55; color: "#28000000" }
                    GradientStop { position: 1; color: "#E6080A0F" }
                }
                Behavior on opacity { NumberAnimation { duration: 180 } }
            }

            Rectangle {
                objectName: "media-card-pointer-scrim"
                anchors.fill: parent
                anchors.margins: artwork.texturePadding
                color: root.pointerPressed ? "#22000000" : "transparent"

                Behavior on color { ColorAnimation { duration: 110 } }
            }
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
            hoverEnabled: root.pointerMode
            opacity: root.pointerHovered || root.navigationFocusVisible ? 1 : 0.88
            onClicked: root.playRequested()
        }

        CardProgressStrip {
            anchors.fill: parent
            z: 4
            progress: root.progress
            radius: artwork.radius
        }

        Rectangle {
            objectName: "media-card-focus-frame"
            anchors.fill: parent
            z: 10
            radius: artwork.radius
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
}
