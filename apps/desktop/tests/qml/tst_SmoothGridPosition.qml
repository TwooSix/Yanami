import QtQuick
import QtQml.Models
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    name: "SmoothGridPosition"
    width: 640
    height: 480
    visible: true
    when: windowShown

    property var activeSourceModel: firstModel

    ListModel { id: firstModel }
    ListModel { id: secondModel }

    DelegateModel {
        id: stableAdapter
        model: testCase.activeSourceModel
        delegate: Rectangle {
            required property int index
            required property string stableId
            width: 96
            height: 96
        }
    }

    SmoothGridView {
        id: grid
        width: 400
        height: 260
        cellWidth: 100
        cellHeight: 100
        model: stableAdapter
    }

    function populate(model, prefix) {
        for (let index = 0; index < 80; ++index) {
            model.append({
                "stableId": prefix + String(index),
                "label": String(index)
            })
        }
    }

    function init() {
        firstModel.clear()
        secondModel.clear()
        populate(firstModel, "first-")
        populate(secondModel, "second-")
        testCase.activeSourceModel = firstModel
        grid.visible = true
        grid.resetScrollPosition()
        wait(0)
    }

    function test_newContextResetClearsStaleCurrentItem() {
        grid.currentIndex = 60
        grid.positionViewAtIndex(60, GridView.Beginning)
        wait(0)
        verify(grid.contentY > grid.originY)

        grid.visible = false
        testCase.activeSourceModel = secondModel
        grid.visible = true
        grid.forceLayout()
        tryVerify(function() {
            const visibleIndex = grid.indexAt(
                grid.contentX + 1, grid.contentY + 1)
            const visibleItem = grid.itemAtIndex(visibleIndex)
            return visibleItem !== null
                && visibleItem.stableId.indexOf("second-") === 0
        })
        verify(grid.contentY > grid.originY)

        grid.resetScrollPosition()
        tryCompare(grid, "currentIndex", -1)
        tryVerify(function() { return grid.atYBeginning })
        verify(Math.abs(grid.contentY - grid.originY) < 0.5)
        compare(grid.indexAt(grid.contentX + 1, grid.contentY + 1), 0)
        verify(Math.abs(grid.visibleArea.yPosition) < 0.001)
    }

    function test_detailReturnPreservesExistingPosition() {
        grid.currentIndex = 40
        grid.positionViewAtIndex(40, GridView.Beginning)
        wait(0)
        verify(grid.contentY > grid.originY)
        const previousIndex = grid.indexAt(grid.contentX + 1, grid.contentY + 1)
        const previousItem = grid.itemAtIndex(previousIndex)
        verify(previousItem !== null)
        const previousId = previousItem.stableId
        const previousY = grid.contentY

        grid.visible = false
        wait(0)
        grid.visible = true
        wait(0)

        const returnedIndex = grid.indexAt(grid.contentX + 1, grid.contentY + 1)
        const returnedItem = grid.itemAtIndex(returnedIndex)
        verify(returnedItem !== null)
        compare(returnedItem.stableId, previousId)
        compare(grid.currentIndex, 40)
        verify(Math.abs(grid.contentY - previousY) < 0.5)
        verify(!grid.atYBeginning)
    }

    function test_existingContextUpdatePreservesVisibleAnchor() {
        grid.currentIndex = 40
        grid.positionViewAtIndex(40, GridView.Beginning)
        wait(0)
        const previousIndex = grid.indexAt(grid.contentX + 1, grid.contentY + 1)
        const previousItem = grid.itemAtIndex(previousIndex)
        verify(previousItem !== null)
        const previousId = previousItem.stableId

        grid.visible = false
        for (let index = 0; index < 4; ++index) {
            firstModel.insert(0, {
                "stableId": "refresh-" + String(index),
                "label": "refresh"
            })
        }
        grid.forceLayout()
        grid.visible = true
        grid.forceLayout()
        wait(0)

        const returnedIndex = grid.indexAt(grid.contentX + 1, grid.contentY + 1)
        const returnedItem = grid.itemAtIndex(returnedIndex)
        verify(returnedItem !== null)
        compare(returnedItem.stableId, previousId)
        verify(!grid.atYBeginning)
    }
}
