import QtQuick
import Yanami.Ui

Item {
    id: root

    property var targetWindow
    property bool playerMode: false
    property bool fullScreenMode: false

    height: PopupCoordinator.applicationChromeHeight

    MouseArea {
        id: dragArea
        anchors.left: parent.left
        anchors.right: windowControls.visible ? windowControls.left : parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.playerMode ? 88 : 8
        enabled: root.targetWindow && !root.fullScreenMode
        property point pressPosition
        property bool moveStarted: false

        onPressed: mouse => {
            PopupCoordinator.noteApplicationChromePress()
            pressPosition = Qt.point(mouse.x, mouse.y)
            moveStarted = false
        }
        onPositionChanged: mouse => {
            if (!pressed || moveStarted || !root.targetWindow)
                return
            if (Math.abs(mouse.x - pressPosition.x) + Math.abs(mouse.y - pressPosition.y) > 6) {
                moveStarted = true
                root.targetWindow.startSystemMove()
            }
        }
        onDoubleClicked: {
            if (!root.targetWindow)
                return
            if (root.targetWindow.visibility === Window.Maximized)
                root.targetWindow.showNormal()
            else
                root.targetWindow.showMaximized()
        }

        Component.onCompleted:
            PopupCoordinator.registerApplicationChromeItem(dragArea)
        Component.onDestruction:
            PopupCoordinator.unregisterApplicationChromeItem(dragArea)
    }

    Row {
        id: windowControls
        visible: !root.playerMode
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.rightMargin: 7
        anchors.topMargin: 5
        spacing: 2

        Repeater {
            model: ["minimize", "maximize", "close"]

            Rectangle {
                id: control
                required property string modelData
                width: 40
                height: 31
                radius: 10
                color: controlMouse.containsMouse
                    ? (modelData === "close" ? "#D94A5360" : "#16FFFFFF")
                    : "transparent"

                Accessible.role: Accessible.Button
                Accessible.name: modelData

                AppIcon {
                    anchors.centerIn: parent
                    width: 15
                    height: 15
                    name: control.modelData === "minimize"
                        ? "window-minimize"
                        : (control.modelData === "close"
                            ? "window-close"
                            : (root.targetWindow && root.targetWindow.visibility === Window.Maximized
                                ? "window-restore" : "window-maximize"))
                    color: controlMouse.containsMouse ? Theme.text : "#AAB2C0"
                    strokeWidth: 1.55
                }

                MouseArea {
                    id: controlMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onPressed: PopupCoordinator.noteApplicationChromePress()
                    onClicked: {
                        if (!root.targetWindow)
                            return
                        if (control.modelData === "minimize") {
                            root.targetWindow.showMinimized()
                        } else if (control.modelData === "close") {
                            root.targetWindow.close()
                        } else if (root.targetWindow.visibility === Window.Maximized) {
                            root.targetWindow.showNormal()
                        } else {
                            root.targetWindow.showMaximized()
                        }
                    }

                    Component.onCompleted:
                        PopupCoordinator.registerApplicationChromeItem(controlMouse)
                    Component.onDestruction:
                        PopupCoordinator.unregisterApplicationChromeItem(controlMouse)
                }

                Behavior on color { ColorAnimation { duration: 120 } }
            }
        }
    }
}
