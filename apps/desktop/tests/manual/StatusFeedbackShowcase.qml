import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Yanami.Ui

ApplicationWindow {
    id: root

    property bool loadingActive: false
    property bool tourActive: false
    property int tourStep: 0
    property string pendingToastTarget: ""
    property string pendingToastMessage: ""
    property string pendingToastTone: "info"
    property int pendingToastDuration: 3200
    property string currentState: "待命"
    property string currentDetail: "选择一个场景，观察真实组件的出现与收回。"
    property string currentTone: "info"
    readonly property color currentToneColor: currentTone === "warning"
        ? "#FFB86B"
        : (currentTone === "success" ? Theme.success
            : (currentTone === "error" ? Theme.danger : Theme.info))

    visible: true
    width: 1280
    height: 800
    minimumWidth: 1040
    minimumHeight: 680
    title: "Yanami · 状态反馈陈列室"
    color: Theme.background

    function stopPendingWork() {
        stallPromptTimer.stop()
        manualShowTimer.stop()
        tourTimer.stop()
        tourActive = false
    }

    function clearVisuals() {
        loadingActive = false
        globalToast.dismiss()
        playerToast.dismiss()
    }

    function resetShowcase() {
        stopPendingWork()
        clearVisuals()
        currentState = "待命"
        currentDetail = "选择一个场景，观察真实组件的出现与收回。"
        currentTone = "info"
    }

    function beginManualState(state, detail, tone) {
        stopPendingWork()
        clearVisuals()
        currentState = state
        currentDetail = detail
        currentTone = tone
    }

    function queueToast(target, message, tone, durationMs) {
        pendingToastTarget = target
        pendingToastMessage = message
        pendingToastTone = tone
        pendingToastDuration = durationMs
        manualShowTimer.restart()
    }

    function showInfo() {
        beginManualState("信息 Toast", "主界面的普通状态提示，自动淡出收回。", "info")
        queueToast("global", "媒体库已同步到最新状态。", "info", 3200)
    }

    function showSuccess() {
        beginManualState("成功 Toast", "成功操作使用绿色语义，不再误显示为错误。", "success")
        queueToast("global", "已加入播放列表。", "success", 3200)
    }

    function showWarning() {
        beginManualState("警告 Toast", "播放器内的可恢复状态位于中上区域。", "warning")
        queueToast("player", "连接较慢，恢复后将自动继续播放。", "warning", 5200)
    }

    function showError() {
        beginManualState("错误 Toast", "普通播放错误留在画面内，不再弹出阻塞式窗口。", "error")
        queueToast("player", "媒体读取失败，请检查服务器存储状态。", "error", 5200)
    }

    function showLoadingOnly() {
        beginManualState("仅 Loading", "中央只显示 28 px 指示器，不使用方形底板。", "info")
        loadingActive = true
    }

    function simulateStall() {
        beginManualState("模拟网络卡顿", "先出现中央 Loading，随后给出可恢复警告。", "warning")
        loadingActive = true
        stallPromptTimer.restart()
    }

    function simulateRecovery() {
        stopPendingWork()
        loadingActive = false
        playerToast.dismiss()
        globalToast.dismiss()
        currentState = "模拟恢复"
        currentDetail = "恢复事件立即收回陈旧警告，画面继续播放。"
        currentTone = "success"
    }

    function showConcurrentStates() {
        beginManualState("并发避让", "主界面与播放器 Toast 按实际高度保持 8 px 间距。", "warning")
        loadingActive = true
        queueToast("concurrent", "", "warning", 4800)
    }

    function startTour() {
        stopPendingWork()
        clearVisuals()
        tourActive = true
        tourStep = 0
        advanceTour()
    }

    function scheduleTour(delayMs) {
        tourTimer.interval = delayMs
        tourTimer.restart()
    }

    function advanceTour() {
        if (!tourActive)
            return

        if (tourStep === 0) {
            currentState = "自动演示 · 信息"
            currentDetail = "先看主界面的普通状态提示。"
            currentTone = "info"
            globalToast.show("媒体库已同步到最新状态。", "info", 3600)
            scheduleTour(1600)
        } else if (tourStep === 1) {
            globalToast.dismiss()
            scheduleTour(300)
        } else if (tourStep === 2) {
            globalToast.show("已加入播放列表。", "success", 3600)
            currentState = "自动演示 · 成功"
            currentDetail = "状态颜色平滑切换为绿色。"
            currentTone = "success"
            scheduleTour(1600)
        } else if (tourStep === 3) {
            globalToast.dismiss()
            scheduleTour(300)
        } else if (tourStep === 4) {
            loadingActive = true
            currentState = "自动演示 · Loading"
            currentDetail = "短暂等待 180 ms 后显示中央裸指示器。"
            currentTone = "info"
            scheduleTour(900)
        } else if (tourStep === 5) {
            playerToast.show("连接较慢，恢复后将自动继续播放。", "warning", 5200)
            currentState = "自动演示 · 卡顿"
            currentDetail = "Loading 与警告分层呈现，不遮挡主要画面。"
            currentTone = "warning"
            scheduleTour(2100)
        } else if (tourStep === 6) {
            loadingActive = false
            playerToast.dismiss()
            currentState = "自动演示 · 恢复"
            currentDetail = "恢复事件提前触发警告收回动画。"
            currentTone = "success"
            scheduleTour(600)
        } else if (tourStep === 7) {
            playerToast.show("媒体读取失败，请检查服务器存储状态。", "error", 5200)
            currentState = "自动演示 · 错误"
            currentDetail = "错误使用红色 Toast，不跳出旧 Modal。"
            currentTone = "error"
            scheduleTour(2100)
        } else if (tourStep === 8) {
            playerToast.dismiss()
            scheduleTour(400)
        } else if (tourStep === 9) {
            globalToast.show("这条信息将按真实时长自动收回。", "info", 2200)
            currentState = "自动演示 · 自动收回"
            currentDetail = "没有交互也会执行同一套退出动画。"
            currentTone = "info"
            scheduleTour(2700)
        } else if (tourStep === 10) {
            loadingActive = true
            globalToast.show("主界面：已加入播放列表。", "success", 4200)
            playerToast.show("播放器：连接较慢，正在等待媒体数据。", "warning", 4200)
            currentState = "自动演示 · 并发"
            currentDetail = "两层 Toast 同时出现但不会重叠。"
            currentTone = "warning"
            scheduleTour(3000)
        } else if (tourStep === 11) {
            loadingActive = false
            playerToast.dismiss()
            globalToast.dismiss()
            scheduleTour(300)
        } else {
            globalToast.show("演示完成，可继续点击任意场景。", "info", 3200)
            currentState = "演示完成"
            currentDetail = "所有效果都来自本轮实际交付组件。"
            currentTone = "info"
            tourActive = false
            tourTimer.stop()
            return
        }
        ++tourStep
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.background

        gradient: Gradient {
            GradientStop { position: 0; color: "#0D1017" }
            GradientStop { position: 1; color: "#07090D" }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 18

        GlassPanel {
            Layout.preferredWidth: 318
            Layout.fillHeight: true
            radius: 28
            color: "#D0151820"
            border.color: "#22FFFFFF"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: badgeLabel.implicitWidth + 20
                    Layout.preferredHeight: 28
                    radius: 14
                    color: Theme.accentSoft
                    border.color: "#45FF6687"

                    Text {
                        id: badgeLabel
                        anchors.centerIn: parent
                        text: "DEV · INTERACTIVE"
                        color: Theme.accent
                        font.family: Theme.fontFamily
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.7
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: "状态反馈陈列室"
                    color: Theme.text
                    font.family: Theme.cjkFontFamily
                    font.pixelSize: 27
                    font.weight: Font.DemiBold
                }

                Text {
                    Layout.fillWidth: true
                    text: "点击即可重复查看本轮 Loading 与 Toast 的真实动效。"
                    color: Theme.textMuted
                    font.family: Theme.cjkFontFamily
                    font.pixelSize: 13
                    lineHeight: 1.25
                    wrapMode: Text.Wrap
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    height: 1
                    color: Theme.outline
                }

                Text {
                    text: "TOAST 语义"
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 10

                    AppButton {
                        Layout.fillWidth: true
                        text: "信息"
                        controlSize: 40
                        onClicked: root.showInfo()
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: "成功"
                        controlSize: 40
                        onClicked: root.showSuccess()
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: "警告"
                        controlSize: 40
                        onClicked: root.showWarning()
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: "错误"
                        kind: "danger"
                        controlSize: 40
                        onClicked: root.showError()
                    }
                }

                Text {
                    Layout.topMargin: 4
                    text: "播放器状态"
                    color: Theme.textMuted
                    font.family: Theme.cjkFontFamily
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.8
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 10
                    rowSpacing: 10

                    AppButton {
                        Layout.fillWidth: true
                        text: "仅 Loading"
                        controlSize: 40
                        onClicked: root.showLoadingOnly()
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: "模拟卡顿"
                        controlSize: 40
                        onClicked: root.simulateStall()
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: "模拟恢复"
                        controlSize: 40
                        onClicked: root.simulateRecovery()
                    }
                    AppButton {
                        Layout.fillWidth: true
                        text: "并发避让"
                        controlSize: 40
                        onClicked: root.showConcurrentStates()
                    }
                }

                AppButton {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    text: root.tourActive ? "演示进行中…" : "播放全部动效"
                    kind: "primary"
                    controlSize: 44
                    enabled: !root.tourActive
                    onClicked: root.startTour()
                }

                AppButton {
                    Layout.fillWidth: true
                    text: "清空画面"
                    kind: "ghost"
                    controlSize: 40
                    onClicked: root.resetShowcase()
                }

                Item { Layout.fillHeight: true }

                GlassPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 82
                    radius: 18
                    color: "#7410131A"
                    border.color: "#20FFFFFF"

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 16
                        spacing: 12

                        Rectangle {
                            Layout.preferredWidth: 9
                            Layout.preferredHeight: 9
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 6
                            radius: 4.5
                            color: root.currentToneColor

                            Behavior on color {
                                ColorAnimation { duration: 180 }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3

                            Text {
                                Layout.fillWidth: true
                                text: root.currentState
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.currentDetail
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                                lineHeight: 1.2
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }
        }

        GlassPanel {
            id: preview
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.minimumWidth: 650
            radius: 28
            clip: true
            color: "#11151C"
            border.color: "#2AFFFFFF"

            Rectangle {
                anchors.fill: parent
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "#172E3D" }
                    GradientStop { position: 0.45; color: "#B8B39F" }
                    GradientStop { position: 0.72; color: "#665F68" }
                    GradientStop { position: 1; color: "#12151B" }
                }
            }

            Rectangle {
                width: preview.width * 0.52
                height: preview.height * 0.34
                x: -preview.width * 0.08
                y: preview.height * 0.30
                radius: height / 2
                rotation: -8
                color: "#2C8DB3B9"
            }

            Rectangle {
                width: preview.width * 0.42
                height: preview.height * 0.56
                x: preview.width * 0.41
                y: preview.height * 0.19
                radius: 88
                rotation: 11
                color: "#29F3E5C8"
            }

            Rectangle {
                width: preview.width * 0.22
                height: preview.height * 0.70
                anchors.right: parent.right
                anchors.rightMargin: -36
                anchors.verticalCenter: parent.verticalCenter
                radius: 70
                color: "#53101720"
            }

            Rectangle {
                z: 4
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.margins: 22
                width: stageLabel.implicitWidth + 22
                height: 30
                radius: 15
                color: "#9911161E"
                border.color: "#28FFFFFF"

                Text {
                    id: stageLabel
                    anchors.centerIn: parent
                    text: "实时播放器预览"
                    color: Theme.text
                    font.family: Theme.cjkFontFamily
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                }
            }

            Text {
                z: 3
                x: 54
                y: 66
                text: "Loading 只保留中央图标"
                color: "white"
                style: Text.Outline
                styleColor: "#B0000000"
                font.family: Theme.cjkFontFamily
                font.pixelSize: 21
                font.weight: Font.DemiBold
            }

            Text {
                z: 3
                anchors.right: parent.right
                anchors.rightMargin: 52
                y: 250
                text: "Toast 右侧可关闭，也没有多余阴影"
                color: "#FFE36C"
                style: Text.Outline
                styleColor: "#B0000000"
                font.family: Theme.cjkFontFamily
                font.pixelSize: 19
                font.weight: Font.DemiBold
            }

            Text {
                z: 3
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: progressTrack.top
                anchors.bottomMargin: 28
                text: "这是字幕安全区，用来检查提示不会互相遮挡"
                color: "white"
                style: Text.Outline
                styleColor: "#E0000000"
                font.family: Theme.cjkFontFamily
                font.pixelSize: 23
                font.weight: Font.DemiBold
            }

            Text {
                z: 3
                anchors.left: parent.left
                anchors.leftMargin: 26
                anchors.bottom: progressTrack.top
                anchors.bottomMargin: 7
                text: "18:42"
                color: "#E9FFFFFF"
                font.family: Theme.fontFamily
                font.pixelSize: 11
            }

            Rectangle {
                id: progressTrack
                z: 3
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.leftMargin: 26
                anchors.rightMargin: 26
                anchors.bottomMargin: 24
                height: 3
                radius: 1.5
                color: "#45FFFFFF"

                Rectangle {
                    width: parent.width * 0.46
                    height: parent.height
                    radius: parent.radius
                    color: Theme.accent
                }
            }

            LoadingOverlay {
                anchors.fill: parent
                z: 10
                active: root.loadingActive
                blocksInput: false
                showPanel: false
                indicatorOutlineColor: "#8A000000"
                indicatorOutlineWidth: 0.45
            }

            StatusToast {
                id: globalToast
                z: 20
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: 86
                minimumWidth: Math.min(300, maximumWidth)
                maximumWidth: Math.max(260, Math.min(520, parent.width - 48))
                defaultTone: "info"
                timeout: 3200
            }

            StatusToast {
                id: playerToast
                z: 20
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: globalToast.visible
                    ? globalToast.y + globalToast.height + 8
                    : Math.max(156, Math.min(200, parent.height * 0.18))
                minimumWidth: Math.min(300, maximumWidth)
                maximumWidth: Math.max(260, Math.min(520, parent.width - 48))
                autoDismiss: true
                dismissible: true
                timeout: 5200

                Behavior on anchors.topMargin {
                    enabled: !globalToast.visible
                    NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
                }
            }
        }
    }

    Timer {
        id: stallPromptTimer
        interval: 700
        onTriggered: {
            if (root.loadingActive)
                playerToast.show(
                    "连接较慢，恢复后将自动继续播放。", "warning", 5200)
        }
    }

    Timer {
        id: manualShowTimer
        interval: 180
        onTriggered: {
            if (root.pendingToastTarget === "global") {
                globalToast.show(root.pendingToastMessage,
                                 root.pendingToastTone,
                                 root.pendingToastDuration)
            } else if (root.pendingToastTarget === "player") {
                playerToast.show(root.pendingToastMessage,
                                 root.pendingToastTone,
                                 root.pendingToastDuration)
            } else if (root.pendingToastTarget === "concurrent") {
                globalToast.show("主界面：已加入播放列表。",
                                 "success", root.pendingToastDuration)
                playerToast.show("播放器：连接较慢，正在等待媒体数据。",
                                 "warning", root.pendingToastDuration)
            }
            root.pendingToastTarget = ""
        }
    }

    Timer {
        id: tourTimer
        onTriggered: root.advanceTour()
    }
}
