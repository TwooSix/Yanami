import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami

ApplicationWindow {
    id: window
    width: 1240
    height: 800
    minimumWidth: 900
    minimumHeight: 620
    visible: true
    title: "Yanami"
    color: Theme.background
    flags: Qt.Window | Qt.FramelessWindowHint

    readonly property bool fullScreenMode: windowShell.fullScreen
    readonly property bool roundedFrame: !fullScreenMode
        && visibility !== Window.Maximized

    property int currentPage: 0
    property int playerReturnPage: 0
    property double developmentSeekSeconds: 0
    property int developmentAutoStopMs: 0
    property bool developmentLoadingPreview: false
    property bool developmentAutoSkipIntro: false
    property bool developmentRenderDiagnostics: false
    function enterFullScreen() {
        windowShell.enterFullScreen(window)
    }

    function exitFullScreen() {
        windowShell.exitFullScreen()
    }

    function toggleFullScreen() {
        windowShell.toggleFullScreen(window)
    }

    function closePlayer() {
        playerPage.closePlayback()
        windowShell.exitFullScreen()
        window.currentPage = window.playerReturnPage
    }

    function showBackendError() {
        if (backend && backend.statusIsError && backend.statusMessage.length > 0)
            errorDialog.show(backend.statusMessage)
    }

    function navigateBack() {
        if (window.currentPage === 2) {
            if (window.fullScreenMode)
                window.exitFullScreen()
            else
                window.closePlayer()
        } else if (window.currentPage === 0) {
            if (homePage.depth > 0)
                homePage.goBack()
        } else {
            window.currentPage = 0
        }
    }

    function refreshPlaybackHistory() {
        if (!backend.embyConnected)
            return
        backend.refreshActivity()
    }

    Rectangle {
        id: appFrame
        anchors.fill: parent
        color: Theme.background
        radius: window.roundedFrame ? Theme.radius : 0
        border.width: window.roundedFrame ? 1 : 0
        border.color: Theme.outline
        antialiasing: window.roundedFrame
        clip: true

        Rectangle {
            width: 440
            height: 440
            radius: 220
            x: -180
            y: -190
            color: "#204E294B"
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: window.currentPage === 2 ? 0 : 18
            anchors.rightMargin: window.currentPage === 2 ? 0 : 18
            anchors.bottomMargin: window.currentPage === 2 ? 0 : 18
            anchors.topMargin: window.currentPage === 2 ? 0 : 52
            spacing: window.currentPage === 2 ? 0 : 18

            GlassPanel {
                visible: window.currentPage !== 2
                Layout.fillHeight: true
                Layout.preferredWidth: visible ? 76 : 0

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    BrandMark {
                        Layout.alignment: Qt.AlignHCenter
                        width: 46
                        height: 46
                    }
                    Item { Layout.preferredHeight: 16 }
                    NavButton {
                        iconName: "home"
                        selected: window.currentPage === 0
                        onClicked: {
                            homePage.goHome()
                            window.currentPage = 0
                        }
                    }
                    NavButton {
                        iconName: "search"
                        selected: window.currentPage === 1
                        onClicked: {
                            window.currentPage = 1
                            searchPage.focusSearch()
                        }
                    }
                    Item { Layout.fillHeight: true }
                    NavButton { iconName: "settings"; selected: window.currentPage === 3; onClicked: window.currentPage = 3 }
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: window.currentPage

                HomePage {
                    id: homePage
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    mediaItems: backend.mediaItems
                    libraryViews: backend.libraryViews
                    resumeItems: backend.resumeItems
                    recentItems: backend.recentItems
                    collectionItems: backend.collectionItems
                    collectionParent: backend.collectionParent
                    onPlayRequested: (itemId, title) => backend.preparePlayback(itemId)
                    onSettingsRequested: window.currentPage = 3
                }
                SearchPage {
                    id: searchPage
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    mediaItems: backend.mediaItems
                    onItemRequested: item => {
                        window.currentPage = 0
                        homePage.openSearchItem(item)
                    }
                    onPlayRequested: (itemId, title) => backend.preparePlayback(itemId)
                }
                PlayerPage {
                    id: playerPage
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    developmentSeekSeconds: window.developmentSeekSeconds
                    developmentAutoSkipIntro: window.developmentAutoSkipIntro
                    developmentRenderDiagnostics: window.developmentRenderDiagnostics
                    loadingPreview: window.developmentLoadingPreview
                    windowFullScreen: window.fullScreenMode
                    onPlaybackLoaded: {
                        if (window.developmentAutoStopMs > 0)
                            developmentStopTimer.restart()
                    }
                    onCloseRequested: window.closePlayer()
                    onToggleFullScreenRequested: window.toggleFullScreen()
                    onErrorOccurred: message => errorDialog.show(message)
                    onEpisodeSwitchRequested: (itemId, positionSeconds, paused) => {
                        backend.switchPlayback(itemId, positionSeconds, paused)
                    }
                }
                SettingsPage {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                }
            }
        }

        LoadingOverlay {
            anchors.fill: parent
            z: 100
            active: window.currentPage !== 2
                && (backend.blockingBusy || window.developmentLoadingPreview)
        }

        WindowTitleBar {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            z: 200
            targetWindow: window
            playerMode: window.currentPage === 2
            fullScreenMode: window.fullScreenMode
        }

        WindowResizeHandles {
            anchors.fill: parent
            z: 300
            targetWindow: window
            enabled: !window.fullScreenMode
        }

        AppErrorDialog {
            id: errorDialog
            z: 500
        }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        enabled: !(window.currentPage === 2 && playerPage.popupOpened)
        onActivated: window.navigateBack()
    }

    Shortcut {
        sequence: "F11"
        context: Qt.ApplicationShortcut
        enabled: window.currentPage === 2
        onActivated: window.toggleFullScreen()
    }

    Connections {
        target: backend
        function onPlaybackReady(mediaUrl, headers, resumeTicks, title, previousItem,
                                 nextItem, externalSubtitles, introStartTicks,
                                 introEndTicks, danmakuFile) {
            playerPage.mediaTitle = title
            playerPage.previousItem = previousItem
            playerPage.nextItem = nextItem
            playerPage.resumeTicks = resumeTicks
            playerPage.introStartSeconds = introStartTicks >= 0 ? introStartTicks / 10000000 : -1
            playerPage.introEndSeconds = introEndTicks >= 0 ? introEndTicks / 10000000 : -1
            playerPage.danmakuFile = danmakuFile
            playerPage.externalSubtitles = externalSubtitles
            playerPage.requestHeaders = headers
            playerPage.mediaUrl = mediaUrl
            if (window.currentPage !== 2)
                window.playerReturnPage = window.currentPage
            window.currentPage = 2
        }
        function onPlaybackStoppedReported() {
            playbackActivityRefreshTimer.restart()
        }
        function onStatusMessageChanged() {
            window.showBackendError()
        }
    }

    Timer {
        id: playbackActivityRefreshTimer
        interval: 800
        onTriggered: window.refreshPlaybackHistory()
    }

    Timer {
        id: developmentStopTimer
        interval: Math.max(250, window.developmentAutoStopMs)
        onTriggered: playerPage.closeRequested()
    }

    onClosing: {
        if (window.currentPage === 2)
            playerPage.closePlayback()
    }

    Component.onCompleted: Qt.callLater(window.showBackendError)
}
