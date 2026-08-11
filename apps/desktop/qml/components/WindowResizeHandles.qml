import QtQuick

Item {
    id: root

    property var targetWindow
    property int handleSize: 6
    readonly property bool resizeEnabled: targetWindow
        && targetWindow.visibility !== Window.Maximized
        && targetWindow.visibility !== Window.FullScreen

    enabled: resizeEnabled

    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.handleSize
        cursorShape: Qt.SizeHorCursor
        onPressed: root.targetWindow.startSystemResize(Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: root.handleSize
        cursorShape: Qt.SizeHorCursor
        onPressed: root.targetWindow.startSystemResize(Qt.RightEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.handleSize
        cursorShape: Qt.SizeVerCursor
        onPressed: root.targetWindow.startSystemResize(Qt.TopEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: root.handleSize
        cursorShape: Qt.SizeVerCursor
        onPressed: root.targetWindow.startSystemResize(Qt.BottomEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.top: parent.top
        width: root.handleSize * 2
        height: root.handleSize * 2
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.targetWindow.startSystemResize(Qt.TopEdge | Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.top: parent.top
        width: root.handleSize * 2
        height: root.handleSize * 2
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.targetWindow.startSystemResize(Qt.TopEdge | Qt.RightEdge)
    }
    MouseArea {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        width: root.handleSize * 2
        height: root.handleSize * 2
        cursorShape: Qt.SizeBDiagCursor
        onPressed: root.targetWindow.startSystemResize(Qt.BottomEdge | Qt.LeftEdge)
    }
    MouseArea {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        width: root.handleSize * 2
        height: root.handleSize * 2
        cursorShape: Qt.SizeFDiagCursor
        onPressed: root.targetWindow.startSystemResize(Qt.BottomEdge | Qt.RightEdge)
    }
}
