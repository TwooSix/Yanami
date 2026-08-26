import QtQuick
import QtTest
import Yanami.Ui

Item {
    id: scene
    width: 640
    height: 520

    Keys.priority: Keys.AfterItem
    Keys.onPressed: event => {
        let direction = ""
        if (event.key === Qt.Key_Left)
            direction = "left"
        else if (event.key === Qt.Key_Right)
            direction = "right"
        else if (event.key === Qt.Key_Up)
            direction = "up"
        else if (event.key === Qt.Key_Down)
            direction = "down"
        if (direction.length > 0)
            event.accepted = navigator.move(direction)
    }

    SpatialFocusNavigator {
        id: navigator
        navigationRoot: scene
    }

    Rectangle {
        id: leftControl
        x: 30
        y: 30
        width: 100
        height: 48
        activeFocusOnTab: true
    }

    Rectangle {
        id: rightControl
        x: 230
        y: 30
        width: 100
        height: 48
        activeFocusOnTab: true
    }

    ListView {
        id: rail
        x: 30
        y: 150
        width: 176
        height: 64
        orientation: ListView.Horizontal
        spacing: 8
        clip: true
        model: 12
        currentIndex: -1

        delegate: Rectangle {
            required property int index
            width: 80
            height: 56
            activeFocusOnTab: true
        }
    }

    SmoothHorizontalList {
        id: horizontalRevealList
        x: 30
        y: 365
        width: 176
        height: 64
        orientation: ListView.Horizontal
        spacing: 8
        clip: true
        model: 16
        currentIndex: -1

        delegate: Rectangle {
            required property int index
            width: 80
            height: 56
            activeFocusOnTab: true
        }
    }

    Flickable {
        id: nestedScroller
        x: 380
        y: 140
        width: 150
        height: 84
        contentWidth: width
        contentHeight: 420
        clip: true

        Rectangle {
            id: nestedTarget
            x: 10
            y: 330
            width: 120
            height: 48
            activeFocusOnTab: true
        }
    }

    SmoothFlickable {
        id: focusRevealScroller
        x: 380
        y: 250
        width: 150
        height: 84
        contentWidth: width
        contentHeight: 420

        Rectangle {
            id: focusScrollAnchor
            x: 10
            y: 10
            width: 120
            height: 48
            activeFocusOnTab: true
        }

        Rectangle {
            id: focusRevealTarget
            x: 10
            y: 330
            width: 120
            height: 48
            activeFocusOnTab: true
        }
    }

    SmoothListView {
        id: virtualRevealList
        x: 220
        y: 250
        width: 130
        height: 84
        model: 12
        spacing: 4

        delegate: Rectangle {
            required property int index
            width: virtualRevealList.width
            height: 48
            activeFocusOnTab: true
        }
    }

    Loader {
        id: bookmarkLoader
        x: 500
        y: 30
        active: true
        sourceComponent: Rectangle {
            width: 90
            height: 44
            activeFocusOnTab: true
        }
    }

    Item {
        id: delayedView
        x: 30
        y: 260
        width: 176
        height: 64

        property int count: 2
        property int currentIndex: 0
        property int orientation: Qt.Horizontal
        property bool keyNavigationEnabled: true
        property int itemRequests: 0
        property int nullResponsesRemaining: 0

        function positionViewAtIndex(index, mode) {
        }

        function itemAtIndex(index) {
            ++itemRequests
            if (index === 1 && nullResponsesRemaining > 0) {
                --nullResponsesRemaining
                return null
            }
            return index === 0 ? delayedFirst : delayedSecond
        }

        Rectangle {
            id: delayedFirst
            width: 80
            height: 56
            activeFocusOnTab: true
        }

        Rectangle {
            id: delayedSecond
            x: 88
            width: 80
            height: 56
            activeFocusOnTab: true
        }
    }

    TestCase {
        name: "SpatialFocusNavigator"
        when: windowShown

        function init() {
            navigator.pendingView = null
            navigator.pendingIndex = -1
            navigator.pendingAttempts = 0
            rail.currentIndex = -1
            rail.positionViewAtBeginning()
            horizontalRevealList.currentIndex = -1
            horizontalRevealList.resetScrollPosition()
            delayedView.currentIndex = 0
            delayedView.itemRequests = 0
            delayedView.nullResponsesRemaining = 0
            nestedScroller.contentY = 0
            focusRevealScroller.revealContentY(0)
            virtualRevealList.revealContentY(virtualRevealList.originY)
            bookmarkLoader.active = true
            tryVerify(function() { return bookmarkLoader.item !== null })
            leftControl.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(leftControl, "activeFocus", true)
        }

        function test_geometry_navigation_and_bookmark_restore() {
            verify(navigator.isFocusable(leftControl))
            verify(navigator.isFocusable(rightControl))
            verify(navigator.collectFocusable(scene).length >= 2)
            verify(navigator.move("right"))
            tryCompare(rightControl, "activeFocus", true)

            navigator.remember("test-page", rightControl)
            leftControl.forceActiveFocus(Qt.TabFocusReason)
            verify(navigator.restore("test-page", scene))
            tryCompare(rightControl, "activeFocus", true)
        }

        function test_virtualized_rail_keeps_model_order() {
            const first = rail.itemAtIndex(0)
            verify(first !== null)
            first.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(first, "activeFocus", true)

            verify(navigator.move("right"))
            tryCompare(rail, "currentIndex", 1)
            tryVerify(function() {
                const second = rail.itemAtIndex(1)
                return second !== null && second.activeFocus
            })
        }

        function test_virtualized_rail_crosses_the_delegate_cache() {
            const first = rail.itemAtIndex(0)
            verify(first !== null)
            first.forceActiveFocus(Qt.TabFocusReason)
            for (let expected = 1; expected < rail.count; ++expected) {
                verify(navigator.move("right"))
                tryCompare(rail, "currentIndex", expected)
                tryVerify(function() {
                    const current = rail.itemAtIndex(expected)
                    return current !== null && current.activeFocus
                })
            }
        }

        function test_smooth_horizontal_rail_crosses_delegate_cache() {
            horizontalRevealList.forceLayout()
            const first = horizontalRevealList.itemAtIndex(0)
            verify(first !== null)
            verify(navigator.focusItem(first))
            tryCompare(first, "activeFocus", true)

            for (let expected = 1;
                    expected < horizontalRevealList.count; ++expected) {
                verify(navigator.move("right"))
                tryCompare(horizontalRevealList, "currentIndex", expected)
                tryVerify(function() {
                    const current = horizontalRevealList.itemAtIndex(expected)
                    return current !== null && current.activeFocus
                })

                const current = horizontalRevealList.itemAtIndex(expected)
                const topLeft = current.mapToItem(
                    horizontalRevealList.contentItem, 0, 0)
                verify(topLeft.x >= horizontalRevealList.contentX - 0.5)
                verify(topLeft.x + current.width
                       <= horizontalRevealList.contentX
                       + horizontalRevealList.width + 0.5)
                verify(!horizontalRevealList.wheelAnimationRunning)
            }
        }

        function test_repeated_horizontal_focus_moves_do_not_drop_steps() {
            horizontalRevealList.forceLayout()
            const first = horizontalRevealList.itemAtIndex(0)
            verify(first !== null)
            verify(navigator.focusItem(first))
            tryCompare(first, "activeFocus", true)

            // Deliberately issue both moves before the 16 ms pending-focus
            // timer can transfer focus to index 1.
            verify(navigator.move("right"))
            verify(navigator.move("right"))
            compare(horizontalRevealList.currentIndex, 2)
            compare(navigator.pendingIndex, 2)

            // Delegate recycling can temporarily return active focus to the
            // view while the requested delegate is still pending. A further
            // repeat must continue from index 2 rather than fall back to the
            // first control on the page.
            horizontalRevealList.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(horizontalRevealList, "activeFocus", true)
            verify(navigator.move("right"))
            compare(horizontalRevealList.currentIndex, 3)
            compare(navigator.pendingIndex, 3)
            tryVerify(function() {
                const fourth = horizontalRevealList.itemAtIndex(3)
                return fourth !== null && fourth.activeFocus
            })
            tryCompare(navigator, "pendingIndex", -1)
        }

        function test_horizontal_key_event_uses_spatial_navigation_path() {
            verify(!horizontalRevealList.keyNavigationEnabled)
            verify(horizontalRevealList.controllerVirtualNavigationEnabled)
            horizontalRevealList.forceLayout()
            const first = horizontalRevealList.itemAtIndex(0)
            verify(first !== null)
            verify(navigator.focusItem(first))
            tryCompare(first, "activeFocus", true)

            horizontalRevealList.scrollContentXBy(180)
            verify(horizontalRevealList.wheelAnimationRunning)
            wait(50)
            keyClick(Qt.Key_Right)

            tryCompare(horizontalRevealList, "currentIndex", 1)
            tryVerify(function() {
                const second = horizontalRevealList.itemAtIndex(1)
                return second !== null && second.activeFocus
            })
            verify(!horizontalRevealList.wheelAnimationRunning)
            const revealedContentX = horizontalRevealList.contentX
            wait(220)
            compare(horizontalRevealList.contentX, revealedContentX)
        }

        function test_virtualized_vertical_list_crosses_delegate_cache() {
            virtualRevealList.revealContentY(virtualRevealList.originY)
            const first = virtualRevealList.itemAtIndex(0)
            verify(first !== null)
            first.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(first, "activeFocus", true)

            for (let expected = 1;
                    expected < virtualRevealList.count; ++expected) {
                verify(navigator.move("down"))
                tryCompare(virtualRevealList, "currentIndex", expected)
                tryVerify(function() {
                    const current = virtualRevealList.itemAtIndex(expected)
                    return current !== null && current.activeFocus
                })
            }
        }

        function test_virtualized_delegate_waits_for_instantiation() {
            delayedView.currentIndex = 0
            delayedView.itemRequests = 0
            delayedView.nullResponsesRemaining = 8
            delayedFirst.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(delayedFirst, "activeFocus", true)

            verify(navigator.move("right"))
            compare(delayedView.currentIndex, 1)
            tryVerify(function() { return delayedView.itemRequests > 0 })

            // While itemAtIndex() is still null, focus must stay put and the
            // pending request must remain alive instead of falling back to the
            // first focusable control on the page.
            verify(delayedView.nullResponsesRemaining > 0)
            verify(delayedFirst.activeFocus)
            compare(navigator.pendingIndex, 1)

            tryCompare(delayedSecond, "activeFocus", true)
            compare(navigator.pendingIndex, -1)
            verify(delayedView.itemRequests >= 9)
        }

        function test_focus_reveals_nested_flickable() {
            compare(nestedScroller.contentY, 0)
            verify(navigator.focusItem(nestedTarget))
            tryVerify(function() { return nestedScroller.contentY > 0 })
            verify(nestedTarget.activeFocus)
        }

        function test_focus_reveal_is_synchronous_at_controller_repeat_rate() {
            compare(focusRevealScroller.contentY, 0)
            focusRevealScroller.scrollToContentY(120)
            verify(focusRevealScroller.wheelAnimationRunning)
            verify(navigator.focusItem(focusRevealTarget))
            verify(focusRevealTarget.activeFocus)
            verify(focusRevealScroller.contentY > 0)
            verify(!focusRevealScroller.wheelAnimationRunning)

            const revealedContentY = focusRevealScroller.contentY
            wait(220)
            compare(focusRevealScroller.contentY, revealedContentY)

            const topLeft = focusRevealTarget.mapToItem(
                focusRevealScroller.contentItem, 0, 0)
            const bottom = topLeft.y + focusRevealTarget.height
            verify(topLeft.y >= focusRevealScroller.contentY - 0.5)
            verify(bottom <= focusRevealScroller.contentY
                   + focusRevealScroller.height + 0.5)
        }

        function test_repeated_controller_scroll_accumulates_destinations() {
            focusRevealScroller.revealContentY(0)
            focusScrollAnchor.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(focusScrollAnchor, "activeFocus", true)

            verify(navigator.scroll(scene, 0, 1, false))
            verify(navigator.scroll(scene, 0, 1, false))
            tryCompare(focusRevealScroller, "contentY", 176, 500)
        }

        function test_repeated_horizontal_controller_scroll_accumulates_destinations() {
            horizontalRevealList.revealContentX(horizontalRevealList.originX)
            horizontalRevealList.forceLayout()
            const first = horizontalRevealList.itemAtIndex(0)
            verify(first !== null)
            first.forceActiveFocus(Qt.TabFocusReason)
            tryCompare(first, "activeFocus", true)

            verify(navigator.scroll(scene, 1, 0, false))
            verify(navigator.scroll(scene, 1, 0, false))
            tryCompare(horizontalRevealList, "contentX",
                       horizontalRevealList.originX + 176, 500)
        }

        function test_horizontal_scroll_clamps_at_both_boundaries() {
            horizontalRevealList.revealContentX(
                horizontalRevealList.originX - 1000)
            compare(horizontalRevealList.contentX,
                    horizontalRevealList.originX)

            const maximum = Math.max(horizontalRevealList.originX,
                horizontalRevealList.originX
                + horizontalRevealList.contentWidth
                - horizontalRevealList.width)
            horizontalRevealList.scrollContentXBy(100000)
            tryCompare(horizontalRevealList, "contentX", maximum, 500)
            verify(!horizontalRevealList.wheelAnimationRunning)

            horizontalRevealList.revealContentX(maximum + 1000)
            compare(horizontalRevealList.contentX, maximum)
            horizontalRevealList.scrollContentXBy(88)
            verify(!horizontalRevealList.wheelAnimationRunning)
        }

        function test_virtual_view_focus_cancels_pointer_scroll_animation() {
            const first = virtualRevealList.itemAtIndex(0)
            verify(first !== null)
            virtualRevealList.scrollToContentY(
                virtualRevealList.originY + 180)
            verify(virtualRevealList.wheelAnimationRunning)
            wait(50)

            verify(navigator.focusItem(first))
            verify(first.activeFocus)
            verify(!virtualRevealList.wheelAnimationRunning)
            const revealedContentY = virtualRevealList.contentY
            wait(220)
            compare(virtualRevealList.contentY, revealedContentY)
        }

        function test_horizontal_focus_cancels_pointer_scroll_animation() {
            horizontalRevealList.forceLayout()
            const first = horizontalRevealList.itemAtIndex(0)
            verify(first !== null)
            horizontalRevealList.scrollContentXBy(180)
            verify(horizontalRevealList.wheelAnimationRunning)
            wait(50)

            verify(navigator.focusItem(first))
            verify(first.activeFocus)
            verify(!horizontalRevealList.wheelAnimationRunning)
            const revealedContentX = horizontalRevealList.contentX
            wait(220)
            compare(horizontalRevealList.contentX, revealedContentX)

            const topLeft = first.mapToItem(
                horizontalRevealList.contentItem, 0, 0)
            verify(topLeft.x >= horizontalRevealList.contentX - 0.5)
            verify(topLeft.x + first.width
                   <= horizontalRevealList.contentX
                   + horizontalRevealList.width + 0.5)
        }

        function test_destroyed_bookmark_falls_back_safely() {
            const transient = bookmarkLoader.item
            verify(navigator.focusItem(transient))
            navigator.remember("destroyed", transient)
            bookmarkLoader.active = false
            tryVerify(function() { return bookmarkLoader.item === null })
            verify(navigator.restore("destroyed", scene))
            verify(scene.Window.window.activeFocusItem !== null)
        }
    }
}
