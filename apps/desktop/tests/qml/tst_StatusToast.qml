import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    property int surfaceClickCount: 0

    name: "StatusToast"
    width: 900
    height: 620
    visible: true
    when: windowShown

    Item {
        id: surface
        anchors.fill: parent

        Rectangle {
            anchors.fill: parent
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: "#F1E7D0" }
                GradientStop { position: 0.5; color: "#8FA9B7" }
                GradientStop { position: 1; color: "#14171D" }
            }
        }

        MouseArea {
            anchors.fill: parent
            onClicked: ++testCase.surfaceClickCount
        }

        LoadingOverlay {
            id: bareLoading
            anchors.fill: parent
            showDelay: 0
            minimumVisibleTime: 0
            showPanel: false
            blocksInput: false
        }

        StatusToast {
            id: statusToast
            x: Math.round((surface.width - width) / 2)
            y: globalStatusToast.visible
                ? globalStatusToast.y + globalStatusToast.height + 8
                : Math.max(156, Math.min(200, surface.height * 0.18))
            minimumWidth: Math.min(300, maximumWidth)
            maximumWidth: Math.max(260, Math.min(520, surface.width - 48))
            autoDismiss: false
            dismissible: true
        }

        StatusToast {
            id: globalStatusToast
            x: Math.round((surface.width - width) / 2)
            y: 86
            minimumWidth: Math.min(300, maximumWidth)
            maximumWidth: Math.max(260, Math.min(520, surface.width - 48))
            autoDismiss: false
            dismissible: true
        }
    }

    function init() {
        testCase.width = 900
        testCase.height = 620
        testCase.surfaceClickCount = 0
        bareLoading.active = false
        statusToast.dismiss()
        globalStatusToast.dismiss()
        statusToast.autoDismiss = false
        statusToast.dismissible = true
        globalStatusToast.dismissible = true
        statusToast.timeout = 4200
        tryCompare(statusToast, "opacity", 0)
        tryCompare(globalStatusToast, "opacity", 0)
    }

    function test_loadingCanRenderAsABareCenteredIndicator() {
        bareLoading.active = true
        tryCompare(bareLoading, "shown", true)

        const hud = findChild(bareLoading, "loadingOverlayHud")
        verify(hud !== null)
        compare(hud.width, 32)
        compare(hud.height, 32)
        compare(hud.color.a, 0)
        compare(hud.border.color.a, 0)

        mouseClick(bareLoading, bareLoading.width / 2,
                   bareLoading.height / 2)
        compare(testCase.surfaceClickCount, 1)
    }

    function test_warningToastGeometry_data() {
        return [
            { tag: "minimum", viewportWidth: 900, viewportHeight: 620 },
            { tag: "default", viewportWidth: 1240, viewportHeight: 800 },
            { tag: "full-hd", viewportWidth: 1920, viewportHeight: 1080 },
            { tag: "ultrawide", viewportWidth: 2560, viewportHeight: 1080 }
        ]
    }

    function test_warningToastGeometry(data) {
        testCase.width = data.viewportWidth
        testCase.height = data.viewportHeight
        statusToast.show("连接较慢，恢复后将自动继续播放。", "warning")
        tryCompare(statusToast, "shown", true)

        verify(statusToast.x >= 24)
        verify(statusToast.x + statusToast.width <= surface.width - 24)
        verify(statusToast.y >= 156)
        verify(statusToast.width <= 520)

        const panel = findChild(statusToast, "statusToastPanel")
        const message = findChild(statusToast, "statusToastMessage")
        verify(panel !== null)
        verify(message !== null)
        verify(panel.color.a > 0.9)
        compare(message.color, Theme.text)
        verify(message.lineCount <= 3)
    }

    function test_playerAndGlobalToastDoNotOverlap_data() {
        return test_warningToastGeometry_data()
    }

    function test_playerAndGlobalToastDoNotOverlap(data) {
        testCase.width = data.viewportWidth
        testCase.height = data.viewportHeight
        globalStatusToast.show(
            "The server could not complete this action. Please try again.",
            "error")
        statusToast.show("连接较慢，恢复后将自动继续播放。", "warning")
        tryCompare(globalStatusToast, "shown", true)
        tryCompare(statusToast, "shown", true)

        compare(statusToast.y
                - (globalStatusToast.y + globalStatusToast.height), 8)

        const shortHeight = globalStatusToast.height
        globalStatusToast.show(
            "服务器正在重新连接媒体源；当前播放状态会被保留，连接恢复后会自动继续，请稍候。",
            "warning")
        tryVerify(function() {
            return globalStatusToast.height > shortHeight
        })
        compare(statusToast.y
                - (globalStatusToast.y + globalStatusToast.height), 8)

        tryCompare(globalStatusToast, "opacity", 1)
        globalStatusToast.dismiss()
        compare(globalStatusToast.shown, false)
        compare(globalStatusToast.visible, true)
        compare(statusToast.y
                - (globalStatusToast.y + globalStatusToast.height), 8)
        tryCompare(globalStatusToast, "visible", false)
        compare(statusToast.y,
                Math.max(156, Math.min(200, surface.height * 0.18)))
    }

    function test_nonDismissiblePlayerToastDoesNotConsumeInput() {
        statusToast.dismissible = false
        statusToast.show("连接较慢，恢复后将自动继续播放。", "warning")
        tryCompare(statusToast, "shown", true)

        const closeButton = findChild(statusToast, "statusToastCloseButton")
        verify(closeButton !== null)
        compare(closeButton.visible, false)

        mouseClick(statusToast, statusToast.width / 2,
                   statusToast.height / 2)
        compare(testCase.surfaceClickCount, 1)
        compare(statusToast.shown, true)
    }

    function test_toastBodyDoesNotDismissButCloseButtonDoes() {
        statusToast.show("媒体库已同步到最新状态。", "info")
        tryCompare(statusToast, "shown", true)

        mouseClick(statusToast, statusToast.width / 2,
                   statusToast.height / 2)
        compare(testCase.surfaceClickCount, 1)
        compare(statusToast.shown, true)

        const closeButton = findChild(statusToast, "statusToastCloseButton")
        verify(closeButton !== null)
        compare(closeButton.visible, true)
        compare(closeButton.controlSize, 28)
        compare(closeButton.iconSize, 11)
        compare(closeButton.foregroundColor, Theme.text)
        compare(closeButton.accessibleName, "Close notification")
        tryCompare(closeButton, "width", 28)
        tryCompare(closeButton, "height", 28)
        compare(closeButton.enabled, true)

        const closeHighlight = findChild(
            statusToast, "statusToastCloseHighlight")
        verify(closeHighlight !== null)
        compare(closeHighlight.width, 20)
        compare(closeHighlight.height, 20)
        compare(closeHighlight.color.a, 0)

        mouseMove(closeButton, closeButton.width / 2,
                  closeButton.height / 2)
        tryCompare(closeButton, "hovered", true)
        tryVerify(function() { return closeHighlight.color.a > 0 })
        mouseClick(closeButton, closeButton.width / 2,
                   closeButton.height / 2)
        compare(statusToast.shown, false)
    }

    function test_closeButtonDoesNotCrowdThreeLineMessage() {
        statusToast.show(
            "服务器正在重新连接媒体源；当前播放状态会被保留，连接恢复后会自动继续，请稍候。若问题持续存在，请检查服务器状态。",
            "info")
        tryCompare(statusToast, "shown", true)

        const message = findChild(statusToast, "statusToastMessage")
        const closeButton = findChild(statusToast, "statusToastCloseButton")
        verify(message !== null)
        verify(closeButton !== null)
        tryVerify(function() { return message.lineCount >= 2 })
        verify(message.lineCount <= 3)
        verify(message.x + message.width <= closeButton.x)
        verify(closeButton.x + closeButton.width <= statusToast.width - 8)
        verify(statusToast.height >= closeButton.height + 20)
    }

    function test_infoToneUsesDedicatedBlue() {
        statusToast.show("普通状态", "info")
        compare(statusToast.toneColor, Theme.info)
        verify(String(statusToast.toneColor) !== String(Theme.danger))
    }

    function test_unknownToneFallsBackToInfoRatherThanError() {
        statusToast.show("普通状态", "unexpected-tone")
        compare(statusToast.toneColor, Theme.info)
    }

    function test_missingToneReturnsToTheDefaultSemanticColor() {
        statusToast.show("操作成功", "success")
        compare(statusToast.tone, "success")
        statusToast.show("普通状态")
        compare(statusToast.tone, "info")
    }

    function test_toastRetractsWithAFade() {
        statusToast.show("连接较慢，恢复后将自动继续播放。", "warning")
        tryCompare(statusToast, "opacity", 1)
        statusToast.dismiss()
        compare(statusToast.shown, false)
        tryCompare(statusToast, "opacity", 0)
        compare(statusToast.visible, false)
    }

    function test_autoDismissUsesTheSameExitAnimation() {
        statusToast.autoDismiss = true
        statusToast.show("操作失败", "error", 40)
        tryCompare(statusToast, "shown", true)
        tryCompare(statusToast, "shown", false)
        tryCompare(statusToast, "opacity", 0)
    }
}
