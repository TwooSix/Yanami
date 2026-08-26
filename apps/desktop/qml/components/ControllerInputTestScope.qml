import QtQuick
import Yanami.Ui

Item {
    id: root

    property bool available: false
    readonly property bool running: ownership.running
    readonly property bool acquired: ownership.acquired

    visible: false
    enabled: false
    width: 0
    height: 0

    function start() {
        if (!root.available)
            return false
        ownership.running = true
        root.synchronizeOwnership()
        if (!ownership.acquired)
            ownership.running = false
        return ownership.acquired
    }

    function stop() {
        ownership.running = false
        root.synchronizeOwnership()
    }

    function handleAction(action, repeated) {
        if (!ownership.acquired)
            return false
        if (action === InputModality.Back) {
            root.stop()
            return true
        }
        return false
    }

    function synchronizeOwnership() {
        if (root.available && ownership.running) {
            if (!ownership.acquired) {
                ownership.acquired =
                    InputModality.acquireControllerInputTest(root)
            }
            return
        }

        if (ownership.acquired) {
            InputModality.releaseControllerInputTest(root)
            ownership.acquired = false
        }
    }

    QtObject {
        id: ownership
        property bool running: false
        property bool acquired: false
    }

    onAvailableChanged: {
        if (!root.available)
            root.stop()
    }
    Component.onCompleted: synchronizeOwnership()
    Component.onDestruction: {
        if (ownership.acquired)
            InputModality.releaseControllerInputTest(root)
    }
}
