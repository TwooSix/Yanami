import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami
import Yanami.Native

Item {
    id: root
    property url mediaUrl
    property var requestHeaders: ({})
    property url danmakuFile
    property var externalSubtitles: []
    property string mediaTitle
    property var previousItem: ({})
    property var nextItem: ({})
    property double resumeTicks: 0
    property double introStartSeconds: -1
    property double introEndSeconds: -1
    property double developmentSeekSeconds: 0
    property bool developmentAutoSkipIntro: false
    property bool loadingPreview: false
    property bool windowFullScreen: false
    property bool reportingActive: false
    property bool introOfferArmed: false
    property bool introOfferDismissed: false
    property bool switchingEpisode: false
    property bool developmentRenderDiagnostics: false
    property double initialPlaybackTargetSeconds: 0
    readonly property bool chromeVisible: controls.opacity > 0.05
        || topChrome.opacity > 0.05
    readonly property bool popupOpened: subtitleMenu.opened
        || audioMenu.opened || qualityMenu.opened || volumeControl.opened
    readonly property bool chromeInteractionActive: controlsHover.hovered
        || topChromeHover.hovered || skipIntroButton.hovered || popupOpened
    readonly property bool playbackShortcutsEnabled: root.visible
        && root.reportingActive
        && !subtitleMenu.opened
        && !audioMenu.opened
        && !qualityMenu.opened
        && !volumeControl.opened
    readonly property bool canSkipIntro: introOfferArmed
        && !introOfferDismissed
        && introStartSeconds >= 0
        && introEndSeconds > introStartSeconds
        && player.position >= Math.max(0, introStartSeconds - 2)
        && player.position < introEndSeconds
    signal closeRequested()
    signal toggleFullScreenRequested()
    signal errorOccurred(string message)
    signal playbackLoaded()
    signal episodeSwitchRequested(string itemId, double positionSeconds, bool paused)

    Rectangle { anchors.fill: parent; color: "black" }

    MpvVideoItem {
        id: player
        anchors.fill: parent
        onPlaybackError: message => {
            errorLabel.text = message
            root.errorOccurred(message)
        }
        onFileLoaded: {
            root.introOfferArmed = false
            root.initialPlaybackTargetSeconds = root.developmentSeekSeconds > 0
                ? root.developmentSeekSeconds
                : Math.max(0, root.resumeTicks / 10000000)
            root.introOfferDismissed = root.introEndSeconds > 0
                && root.initialPlaybackTargetSeconds >= root.introEndSeconds
            if (root.initialPlaybackTargetSeconds > 0)
                seek(root.initialPlaybackTargetSeconds)
            for (let index = 0; index < root.externalSubtitles.length; ++index) {
                const subtitle = root.externalSubtitles[index]
                addSubtitle(subtitle.url, subtitle.title, subtitle.selected)
            }
            if (root.danmakuFile.toString().length > 0)
                setDanmakuFile(root.danmakuFile)
            root.reportingActive = true
            backend.reportPlayback("started",
                Math.max(position, root.resumeTicks / 10000000,
                    root.developmentSeekSeconds), paused)
            playbackReportTimer.restart()
            root.playbackLoaded()
            introActivationTimer.restart()
        }
        onFileEnded: {
            if (root.reportingActive) {
                backend.reportPlayback("stopped", position, paused)
                root.reportingActive = false
                playbackReportTimer.stop()
            }
        }
        onPausedChanged: {
            if (root.reportingActive)
                backend.reportPlayback("progress", position, paused)
        }
    }

    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onPositionChanged: revealChrome()
        onClicked: {
            if (root.chromeVisible)
                root.hideChrome()
            else
                root.revealChrome()
            root.forceActiveFocus()
        }
        onDoubleClicked: player.paused = !player.paused
    }

    Rectangle {
        id: topShade
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 150
        z: 1
        opacity: topChrome.opacity
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0; color: "#C9000000" }
            GradientStop { position: 1; color: "#00000000" }
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 240
        z: 1
        opacity: controls.opacity
        gradient: Gradient {
            orientation: Gradient.Vertical
            GradientStop { position: 0; color: "#00000000" }
            GradientStop { position: 1; color: "#D9000000" }
        }
    }

    Item {
        id: topChrome
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: 96
        z: 3
        visible: opacity > 0
        enabled: opacity > 0.05

        HoverHandler {
            id: topChromeHover
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        }

        AppButton {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 28
            kind: "secondary"
            iconOnly: true
            iconName: "back"
            controlSize: 46
            onClicked: root.closeRequested()
        }

        Text {
            anchors.centerIn: parent
            width: Math.min(parent.width * 0.56, 680)
            text: root.mediaTitle
            color: Theme.text
            font.family: Theme.fontForText(root.mediaTitle)
            font.pixelSize: 16
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
        }

        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    Text {
        id: errorLabel
        anchors.centerIn: parent
        color: Theme.danger
        font.family: Theme.fontForText(text)
        font.pixelSize: 14
        z: 3
    }

    LoadingOverlay {
        anchors.fill: parent
        z: 2
        active: root.loadingPreview || root.switchingEpisode
            || player.playbackState === "loading"
    }

    AppButton {
        id: skipIntroButton
        z: 4
        anchors.right: controls.right
        anchors.bottom: controls.top
        anchors.bottomMargin: 16
        kind: "glass"
        text: qsTr("Skip Intro")
        controlSize: 44
        enabled: root.canSkipIntro
        visible: opacity > 0
        opacity: root.canSkipIntro ? 1 : 0
        onClicked: root.skipIntro()

        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    GlassPanel {
        id: controls
        z: 3
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 28
        width: Math.min(parent.width - 56, 1120)
        height: 120
        radius: 26
        color: "#E213151C"
        border.color: "#32FFFFFF"
        visible: opacity > 0
        enabled: opacity > 0.05

        HoverHandler {
            id: controlsHover
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.leftMargin: 24
            anchors.rightMargin: 24
            anchors.topMargin: 17
            anchors.bottomMargin: 16
            spacing: 10

            AppSlider {
                Layout.fillWidth: true
                from: 0
                to: Math.max(1, player.duration)
                value: player.position
                bufferedValue: Math.max(player.position, player.bufferedPosition)
                Accessible.name: qsTr("Playback position")
                onMoved: player.seek(value)
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 9

                AppButton {
                    kind: "ghost"
                    iconOnly: true
                    iconName: "previous-track"
                    iconSize: 18
                    controlSize: 38
                    enabled: !root.switchingEpisode
                        && String(root.previousItem.id || "").length > 0
                    Accessible.name: qsTr("Previous episode")
                    ToolTip.visible: hovered
                    ToolTip.text: root.previousItem.title
                        ? qsTr("Previous episode") + " · " + root.previousItem.title
                        : qsTr("Previous episode")
                    ToolTip.delay: 500
                    onClicked: root.playAdjacent(root.previousItem)
                }
                MediaPlayButton {
                    paused: player.paused
                    onClicked: player.paused = !player.paused
                }
                AppButton {
                    kind: "ghost"
                    iconOnly: true
                    iconName: "next-track"
                    iconSize: 18
                    controlSize: 38
                    enabled: !root.switchingEpisode
                        && String(root.nextItem.id || "").length > 0
                    Accessible.name: qsTr("Next episode")
                    ToolTip.visible: hovered
                    ToolTip.text: root.nextItem.title
                        ? qsTr("Next episode") + " · " + root.nextItem.title
                        : qsTr("Next episode")
                    ToolTip.delay: 500
                    onClicked: root.playAdjacent(root.nextItem)
                }
                AppButton {
                    id: danmakuButton
                    kind: "ghost"
                    text: "−10"
                    controlSize: 38
                    onClicked: player.seek(Math.max(0, player.position - 10))
                }
                AppButton {
                    kind: "ghost"
                    text: "+10"
                    controlSize: 38
                    onClicked: player.seek(Math.min(player.duration, player.position + 10))
                }
                Text {
                    Layout.leftMargin: 4
                    text: formatTime(player.position) + "  /  " + formatTime(player.duration)
                    color: Theme.textMuted
                    font.family: Theme.fontFamily
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }
                Item { Layout.fillWidth: true }
                AppButton {
                    kind: "ghost"
                    text: qsTr("Danmaku")
                    controlSize: 38
                    checkable: true
                    checked: true
                    onToggled: player.setDanmakuVisible(checked)
                }
                AppButton {
                    id: subtitleButton
                    kind: "ghost"
                    text: player.subtitleTracks.length > 0
                        ? qsTr("Subtitles · %1").arg(player.subtitleTracks.length)
                        : qsTr("Subtitles")
                    controlSize: 38
                    checkable: true
                    checked: subtitleMenu.opened
                    onClicked: {
                        root.revealChrome()
                        subtitleMenu.opened ? subtitleMenu.close() : subtitleMenu.open()
                    }
                }
                AppButton {
                    id: audioButton
                    kind: "ghost"
                    text: player.audioTracks.length > 0
                        ? qsTr("Audio · %1").arg(player.audioTracks.length)
                        : qsTr("Audio")
                    controlSize: 38
                    checkable: true
                    checked: audioMenu.opened
                    onClicked: {
                        root.revealChrome()
                        audioMenu.opened ? audioMenu.close() : audioMenu.open()
                    }
                }
                AppButton {
                    id: qualityButton
                    kind: "ghost"
                    text: qsTr("Original")
                    controlSize: 38
                    checkable: true
                    checked: qualityMenu.opened
                    onClicked: {
                        root.revealChrome()
                        qualityMenu.opened ? qualityMenu.close() : qualityMenu.open()
                    }
                }
                VolumeControl {
                    id: volumeControl
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    volume: player.volume
                    onVolumeRequested: value => player.setVolume(value)
                    onOpenedChanged: {
                        if (opened)
                            hideTimer.stop()
                        else
                            root.revealChrome()
                    }
                }
                AppButton {
                    id: fullScreenButton
                    kind: "ghost"
                    iconOnly: true
                    iconName: root.windowFullScreen ? "fullscreen-exit" : "fullscreen-enter"
                    iconSize: 18
                    controlSize: 38
                    Accessible.name: root.windowFullScreen
                        ? qsTr("Exit fullscreen")
                        : qsTr("Enter fullscreen")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    ToolTip.delay: 500
                    onClicked: {
                        root.revealChrome()
                        root.toggleFullScreenRequested()
                    }
                }
            }
        }

        TrackMenu {
            id: subtitleMenu
            parent: subtitleButton
            x: subtitleButton.width - width
            y: -height - 14
            heading: qsTr("Subtitle tracks")
            tracks: player.subtitleTracks
            selectedId: player.selectedSubtitleTrack
            allowOff: true
            onOpened: hideTimer.stop()
            onClosed: root.revealChrome()
            onTrackSelected: trackId => {
                if (trackId < 0)
                    player.disableSubtitles()
                else
                    player.selectSubtitleTrack(trackId)
                close()
            }
        }

        TrackMenu {
            id: audioMenu
            parent: audioButton
            x: audioButton.width - width
            y: -height - 14
            heading: qsTr("Audio tracks")
            tracks: player.audioTracks
            selectedId: player.selectedAudioTrack
            onOpened: hideTimer.stop()
            onClosed: root.revealChrome()
            onTrackSelected: trackId => {
                player.selectAudioTrack(trackId)
                close()
            }
        }

        TrackMenu {
            id: qualityMenu
            parent: qualityButton
            x: qualityButton.width - width
            y: -height - 14
            heading: qsTr("Playback quality")
            tracks: [{
                "id": 0,
                "label": qsTr("Original · Automatically select the best direct source"),
                "selected": true
            }]
            selectedId: 0
            onOpened: hideTimer.stop()
            onClosed: root.revealChrome()
            onTrackSelected: trackId => close()
        }

        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    Timer {
        id: introActivationTimer
        interval: 700
        onTriggered: {
            if (!root.reportingActive || root.mediaUrl.toString().length === 0)
                return
            if (player.playbackState === "loading") {
                restart()
                return
            }
            root.introOfferArmed = !root.introOfferDismissed
            if (root.developmentAutoSkipIntro && root.canSkipIntro)
                developmentSkipIntroTimer.restart()
        }
    }

    Timer {
        id: introOfferTimeout
        interval: 8000
        onTriggered: root.introOfferDismissed = true
    }

    Timer {
        id: developmentSkipIntroTimer
        interval: 400
        onTriggered: root.skipIntro()
    }

    Timer {
        id: hideTimer
        interval: 3200
        onTriggered: {
            if (root.chromeInteractionActive) {
                if (root.developmentRenderDiagnostics)
                    console.info("player-chrome hide-suppressed interaction-active")
                stop()
                return
            }
            root.hideChrome()
        }
    }

    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: root.playbackShortcutsEnabled
        onActivated: root.togglePlayback()
    }

    Shortcut {
        sequence: "Left"
        context: Qt.ApplicationShortcut
        enabled: root.playbackShortcutsEnabled
        onActivated: root.seekBy(-5)
    }

    Shortcut {
        sequence: "Right"
        context: Qt.ApplicationShortcut
        enabled: root.playbackShortcutsEnabled
        onActivated: root.seekBy(5)
    }

    Shortcut {
        sequence: "Up"
        context: Qt.ApplicationShortcut
        enabled: root.playbackShortcutsEnabled
        onActivated: root.adjustVolume(5)
    }

    Shortcut {
        sequence: "Down"
        context: Qt.ApplicationShortcut
        enabled: root.playbackShortcutsEnabled
        onActivated: root.adjustVolume(-5)
    }

    Timer {
        id: playbackReportTimer
        interval: 10000
        repeat: true
        onTriggered: {
            if (root.reportingActive)
                backend.reportPlayback("progress", player.position, player.paused)
        }
    }

    onMediaUrlChanged: {
        if (mediaUrl.toString().length > 0) {
            switchingEpisode = false
            introOfferArmed = false
            introOfferDismissed = false
            introActivationTimer.stop()
            introOfferTimeout.stop()
            errorLabel.text = ""
            player.open(mediaUrl, requestHeaders)
        }
    }

    onCanSkipIntroChanged: {
        if (canSkipIntro)
            introOfferTimeout.restart()
        else
            introOfferTimeout.stop()
    }

    onVisibleChanged: {
        if (!visible)
            closePopups()
    }

    onChromeInteractionActiveChanged: {
        if (developmentRenderDiagnostics)
            console.info("player-chrome interaction-active=" + chromeInteractionActive)
        if (chromeInteractionActive) {
            hideTimer.stop()
            controls.opacity = 1
            topChrome.opacity = 1
        } else if (chromeVisible) {
            hideTimer.restart()
        }
    }

    function revealChrome() {
        controls.opacity = 1
        topChrome.opacity = 1
        if (chromeInteractionActive)
            hideTimer.stop()
        else
            hideTimer.restart()
    }

    function closePopups() {
        subtitleMenu.close()
        audioMenu.close()
        qualityMenu.close()
        volumeControl.closePopup()
    }

    function hideChrome() {
        hideTimer.stop()
        controls.opacity = 0
        topChrome.opacity = 0
    }

    function togglePlayback() {
        player.paused = !player.paused
        revealChrome()
    }

    function seekBy(seconds) {
        player.seek(Math.max(0, Math.min(player.duration, player.position + seconds)))
        revealChrome()
    }

    function adjustVolume(amount) {
        player.setVolume(Math.max(0, Math.min(100, player.volume + amount)))
        revealChrome()
    }

    function closePlayback() {
        if (reportingActive)
            backend.reportPlayback("stopped", player.position, player.paused)
        reportingActive = false
        playbackReportTimer.stop()
        introActivationTimer.stop()
        introOfferTimeout.stop()
        developmentSkipIntroTimer.stop()
        player.stop()
        mediaUrl = ""
        requestHeaders = ({})
        danmakuFile = ""
        externalSubtitles = []
        resumeTicks = 0
        introStartSeconds = -1
        introEndSeconds = -1
        introOfferArmed = false
        introOfferDismissed = false
        initialPlaybackTargetSeconds = 0
        previousItem = ({})
        nextItem = ({})
        switchingEpisode = false
    }

    function playAdjacent(item) {
        const itemId = String(item && item.id ? item.id : "")
        if (itemId.length === 0 || switchingEpisode)
            return
        const lastPosition = player.position
        const wasPaused = player.paused
        reportingActive = false
        playbackReportTimer.stop()
        introActivationTimer.stop()
        introOfferTimeout.stop()
        switchingEpisode = true
        player.stop()
        mediaUrl = ""
        episodeSwitchRequested(itemId, lastPosition, wasPaused)
    }

    function skipIntro() {
        if (!canSkipIntro)
            return
        player.seek(introEndSeconds)
        if (reportingActive)
            backend.reportPlayback("progress", introEndSeconds, player.paused)
        revealChrome()
    }

    function formatTime(seconds) {
        const value = Math.max(0, Math.floor(seconds || 0))
        const hours = Math.floor(value / 3600)
        const minutes = Math.floor((value % 3600) / 60)
        const secs = value % 60
        return (hours > 0 ? hours + ":" : "") + String(minutes).padStart(2, "0") + ":" + String(secs).padStart(2, "0")
    }
}
