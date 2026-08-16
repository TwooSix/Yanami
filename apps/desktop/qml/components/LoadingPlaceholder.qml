import QtQuick
import Yanami.Ui

Item {
    id: root

    property bool running: visible
    property real cornerRadius: Theme.radius
    property bool structured: false
    property real artworkHeight: height

    opacity: running ? pulseOpacity : 0
    property real pulseOpacity: 0.8

    Rectangle {
        anchors.fill: parent
        visible: !root.structured
        radius: root.cornerRadius
        color: "#303744"
        border.width: 1
        border.color: "#38FFFFFF"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: Math.min(root.height, root.artworkHeight)
        visible: root.structured
        radius: root.cornerRadius
        color: "#303744"
        border.width: 1
        border.color: "#38FFFFFF"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: Math.min(root.height, root.artworkHeight) + 13
        width: parent.width * 0.72
        height: 12
        visible: root.structured && y + height <= root.height
        radius: 6
        color: "#444D5C"
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.topMargin: Math.min(root.height, root.artworkHeight) + 36
        width: parent.width * 0.46
        height: 9
        visible: root.structured && y + height <= root.height
        radius: 5
        color: "#39414F"
    }

    SequentialAnimation on pulseOpacity {
        running: root.running
        loops: Animation.Infinite
        NumberAnimation {
            from: 0.8
            to: 1.0
            duration: 760
            easing.type: Easing.InOutSine
        }
        NumberAnimation {
            from: 1.0
            to: 0.8
            duration: 760
            easing.type: Easing.InOutSine
        }
    }

    Behavior on opacity {
        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
    }
}
