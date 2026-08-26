import QtQuick

Item {
    id: root

    property Item navigationRoot: null
    property var pageBookmarks: ({})
    property var pendingView: null
    property int pendingIndex: -1
    property int pendingAttempts: 0

    visible: false
    enabled: false
    width: 0
    height: 0

    function propertyValue(object, name, fallback) {
        if (!object)
            return fallback
        try {
            const value = object[name]
            return value === undefined ? fallback : value
        } catch (error) {
            return fallback
        }
    }

    function isScrollable(item) {
        return item
            && propertyValue(item, "contentItem", null)
            && propertyValue(item, "contentWidth", undefined) !== undefined
            && propertyValue(item, "contentHeight", undefined) !== undefined
    }

    function isVisibleInNavigationRoot(item) {
        if (!item || !root.navigationRoot || item === root.navigationRoot)
            return false

        let ancestor = item
        while (ancestor && ancestor !== root.navigationRoot) {
            if (propertyValue(ancestor, "visible", true) === false
                    || propertyValue(ancestor, "enabled", true) === false
                    || Number(propertyValue(ancestor, "opacity", 1)) <= 0.01) {
                return false
            }

            // A non-scrolling clip is a real focus-scope boundary. Scrollable
            // views are allowed to contribute cached/off-screen delegates;
            // focusItem() reveals them before the hand-off completes.
            if (propertyValue(ancestor, "clip", false)
                    && !root.isScrollable(ancestor)) {
                const center = item.mapToItem(
                    ancestor, item.width / 2, item.height / 2)
                if (center.x < 0 || center.x > ancestor.width
                        || center.y < 0 || center.y > ancestor.height) {
                    return false
                }
            }
            ancestor = ancestor.parent
        }
        return ancestor === root.navigationRoot
    }

    function isFocusable(item) {
        if (!root.isVisibleInNavigationRoot(item)
                || Number(item.width) <= 1 || Number(item.height) <= 1)
            return false

        if (propertyValue(item, "controllerFocusable", false) === true)
            return true
        if (propertyValue(item, "activeFocusOnTab", false) === true)
            return true

        const focusPolicy = propertyValue(item, "focusPolicy", Qt.NoFocus)
        return focusPolicy !== Qt.NoFocus
            && typeof item.forceActiveFocus === "function"
    }

    function collectFocusable(scope) {
        const candidates = []
        function visit(item) {
            if (!item || root.propertyValue(item, "visible", true) === false)
                return
            if (root.isFocusable(item))
                candidates.push(item)
            const children = root.propertyValue(item, "children", []) || []
            for (let index = 0; index < children.length; ++index)
                visit(children[index])
        }
        visit(scope || root.navigationRoot)
        return candidates
    }

    function itemRect(item) {
        const topLeft = item.mapToItem(root.navigationRoot, 0, 0)
        const bottomRight = item.mapToItem(
            root.navigationRoot, item.width, item.height)
        return {
            "left": Math.min(topLeft.x, bottomRight.x),
            "right": Math.max(topLeft.x, bottomRight.x),
            "top": Math.min(topLeft.y, bottomRight.y),
            "bottom": Math.max(topLeft.y, bottomRight.y),
            "centerX": (topLeft.x + bottomRight.x) / 2,
            "centerY": (topLeft.y + bottomRight.y) / 2
        }
    }

    function overlap(firstStart, firstEnd, secondStart, secondEnd) {
        return Math.max(0, Math.min(firstEnd, secondEnd)
                        - Math.max(firstStart, secondStart))
    }

    function candidateScore(sourceRect, candidateRect, direction) {
        const horizontal = direction === "left" || direction === "right"
        const sign = direction === "left" || direction === "up" ? -1 : 1
        const primaryDelta = horizontal
            ? (candidateRect.centerX - sourceRect.centerX) * sign
            : (candidateRect.centerY - sourceRect.centerY) * sign
        if (primaryDelta <= 2)
            return Infinity

        const crossDelta = horizontal
            ? Math.abs(candidateRect.centerY - sourceRect.centerY)
            : Math.abs(candidateRect.centerX - sourceRect.centerX)
        const crossOverlap = horizontal
            ? root.overlap(sourceRect.top, sourceRect.bottom,
                           candidateRect.top, candidateRect.bottom)
            : root.overlap(sourceRect.left, sourceRect.right,
                           candidateRect.left, candidateRect.right)

        // Staying on the same visual row/column is the dominant intuition.
        // Once that axis no longer overlaps, cross-axis drift is deliberately
        // expensive so a nearer diagonal control does not steal focus.
        return primaryDelta
            + crossDelta * (crossOverlap > 0 ? 2 : 4)
            + (crossOverlap > 0 ? 0 : 1000)
    }

    function viewIndexForItem(view, item) {
        if (!view || !item)
            return -1
        if (typeof view.indexAt === "function") {
            const point = item.mapToItem(
                view.contentItem, item.width / 2, item.height / 2)
            const index = Number(view.indexAt(point.x, point.y))
            if (index >= 0)
                return index
        }
        return Number(propertyValue(view, "currentIndex", -1))
    }

    function revealItem(item) {
        if (!item)
            return

        let ancestor = item.parent
        while (ancestor && ancestor !== root.navigationRoot) {
            if (typeof ancestor.positionViewAtIndex === "function"
                    && typeof ancestor.itemAtIndex === "function") {
                const index = root.viewIndexForItem(ancestor, item)
                if (index >= 0) {
                    if (typeof ancestor.prepareForFocusReveal === "function")
                        ancestor.prepareForFocusReveal()
                    ancestor.currentIndex = index
                    ancestor.positionViewAtIndex(index, ListView.Contain)
                }
            } else if (root.isScrollable(ancestor)) {
                const topLeft = item.mapToItem(ancestor.contentItem, 0, 0)
                const bottomRight = item.mapToItem(
                    ancestor.contentItem, item.width, item.height)
                const margin = 18
                let targetX = Number(ancestor.contentX)
                let targetY = Number(ancestor.contentY)
                if (topLeft.x < targetX + margin)
                    targetX = topLeft.x - margin
                else if (bottomRight.x > targetX + ancestor.width - margin)
                    targetX = bottomRight.x - ancestor.width + margin
                if (topLeft.y < targetY + margin)
                    targetY = topLeft.y - margin
                else if (bottomRight.y > targetY + ancestor.height - margin)
                    targetY = bottomRight.y - ancestor.height + margin

                const maximumX = Math.max(0,
                    Number(ancestor.contentWidth) - ancestor.width)
                const maximumY = Math.max(0,
                    Number(ancestor.contentHeight) - ancestor.height)
                if (typeof ancestor.revealContentX === "function")
                    ancestor.revealContentX(targetX)
                else
                    ancestor.contentX = Math.max(0, Math.min(maximumX, targetX))
                if (typeof ancestor.revealContentY === "function")
                    ancestor.revealContentY(targetY)
                else if (typeof ancestor.scrollToContentY === "function")
                    ancestor.scrollToContentY(targetY)
                else
                    ancestor.contentY = Math.max(0, Math.min(maximumY, targetY))
            }
            ancestor = ancestor.parent
        }
    }

    function focusItem(item, reason) {
        if (!root.isFocusable(item))
            return false
        // An explicit focus hand-off supersedes any older virtual delegate
        // request. Otherwise the next repeat can accumulate from a stale
        // pendingIndex even though focus has already moved elsewhere.
        pendingFocusTimer.stop()
        root.pendingView = null
        root.pendingIndex = -1
        root.pendingAttempts = 0
        root.revealItem(item)
        item.forceActiveFocus(reason === undefined
                              ? Qt.TabFocusReason : reason)
        return item.activeFocus === true
    }

    function firstFocusable(scope) {
        const candidates = root.collectFocusable(scope)
        candidates.sort(function(left, right) {
            const leftRect = root.itemRect(left)
            const rightRect = root.itemRect(right)
            if (Math.abs(leftRect.top - rightRect.top) > 8)
                return leftRect.top - rightRect.top
            return leftRect.left - rightRect.left
        })
        return candidates.length > 0 ? candidates[0] : null
    }

    function focusFirst(scope) {
        return root.focusItem(root.firstFocusable(scope))
    }

    function remember(pageKey, item) {
        if (item)
            root.pageBookmarks[String(pageKey)] = item
    }

    function restore(pageKey, scope) {
        const target = root.pageBookmarks[String(pageKey)]
        if (root.focusItem(target))
            return true
        return root.focusFirst(scope)
    }

    function containingVirtualView(item) {
        let ancestor = item ? item.parent : null
        while (ancestor && ancestor !== root.navigationRoot) {
            if (typeof ancestor.positionViewAtIndex === "function"
                    && typeof ancestor.itemAtIndex === "function"
                    && Number(propertyValue(ancestor, "count", 0)) > 0
                    && (propertyValue(ancestor,
                        "controllerVirtualNavigationEnabled", false) === true
                        || propertyValue(ancestor,
                            "keyNavigationEnabled", true) !== false)) {
                return ancestor
            }
            ancestor = ancestor.parent
        }
        return null
    }

    function moveInVirtualView(current, direction) {
        let view = root.containingVirtualView(current)
        if (!view && root.pendingView && root.pendingIndex >= 0
                && (!root.isFocusable(current)
                    || current === root.pendingView)) {
            // A ListView may recycle the old focused delegate while the next
            // one is still being instantiated. Keep consuming repeats in the
            // pending virtual view instead of falling back to page geometry.
            view = root.pendingView
        }
        if (!view)
            return false

        // positionViewAtIndex() can make the next delegate visible before the
        // 16 ms focus hand-off timer runs. Controller repeats arriving in that
        // window must advance from the requested model index, not request the
        // old focused delegate's neighbour again.
        const pendingInThisView = root.pendingView === view
            && root.pendingIndex >= 0
        const currentIndex = pendingInThisView
            ? root.pendingIndex : root.viewIndexForItem(view, current)
        const count = Number(view.count)
        if (currentIndex < 0 || currentIndex >= count)
            return false

        let targetIndex = -1
        const horizontal = Number(propertyValue(view, "orientation", -1))
            === Qt.Horizontal
        const cellWidth = Number(propertyValue(view, "cellWidth", 0))
        const cellHeight = Number(propertyValue(view, "cellHeight", 0))
        if (horizontal) {
            if (direction === "left")
                targetIndex = currentIndex - 1
            else if (direction === "right")
                targetIndex = currentIndex + 1
        } else if (cellWidth > 0 && cellHeight > 0) {
            const columns = Math.max(1, Math.floor(view.width / cellWidth))
            if (direction === "left"
                    && currentIndex % columns > 0)
                targetIndex = currentIndex - 1
            else if (direction === "right"
                     && currentIndex % columns < columns - 1)
                targetIndex = currentIndex + 1
            else if (direction === "up")
                targetIndex = currentIndex - columns
            else if (direction === "down")
                targetIndex = currentIndex + columns
        } else {
            // A vertical ListView has no cell geometry. Keep model order just
            // like horizontal rails so navigation can cross its delegate
            // cache instead of relying on whichever delegates exist now.
            if (direction === "up")
                targetIndex = currentIndex - 1
            else if (direction === "down")
                targetIndex = currentIndex + 1
        }

        if (targetIndex < 0 || targetIndex >= count)
            return pendingInThisView

        root.pendingView = view
        root.pendingIndex = targetIndex
        root.pendingAttempts = 0
        if (typeof view.prepareForFocusReveal === "function")
            view.prepareForFocusReveal()
        view.currentIndex = targetIndex
        view.positionViewAtIndex(targetIndex, ListView.Contain)
        pendingFocusTimer.restart()
        return true
    }

    function move(direction) {
        if (!root.navigationRoot || !root.navigationRoot.Window.window)
            return false
        const current = root.navigationRoot.Window.window.activeFocusItem

        // Preserve model order inside virtualized rails/grids before looking
        // at geometry. Otherwise the nearest instantiated card in another
        // section can win when the next delegate has not been created yet.
        if (root.moveInVirtualView(current, direction))
            return true
        if (!root.isFocusable(current))
            return root.focusFirst(root.navigationRoot)

        const sourceRect = root.itemRect(current)
        const candidates = root.collectFocusable(root.navigationRoot)
        let best = null
        let bestScore = Infinity
        for (let index = 0; index < candidates.length; ++index) {
            const candidate = candidates[index]
            if (candidate === current)
                continue
            const score = root.candidateScore(
                sourceRect, root.itemRect(candidate), direction)
            if (score < bestScore) {
                best = candidate
                bestScore = score
            }
        }

        return best ? root.focusItem(best) : false
    }

    function scroll(scope, horizontalDelta, verticalDelta, pageStep) {
        if (!root.navigationRoot || !root.navigationRoot.Window.window)
            return false
        let item = root.navigationRoot.Window.window.activeFocusItem
        let chosen = null
        while (item && item !== root.navigationRoot) {
            if (root.isScrollable(item)) {
                const horizontalRange = Number(item.contentWidth) - item.width
                const verticalRange = Number(item.contentHeight) - item.height
                if ((horizontalDelta !== 0 && horizontalRange > 1)
                        || (verticalDelta !== 0 && verticalRange > 1)) {
                    chosen = item
                    break
                }
            }
            item = item.parent
        }

        if (!chosen) {
            const candidates = []
            function findScrollers(candidate) {
                if (!candidate || root.propertyValue(
                        candidate, "visible", true) === false)
                    return
                if (root.isScrollable(candidate))
                    candidates.push(candidate)
                const children = root.propertyValue(
                    candidate, "children", []) || []
                for (let index = 0; index < children.length; ++index)
                    findScrollers(children[index])
            }
            findScrollers(scope || root.navigationRoot)
            for (let index = 0; index < candidates.length; ++index) {
                const candidate = candidates[index]
                if ((horizontalDelta !== 0
                        && Number(candidate.contentWidth) > candidate.width + 1)
                        || (verticalDelta !== 0
                            && Number(candidate.contentHeight) > candidate.height + 1)) {
                    chosen = candidate
                    break
                }
            }
        }
        if (!chosen)
            return false

        const horizontalStep = pageStep
            ? Math.max(120, chosen.width * 0.82) : 88
        const verticalStep = pageStep
            ? Math.max(160, chosen.height * 0.82) : 88
        const maximumX = Math.max(0,
            Number(chosen.contentWidth) - chosen.width)
        const maximumY = Math.max(0,
            Number(chosen.contentHeight) - chosen.height)
        if (horizontalDelta !== 0) {
            const targetX = Math.max(0, Math.min(maximumX,
                Number(chosen.contentX) + horizontalDelta * horizontalStep))
            if (typeof chosen.scrollContentXBy === "function")
                chosen.scrollContentXBy(horizontalDelta * horizontalStep)
            else if (typeof chosen.revealContentX === "function")
                chosen.revealContentX(targetX)
            else
                chosen.contentX = targetX
        }
        if (verticalDelta !== 0) {
            const targetY = Math.max(0, Math.min(maximumY,
                Number(chosen.contentY) + verticalDelta * verticalStep))
            if (typeof chosen.scrollContentYBy === "function")
                chosen.scrollContentYBy(verticalDelta * verticalStep)
            else if (typeof chosen.scrollToContentY === "function")
                chosen.scrollToContentY(targetY)
            else
                chosen.contentY = targetY
        }
        return true
    }

    Timer {
        id: pendingFocusTimer
        interval: 16
        repeat: true
        onTriggered: {
            const view = root.pendingView
            if (!view || root.pendingIndex < 0) {
                stop()
                return
            }
            const delegate = view.itemAtIndex(root.pendingIndex)
            // A virtualized delegate can legitimately be absent for several
            // frames after positionViewAtIndex(). Do not pass null to
            // firstFocusable(): that helper intentionally falls back to the
            // whole navigation root and would jump focus to the page's first
            // control instead of waiting for the requested model item.
            const target = delegate
                ? (root.isFocusable(delegate)
                    ? delegate : root.firstFocusable(delegate))
                : null
            if (target && root.focusItem(target)) {
                stop()
                root.pendingView = null
                root.pendingIndex = -1
                return
            }
            ++root.pendingAttempts
            if (root.pendingAttempts >= 12) {
                stop()
                root.pendingView = null
                root.pendingIndex = -1
            }
        }
    }
}
