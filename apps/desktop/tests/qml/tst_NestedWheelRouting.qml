import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    name: "NestedWheelRouting"
    width: 640
    height: 420
    visible: true
    when: windowShown

    ListModel { id: railModel }

    SmoothFlickable {
        id: page
        x: 20
        y: 20
        width: 460
        height: 260
        contentHeight: pageContent.height

        Item {
            id: pageContent
            width: page.width
            height: 760

            SmoothHorizontalList {
                id: rail
                x: 24
                y: 96
                width: 340
                height: 104
                spacing: 12
                passVerticalWheelToParent: true
                model: railModel
                delegate: Rectangle {
                    required property int index
                    width: 96
                    height: 88
                    objectName: "rail-item-" + String(index)
                }
            }
        }
    }

    SignalSpy {
        id: userScrollSpy
        target: rail
        signalName: "userScrollStarted"
    }

    function initTestCase() {
        for (let index = 0; index < 12; ++index)
            railModel.append({ "label": String(index) })
    }

    function init() {
        rail.passVerticalWheelToParent = true
        page.scrollToContentY(0)
        rail.resetScrollPosition()
        wait(240)
        userScrollSpy.clear()
    }

    function wheelDownOverRail(modifiers) {
        mouseWheel(rail, rail.width / 2, rail.height / 2,
                   0, -120, Qt.NoButton, modifiers || Qt.NoModifier)
    }

    function verifyVerticalWheelMovesOnlyPage() {
        const originalX = rail.contentX
        wheelDownOverRail(Qt.NoModifier)
        tryVerify(function() { return page.contentY > 1 })
        verify(Math.abs(rail.contentX - originalX) < 0.5)
        compare(userScrollSpy.count, 0)
    }

    function test_verticalWheelAtHorizontalBeginningMovesPage() {
        verify(rail.atXBeginning)
        verifyVerticalWheelMovesOnlyPage()
    }

    function test_verticalWheelAtHorizontalMiddleMovesPage() {
        rail.positionViewAtIndex(5, ListView.Beginning)
        wait(0)
        verify(!rail.atXBeginning)
        verify(!rail.atXEnd)
        verifyVerticalWheelMovesOnlyPage()
    }

    function test_verticalWheelAtHorizontalEndMovesPage() {
        rail.positionViewAtEnd()
        wait(0)
        verify(rail.atXEnd)
        verifyVerticalWheelMovesOnlyPage()
    }

    function test_horizontalWheelMovesOnlyRail() {
        mouseWheel(rail, rail.width / 2, rail.height / 2,
                   -120, 0, Qt.NoButton, Qt.NoModifier)

        tryVerify(function() { return rail.contentX > rail.originX + 1 })
        verify(Math.abs(page.contentY) < 0.5)
        tryVerify(function() { return userScrollSpy.count > 0 })
    }

    function test_shiftVerticalWheelMovesOnlyRail() {
        wheelDownOverRail(Qt.ShiftModifier)

        tryVerify(function() { return rail.contentX > rail.originX + 1 })
        verify(Math.abs(page.contentY) < 0.5)
        compare(userScrollSpy.count, 1)
    }

    function test_legacyModeKeepsVerticalToHorizontalMapping() {
        rail.passVerticalWheelToParent = false
        wheelDownOverRail(Qt.NoModifier)

        tryVerify(function() { return rail.contentX > rail.originX + 1 })
        verify(Math.abs(page.contentY) < 0.5)
        compare(userScrollSpy.count, 1)
    }
}
