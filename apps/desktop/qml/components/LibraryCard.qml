import QtQuick
import Yanami.Ui

Item {
    id: root

    property string title
    property string subtitle
    property url imageUrl
    property color fallbackColor: "#39465D"
    property var mediaItem: ({})
    property real scanProgress: -1
    property bool scanIndicatorShown: scanProgress >= 0
    readonly property bool pointerMode:
        InputModality.modality === InputModality.Pointer
    readonly property bool pointerHovered:
        root.pointerMode && hover.containsMouse
    readonly property bool pointerPressed:
        root.pointerMode && hover.pressed
            && Boolean(hover.pressedButtons & Qt.LeftButton)
    readonly property bool navigationFocusVisible:
        root.activeFocus && InputModality.focusNavigationActive
    signal activated()
    signal contextMenuRequested(var item, var sourceItem, real x, real y,
                                bool keyboardInvocation)

    function requestContextMenu() {
        root.contextMenuRequested(root.mediaItem, root,
                                  root.width / 2, root.height / 2, true)
    }

    activeFocusOnTab: true
    Accessible.role: Accessible.Button
    Accessible.name: root.title + ", " + root.subtitle
        + (root.scanProgress > 0
           ? ", " + qsTr("Scanning %1 percent").arg(Math.round(root.scanProgress))
           : (root.scanProgress >= 0 ? ", " + qsTr("Scanning") : ""))
    Accessible.onPressAction: root.activated()
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                || event.key === Qt.Key_Space) {
            root.activated()
            event.accepted = true
        } else if (event.key === Qt.Key_Menu
                   || (event.key === Qt.Key_F10
                       && (event.modifiers & Qt.ShiftModifier))) {
            root.requestContextMenu()
            event.accepted = true
        }
    }
    implicitWidth: 286
    implicitHeight: 148

    // Keep pointer hit-testing in the card's untransformed coordinate space;
    // only the visual surface below scales during press/release feedback.
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
                root.activated()
        }
    }

    Rectangle {
        id: card
        objectName: "media-card-surface"
        anchors.fill: parent
        radius: 22
        color: root.fallbackColor
        border.width: 1
        border.color: Theme.outline
        gradient: Gradient {
            GradientStop { position: 0; color: Qt.lighter(root.fallbackColor, 1.28) }
            GradientStop { position: 1; color: Qt.darker(root.fallbackColor, 1.3) }
        }

        RoundedImage {
            id: artwork
            anchors.fill: parent
            source: root.imageUrl
            radius: card.radius
            opacity: status === Image.Ready ? (root.scanProgress >= 0 ? 0.66 : 0.78) : 0

            Behavior on opacity { NumberAnimation { duration: 220 } }
        }

        Rectangle {
            anchors.fill: parent
            radius: card.radius
            visible: root.scanProgress >= 0
            color: "#1C05070B"
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

        Rectangle {
            objectName: "media-card-pointer-scrim"
            anchors.fill: parent
            radius: card.radius
            color: root.pointerPressed ? "#22000000" : "transparent"

            Behavior on color { ColorAnimation { duration: 110 } }
        }


        Item {
            id: scanIndicator
            anchors.centerIn: parent
            width: 54
            height: 54
            visible: opacity > 0.01
            opacity: root.scanIndicatorShown ? 1 : 0
            scale: root.scanIndicatorShown ? 1 : 0.88

            Behavior on opacity { NumberAnimation { duration: 170 } }
            Behavior on scale { NumberAnimation { duration: 190; easing.type: Easing.OutCubic } }

            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "#A8141820"
                border.width: 1
                border.color: "#30FFFFFF"
            }

            Item {
                id: progressRing
                anchors.centerIn: parent
                width: 44
                height: 44
                property real displayedProgress: Math.max(0, Math.min(100, root.scanProgress))

                Behavior on displayedProgress {
                    enabled: root.scanProgress >= 0
                    NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
                }

                Canvas {
                    id: progressTrack
                    anchors.fill: parent
                    onPaint: {
                        const context = getContext("2d")
                        context.clearRect(0, 0, width, height)
                        const center = width / 2
                        const radius = width / 2 - 2
                        context.lineWidth = 2.25
                        context.lineCap = "round"
                        context.strokeStyle = "#28FFFFFF"
                        context.beginPath()
                        context.arc(center, center, radius, 0, Math.PI * 2)
                        context.stroke()
                    }
                }

                Canvas {
                    anchors.fill: parent
                    visible: progressRing.displayedProgress > 0
                    property real progress: progressRing.displayedProgress
                    onProgressChanged: requestPaint()
                    onPaint: {
                        const context = getContext("2d")
                        context.clearRect(0, 0, width, height)
                        const center = width / 2
                        const radius = width / 2 - 2
                        context.lineWidth = 2.25
                        context.lineCap = "round"
                        context.strokeStyle = Theme.accent
                        context.beginPath()
                        context.arc(center, center, radius, -Math.PI / 2,
                                    -Math.PI / 2 + Math.PI * 2 * progress / 100)
                        context.stroke()
                    }
                }

                Item {
                    id: indeterminateArc
                    anchors.fill: parent
                    visible: root.scanProgress >= 0 && progressRing.displayedProgress <= 0

                    Canvas {
                        anchors.fill: parent
                        onPaint: {
                            const context = getContext("2d")
                            context.clearRect(0, 0, width, height)
                            const center = width / 2
                            const radius = width / 2 - 2
                            context.lineWidth = 2.25
                            context.lineCap = "round"
                            context.strokeStyle = Theme.accent
                            context.beginPath()
                            context.arc(center, center, radius, -Math.PI / 2,
                                        -Math.PI / 2 + Math.PI * 0.42)
                            context.stroke()
                        }
                    }

                    RotationAnimation on rotation {
                        running: indeterminateArc.visible
                        from: 0
                        to: 360
                        duration: 1050
                        loops: Animation.Infinite
                        easing.type: Easing.InOutSine
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: progressRing.displayedProgress > 0
                text: Math.round(progressRing.displayedProgress) + "%"
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
                font.weight: Font.Medium
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
            color: root.navigationFocusVisible ? Theme.accent
                : (root.pointerHovered ? Theme.text : "#B8FFFFFF")
            font.family: Theme.fontFamily
            font.pixelSize: 28
            font.weight: Font.Medium

            Behavior on color { ColorAnimation { duration: 130 } }
        }

        Rectangle {
            objectName: "media-card-focus-frame"
            anchors.fill: parent
            z: 10
            radius: card.radius
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
