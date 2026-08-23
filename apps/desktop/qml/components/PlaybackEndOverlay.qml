import QtQuick
import QtQuick.Layouts
import Yanami.Ui

FocusScope {
    id: root

    property bool shown: false
    property bool retryMode: false
    property string heading: ""
    property string detail: ""

    signal doneRequested()
    signal replayRequested()
    signal retryRequested()

    visible: shown || opacity > 0.001
    enabled: shown
    focus: shown
    opacity: shown ? 1 : 0
    Accessible.role: Accessible.Pane
    Accessible.name: heading
    Accessible.description: detail

    function clearActionFocus() {
        replayButton.focus = false
        doneButton.focus = false
        retryButton.focus = false
    }

    function focusDefaultAction() {
        clearActionFocus()
        if (!InputModality.focusNavigationActive) {
            if (root.shown)
                root.forceActiveFocus(Qt.MouseFocusReason)
            return
        }
        const target = root.retryMode ? retryButton : doneButton
        if (root.shown && target.visible && target.enabled)
            target.forceActiveFocus(Qt.TabFocusReason)
    }

    onShownChanged: {
        if (shown)
            Qt.callLater(focusDefaultAction)
        else
            clearActionFocus()
    }
    onRetryModeChanged: {
        if (shown)
            Qt.callLater(focusDefaultAction)
    }

    Connections {
        target: InputModality

        function onModalityChanged() {
            if (root.shown)
                root.focusDefaultAction()
        }
    }

    Keys.priority: Keys.AfterItem
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Back) {
            root.doneRequested()
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                   || event.key === Qt.Key_Space) {
            if (root.retryMode)
                root.retryRequested()
            else
                root.doneRequested()
            event.accepted = true
        }
    }

    Rectangle {
        anchors.fill: parent
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0; color: "#52000000" }
            GradientStop { position: 0.52; color: "#76000000" }
            GradientStop { position: 1; color: "#B8000000" }
        }
    }

    MouseArea {
        id: inputBlocker
        objectName: "playbackEndInputBlocker"
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        preventStealing: true
        onPressed: mouse => mouse.accepted = true
        onClicked: mouse => mouse.accepted = true
    }

    WheelHandler {
        enabled: root.shown
        blocking: true
        onWheel: event => event.accepted = true
    }

    GlassPanel {
        id: panel
        objectName: "playbackEndPanel"
        z: 1
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Math.max(32, Math.min(72, root.height * 0.08))
        width: Math.min(560, Math.max(320, root.width - 64))
        height: content.implicitHeight + 52
        radius: Theme.radiusLarge
        color: "#EE1B1E27"
        border.color: "#38FFFFFF"
        opacity: root.shown ? 1 : 0
        scale: root.shown ? 1 : 0.96

        transform: Translate {
            y: root.shown ? 0 : 12
            Behavior on y {
                NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
            }
        }

        Behavior on opacity {
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
        Behavior on scale {
            NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
        }

        ColumnLayout {
            id: content
            anchors.fill: parent
            anchors.leftMargin: 26
            anchors.rightMargin: 26
            anchors.topMargin: 24
            anchors.bottomMargin: 28
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                spacing: 16

                Rectangle {
                    Layout.preferredWidth: 46
                    Layout.preferredHeight: 46
                    radius: 23
                    color: root.retryMode ? "#24FF879E" : "#2474DBA4"

                    AppIcon {
                        anchors.centerIn: parent
                        width: 22
                        height: 22
                        name: root.retryMode ? "info" : "check"
                        color: root.retryMode ? Theme.danger : Theme.success
                        strokeWidth: 2
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 5

                    Text {
                        Layout.fillWidth: true
                        text: root.heading
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 21
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: root.detail
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                        lineHeight: 1.18
                        wrapMode: Text.Wrap
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignRight
                spacing: 10

                AppButton {
                    id: replayButton
                    objectName: "playbackEndReplayButton"
                    visible: !root.retryMode
                    kind: "secondary"
                    iconName: "restart"
                    text: qsTr("Replay")
                    onClicked: root.replayRequested()

                    KeyNavigation.left: doneButton
                    KeyNavigation.right: doneButton
                    KeyNavigation.tab: doneButton
                    KeyNavigation.backtab: doneButton
                }

                AppButton {
                    id: doneButton
                    objectName: "playbackEndDoneButton"
                    kind: root.retryMode ? "secondary" : "primary"
                    text: qsTr("Done")
                    onClicked: root.doneRequested()

                    KeyNavigation.left: root.retryMode ? retryButton : replayButton
                    KeyNavigation.right: root.retryMode ? retryButton : replayButton
                    KeyNavigation.tab: root.retryMode ? retryButton : replayButton
                    KeyNavigation.backtab: root.retryMode ? retryButton : replayButton
                }

                AppButton {
                    id: retryButton
                    objectName: "playbackEndRetryButton"
                    visible: root.retryMode
                    kind: "primary"
                    iconName: "refresh"
                    text: qsTr("Retry")
                    onClicked: root.retryRequested()

                    KeyNavigation.left: doneButton
                    KeyNavigation.right: doneButton
                    KeyNavigation.tab: doneButton
                    KeyNavigation.backtab: doneButton
                }
            }
        }
    }

    Behavior on opacity {
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }
}
