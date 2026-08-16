pragma Singleton

import QtQuick

QtObject {
    id: coordinator

    readonly property int transientRole: 0
    readonly property int modalRole: 1
    readonly property int confirmRole: 2
    readonly property int errorRole: 3
    // Native window chrome must remain interactive while an in-window popup
    // is open, most importantly so dragging the title bar never dismisses it.
    readonly property int applicationChromeZ: 5000000
    readonly property int toolTipZ: applicationChromeZ - 1
    readonly property int applicationChromeHeight: 42
    property int applicationChromePressSerial: 0
    property double applicationChromePressTimestamp: 0
    property int popupContentPressSerial: 0

    property var popupStack: []
    property var overlayClickTargets: []
    property var applicationChromeItems: []
    property int nextStackOrder: 0

    readonly property bool hasOpenPopup: topPopup() !== null
    readonly property bool blocksApplicationShortcuts: hasBlockingPopup()

    function noteApplicationChromePress() {
        ++coordinator.applicationChromePressSerial
        // Overlay.pressed and the title bar MouseArea can be delivered in
        // either order. Keep a short-lived time marker so an overlay handler
        // that runs after the title bar press still recognises window chrome.
        coordinator.applicationChromePressTimestamp = Date.now()
    }

    function notePopupContentPress() {
        ++coordinator.popupContentPressSerial
    }

    function registerOverlayClickTarget(target) {
        if (!target)
            return
        const next = coordinator.overlayClickTargets.filter(function(candidate) {
            return candidate && candidate !== target
        })
        next.push(target)
        coordinator.overlayClickTargets = next
    }

    function unregisterOverlayClickTarget(target) {
        coordinator.overlayClickTargets = coordinator.overlayClickTargets.filter(
            function(candidate) {
                return candidate && candidate !== target
            })
    }

    function overlayClickTargetAt(overlayItem, position) {
        if (!overlayItem)
            return null
        const live = []
        let result = null
        for (let index = 0;
                index < coordinator.overlayClickTargets.length; ++index) {
            const target = coordinator.overlayClickTargets[index]
            if (!target)
                continue
            live.push(target)
            if (!target.visible || !target.enabled || target.width <= 0
                    || target.height <= 0)
                continue
            const local = target.mapFromItem(
                overlayItem, position.x, position.y)
            if (local.x >= 0 && local.x <= target.width
                    && local.y >= 0 && local.y <= target.height)
                result = target
        }
        if (live.length !== coordinator.overlayClickTargets.length)
            coordinator.overlayClickTargets = live
        return result
    }

    function registerApplicationChromeItem(item) {
        if (!item)
            return
        const next = coordinator.applicationChromeItems.filter(
            function(candidate) {
                return candidate && candidate !== item
            })
        next.push(item)
        coordinator.applicationChromeItems = next
    }

    function unregisterApplicationChromeItem(item) {
        coordinator.applicationChromeItems =
            coordinator.applicationChromeItems.filter(function(candidate) {
                return candidate && candidate !== item
            })
    }

    function applicationChromeAt(overlayItem, position) {
        if (!overlayItem)
            return false
        const live = []
        let result = false
        for (let index = 0;
                index < coordinator.applicationChromeItems.length; ++index) {
            const item = coordinator.applicationChromeItems[index]
            if (!item)
                continue
            live.push(item)
            if (!item.visible || !item.enabled || item.width <= 0
                    || item.height <= 0)
                continue
            const local = item.mapFromItem(
                overlayItem, position.x, position.y)
            if (local.x >= 0 && local.x <= item.width
                    && local.y >= 0 && local.y <= item.height)
                result = true
        }
        if (live.length !== coordinator.applicationChromeItems.length)
            coordinator.applicationChromeItems = live
        return result
    }

    function popupWindow(popup) {
        return popup && popup.popupWindow ? popup.popupWindow : null
    }

    function scopeFor(popup) {
        if (!popup)
            return null
        return popup.exclusiveScope || popupWindow(popup)
    }

    function isPopupInScope(popup, scope) {
        if (!popup || !scope)
            return false
        if (scopeFor(popup) === scope || popupWindow(popup) === scope)
            return true
        return popup.parent && coordinator.isItemInside(popup.parent, scope)
    }

    function livePopups() {
        const result = []
        for (let index = 0; index < coordinator.popupStack.length; ++index) {
            const popup = coordinator.popupStack[index]
            if (popup && (popup.visible || popup.opened))
                result.push(popup)
        }
        return result
    }

    function registerPopup(popup) {
        if (!popup)
            return

        let next = livePopups()
        if (next.indexOf(popup) >= 0)
            return

        if (popup.exclusiveWithinScope) {
            const scope = scopeFor(popup)
            for (let index = 0; index < next.length; ++index) {
                const candidate = next[index]
                if (candidate !== popup
                        && candidate.exclusiveWithinScope
                        && scopeFor(candidate) === scope)
                    candidate.requestDismiss("superseded")
            }
            next = livePopups()
        }

        ++coordinator.nextStackOrder
        popup.stackOrder = coordinator.nextStackOrder
        next.push(popup)
        coordinator.popupStack = next
    }

    function unregisterPopup(popup) {
        const next = []
        for (let index = 0; index < coordinator.popupStack.length; ++index) {
            const candidate = coordinator.popupStack[index]
            if (candidate && candidate !== popup
                    && (candidate.visible || candidate.opened))
                next.push(candidate)
        }
        coordinator.popupStack = next
        if (next.length === 0)
            coordinator.nextStackOrder = 0
    }

    function layerBase(role) {
        switch (Number(role)) {
        case coordinator.errorRole: return 4000000
        case coordinator.confirmRole: return 3000000
        case coordinator.modalRole: return 2000000
        default: return 1000000
        }
    }

    function zFor(popup) {
        return layerBase(popup ? popup.popupRole : coordinator.transientRole)
            + Number(popup ? popup.stackOrder : 0)
    }

    function topPopup() {
        const live = livePopups()
        let result = null
        let resultZ = -Infinity
        for (let index = 0; index < live.length; ++index) {
            const popup = live[index]
            const popupZ = Number(popup.z)
            if (!result || popupZ > resultZ
                    || (popupZ === resultZ
                        && popup.stackOrder > result.stackOrder)) {
                result = popup
                resultZ = popupZ
            }
        }
        return result
    }

    function isTop(popup) {
        return popup && topPopup() === popup
    }

    function hasBlockingPopup() {
        const live = livePopups()
        for (let index = 0; index < live.length; ++index) {
            if (live[index].blocksShortcuts)
                return true
        }
        return false
    }

    // This is the single Escape entry point for the application window. A
    // popup that cannot currently be dismissed still consumes Escape so that
    // navigation never happens behind a modal operation or dirty editor.
    function dismissTopOrNavigate(navigateCallback) {
        const popup = topPopup()
        if (popup) {
            if (popup.dismissOnEscape)
                popup.requestDismiss("escape")
            return true
        }
        if (typeof navigateCallback === "function")
            navigateCallback()
        return false
    }

    // Closes popups owned by a page/component before that scope is hidden.
    // Transient popups can be force-closed during teardown; callers should use
    // the guarded default for editable/modal content.
    function closeScope(scope, force) {
        const candidates = livePopups().slice()
        candidates.sort(function(left, right) {
            return Number(right.z) - Number(left.z)
        })

        let handled = 0
        for (let index = 0; index < candidates.length; ++index) {
            const popup = candidates[index]
            if (!isPopupInScope(popup, scope))
                continue
            ++handled
            if (force === true)
                popup.forceDismiss()
            else
                popup.requestDismiss("scope-close")
        }
        return handled
    }

    function isItemInside(item, ancestor) {
        let current = item
        while (current) {
            if (current === ancestor)
                return true
            current = current.parent
        }
        return false
    }
}
