pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import Yanami.Ui

Menu {
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
    property bool exclusiveWithinScope: true
    property var exclusiveScope: null
    property var focusReturnTarget: null
    property int preferredCurrentIndex: 0
    property int stackOrder: 0

    readonly property bool managesOutsideDismissal: true
    readonly property bool keyboardFocusVisible:
        InputModality.focusNavigationActive
    readonly property var popupWindow: root.contentItem
        ? root.contentItem.Window.window : null

    property var _capturedFocus: null
    property var _overlayTapHandler: null

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

    function openPreferred(keyboardInvocation) {
        // Callers already place AppMenu in the coordinate system of its
        // parent. Menu::popup(parent, x, y, item) applies a second positioning
        // pass whose semantics are "place item at this point", which moves an
        // anchored track menu and can reset an overlay context menu to (0, 0).
        // Plain open() preserves the caller's x/y; focusPreferredItem() owns
        // the semantic current-item hand-off after the popup is visible.
        if (Boolean(keyboardInvocation))
            InputModality.noteKeyboardNavigation()
        else
            InputModality.notePointerInput()
        root.currentIndex = -1
        root.open()
    }

    function focusPreferredItem() {
        if (!root.opened || !root.takesFocus || !PopupCoordinator.isTop(root)
                || root.count <= 0)
            return
        const index = Math.max(0, Math.min(
            root.count - 1, Number(root.preferredCurrentIndex)))
        const item = root.itemAt(index)
        if (!item || item.enabled === false)
            return

        // QQuickMenu owns the current-item transition. Resetting the index
        // first guarantees that it applies PopupFocusReason even when the
        // same entry was current the last time the menu was shown.
        root.currentIndex = -1
        root.currentIndex = index
        item.forceActiveFocus(Qt.PopupFocusReason)
    }

    function focusInitialTarget() {
        root.focusPreferredItem()
    }

    function focusItem(index) {
        if (!root.opened || index < 0 || index >= root.count)
            return
        const item = root.itemAt(index)
        if (item && item.enabled !== false) {
            root.currentIndex = index
            item.forceActiveFocus(Qt.TabFocusReason)
        }
    }

    function restorePreviousFocus() {
        if (!root.restoreFocus)
            return

        const popupWindow = root.popupWindow
        const activeItem = popupWindow ? popupWindow.activeFocusItem : null
        if (activeItem && !PopupCoordinator.isItemInside(
                activeItem, root.contentItem)) {
            root._capturedFocus = null
            return
        }

        const target = root.focusReturnTarget || root._capturedFocus
        root._capturedFocus = null
        const nextPopup = PopupCoordinator.topPopup()
        if (nextPopup) {
            if (target && PopupCoordinator.isItemInside(
                    target, nextPopup.contentItem)
                    && target.visible !== false && target.enabled !== false) {
                target.forceActiveFocus(Qt.PopupFocusReason)
            } else if (typeof nextPopup.focusInitialTarget === "function") {
                nextPopup.focusInitialTarget()
            } else if (typeof nextPopup.focusPreferredItem === "function") {
                nextPopup.focusPreferredItem()
            }
            return
        }

        if (target && target.visible !== false && target.enabled !== false)
            target.forceActiveFocus(Qt.PopupFocusReason)
    }

    function dismissFromOutside() {
        if (root.opened && root.dismissOnOutside
                && PopupCoordinator.isTop(root))
            root.requestDismiss("outside")
    }

    function containsOverlayPoint(item, overlayItem, position) {
        if (!item || !overlayItem || item.visible === false)
            return false
        const local = item.mapFromItem(
            overlayItem, position.x, position.y)
        return local.x >= 0 && local.x <= item.width
            && local.y >= 0 && local.y <= item.height
    }

    function handleOverlayTap(overlayItem, position) {
        if (!root.opened || !overlayItem || !PopupCoordinator.isTop(root))
            return

        const inside = root.containsOverlayPoint(
                root.contentItem, overlayItem, position)
            || root.containsOverlayPoint(
                root.background, overlayItem, position)
        const applicationChrome = PopupCoordinator.applicationChromeAt(
            overlayItem, position)
        if (inside || applicationChrome)
            return

        const clickTarget = PopupCoordinator.overlayClickTargetAt(
            overlayItem, position)
        if (clickTarget
                && typeof clickTarget.triggerFromOverlayClick === "function") {
            clickTarget.triggerFromOverlayClick()
            return
        }

        Qt.callLater(function() {
            if (root.opened)
                root.dismissFromOutside()
        })
    }

    function installOverlayTapHandler() {
        if (root._overlayTapHandler || !root.Overlay.overlay)
            return
        root._overlayTapHandler = overlayTapHandlerComponent.createObject(
            root.Overlay.overlay, { "parent": root.Overlay.overlay })
    }

    function removeOverlayTapHandler() {
        const handler = root._overlayTapHandler
        root._overlayTapHandler = null
        if (handler)
            handler.destroy()
    }

    modal: false
    dim: false
    focus: root.takesFocus
    popupType: Popup.Item
    margins: 8
    z: PopupCoordinator.zFor(root)
    // Escape and outside presses have one application-level route through
    // PopupCoordinator. Letting Menu auto-close as well can close the menu and
    // navigate with the same key event.
    closePolicy: Popup.NoAutoClose

    Connections {
        target: root

        function onAboutToShow() {
            const popupWindow = root.popupWindow
            root._capturedFocus = root.focusReturnTarget
                || (popupWindow ? popupWindow.activeFocusItem : null)
            PopupCoordinator.registerPopup(root)
        }

        function onOpened() {
            root.installOverlayTapHandler()
            // Re-apply the item focus after Menu has finished taking focus so
            // the final accessibility focus event belongs to the MenuItem,
            // rather than the popup container.
            const indexAtOpen = root.currentIndex
            Qt.callLater(function() {
                // Never overwrite navigation that happened before this
                // queued accessibility/focus hand-off ran.
                if (root.opened && root.currentIndex === indexAtOpen)
                    root.focusPreferredItem()
            })
        }

        function onClosed() {
            root.removeOverlayTapHandler()
            PopupCoordinator.unregisterPopup(root)
            Qt.callLater(root.restorePreviousFocus)
        }
    }

    Component {
        id: overlayTapHandlerComponent

        TapHandler {
            target: null
            // Left presses are classified here so popup buttons can switch in
            // one gesture. Right presses remain available to the underlying
            // media card, whose context-menu request supersedes this menu via
            // PopupCoordinator instead of requiring a second click.
            acceptedButtons: Qt.LeftButton
            gesturePolicy: TapHandler.DragThreshold
            onTapped: (eventPoint, button) => {
                const overlayItem = root.Overlay.overlay
                root.handleOverlayTap(overlayItem, eventPoint.position)
            }
        }
    }

    Component.onDestruction: {
        root.removeOverlayTapHandler()
        PopupCoordinator.unregisterPopup(root)
    }
}
