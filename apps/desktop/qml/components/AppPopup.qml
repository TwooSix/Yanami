import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Popup {
    id: root

    property int popupRole: PopupCoordinator.transientRole
    property bool dismissOnOutside: true
    property bool dismissOnEscape: true
    property bool dismissBlocked: false
    property bool dirty: false
    property bool confirmDirtyDismiss: false
    property bool takesFocus: true
    property bool blocksShortcuts: true
    property bool restoreFocus: true
    property bool exclusiveWithinScope: false
    property var exclusiveScope: null
    property var initialFocusTarget: null
    property var focusReturnTarget: null
    property bool managesOutsideDismissal: false
    property int stackOrder: 0

    readonly property var popupWindow: root.contentItem
        ? root.contentItem.Window.window : null

    property var _capturedFocus: null

    signal discardRequested(string reason)
    signal dismissedByUser(string reason)

    function requestDismiss(reason) {
        if (root.dismissBlocked)
            return false
        if (root.dirty && root.confirmDirtyDismiss) {
            root.discardRequested(String(reason || "unknown"))
            return false
        }
        root.close()
        root.dismissedByUser(String(reason || "unknown"))
        return true
    }

    function forceDismiss() {
        root.close()
        return true
    }

    function focusInitialTarget() {
        if (!root.opened || !root.takesFocus || !PopupCoordinator.isTop(root))
            return
        const target = root.initialFocusTarget
        if (target && target.visible !== false && target.enabled !== false) {
            if (typeof target.focusInput === "function")
                target.focusInput()
            else
                target.forceActiveFocus(Qt.PopupFocusReason)
        } else if (root.contentItem) {
            root.contentItem.forceActiveFocus(Qt.PopupFocusReason)
        }
    }

    function restorePreviousFocus() {
        if (!root.restoreFocus)
            return

        const popupWindow = root.popupWindow
        const activeItem = popupWindow ? popupWindow.activeFocusItem : null
        if (activeItem && !PopupCoordinator.isItemInside(activeItem, root.contentItem))
            return

        const target = root.focusReturnTarget || root._capturedFocus
        root._capturedFocus = null
        const nextPopup = PopupCoordinator.topPopup()
        if (nextPopup) {
            if (target && PopupCoordinator.isItemInside(
                    target, nextPopup.contentItem)
                    && target.visible !== false && target.enabled !== false) {
                target.forceActiveFocus(Qt.PopupFocusReason)
            } else {
                nextPopup.focusInitialTarget()
            }
            return
        }

        if (target && target.visible !== false && target.enabled !== false)
            target.forceActiveFocus(Qt.PopupFocusReason)
    }

    focus: root.takesFocus
    z: PopupCoordinator.zFor(root)
    closePolicy: !root.dismissBlocked
            && !root.managesOutsideDismissal
            && root.dismissOnOutside
            && !(root.dirty && root.confirmDirtyDismiss)
        ? Popup.CloseOnPressOutside : Popup.NoAutoClose

    Connections {
        target: root

        function onAboutToShow() {
            const popupWindow = root.popupWindow
            root._capturedFocus = root.focusReturnTarget
                || (popupWindow ? popupWindow.activeFocusItem : null)
            PopupCoordinator.registerPopup(root)
        }

        function onOpened() {
            Qt.callLater(root.focusInitialTarget)
        }

        function onClosed() {
            PopupCoordinator.unregisterPopup(root)
            Qt.callLater(root.restorePreviousFocus)
        }
    }

    Component.onDestruction: PopupCoordinator.unregisterPopup(root)
}
