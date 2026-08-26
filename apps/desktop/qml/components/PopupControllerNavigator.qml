import QtQuick
import Yanami.Ui

// Popup-local navigation stays active while the application-level navigator
// is intentionally suspended by PopupCoordinator. It only intercepts arrows
// from controllers/remotes; keyboard users retain native text editing and Tab.
Item {
    id: root

    property var popup: null
    property Item navigationRoot: popup ? popup.contentItem : null
    property bool navigationEnabled: true
    property bool horizontalNavigationEnabled: true
    property bool verticalNavigationEnabled: true
    readonly property bool active: navigationEnabled && popup && popup.opened
        && PopupCoordinator.isTop(popup)
        && (InputModality.modality === InputModality.Controller
            || InputModality.modality === InputModality.Remote)

    visible: false
    enabled: true
    width: 0
    height: 0

    function move(direction) {
        return navigator.move(direction)
    }

    function focusFirst() {
        return navigator.focusFirst(root.navigationRoot)
    }

    function focusedControlConsumes(propertyName) {
        if (!root.popup || !root.popup.popupWindow)
            return false
        let item = root.popup.popupWindow.activeFocusItem
        while (item && item !== root.navigationRoot) {
            if (navigator.propertyValue(item, propertyName, false) === true)
                return true
            item = item.parent
        }
        return false
    }

    SpatialFocusNavigator {
        id: navigator
        navigationRoot: root.navigationRoot
    }

    Shortcut {
        sequence: "Up"
        context: Qt.WindowShortcut
        enabled: root.active && root.verticalNavigationEnabled
            && !root.focusedControlConsumes(
                "controllerConsumesVerticalNavigation")
        onActivated: root.move("up")
    }
    Shortcut {
        sequence: "Down"
        context: Qt.WindowShortcut
        enabled: root.active && root.verticalNavigationEnabled
            && !root.focusedControlConsumes(
                "controllerConsumesVerticalNavigation")
        onActivated: root.move("down")
    }
    Shortcut {
        sequence: "Left"
        context: Qt.WindowShortcut
        enabled: root.active && root.horizontalNavigationEnabled
            && !root.focusedControlConsumes(
                "controllerConsumesHorizontalNavigation")
        onActivated: root.move("left")
    }
    Shortcut {
        sequence: "Right"
        context: Qt.WindowShortcut
        enabled: root.active && root.horizontalNavigationEnabled
            && !root.focusedControlConsumes(
                "controllerConsumesHorizontalNavigation")
        onActivated: root.move("right")
    }

    Connections {
        target: InputModality

        function onActionPressed(action, repeated) {
            if (!root.active)
                return
            if (action === InputModality.PageUp
                    || action === InputModality.ScrollUp) {
                navigator.scroll(root.navigationRoot, 0, -1,
                                 action === InputModality.PageUp)
            } else if (action === InputModality.PageDown
                    || action === InputModality.ScrollDown) {
                navigator.scroll(root.navigationRoot, 0, 1,
                                 action === InputModality.PageDown)
            } else if (action === InputModality.ScrollLeft) {
                navigator.scroll(root.navigationRoot, -1, 0, false)
            } else if (action === InputModality.ScrollRight) {
                navigator.scroll(root.navigationRoot, 1, 0, false)
            }
        }
    }
}
