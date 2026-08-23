import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui
import Yanami.Native

Item {
    id: root
    property url mediaUrl
    property var requestHeaders: ({})
    property string currentItemId
    property string danmakuSearchAnime
    property string danmakuSearchEpisode
    property bool danmakuEnabled: true
    property var danmakuComments: []
    property var externalSubtitles: []
    property string reportSessionId
    property var embyTracks: []
    property string mediaTitle
    property var previousItem: ({})
    property var nextItem: ({})
    property var playbackContext: ({})
    property var playbackQueue: []
    property int currentQueueIndex: -1
    property bool queueResolutionSucceeded: false
    readonly property var previousQueueItem: queueEntryAt(currentQueueIndex - 1)
    readonly property var nextQueueItem: queueEntryAt(currentQueueIndex + 1)
    property double resumeTicks: 0
    property double introStartSeconds: -1
    property double introEndSeconds: -1
    property double developmentSeekSeconds: 0
    property bool developmentAutoSkipIntro: false
    property bool loadingPreview: false
    property bool windowFullScreen: false
    property bool playbackActive: false
    property bool introOfferArmed: false
    property bool introOfferDismissed: false
    property bool switchingEpisode: false
    property bool preparingPlayback: false
    property bool playbackEndVisible: false
    property bool playbackEndRetryMode: false
    property string playbackEndMessage: ""
    property string playbackEndRetryAction: ""
    property bool automaticAdvancePending: false
    property bool automaticAdvanceOpening: false
    property bool automaticQueueRefreshPending: false
    property bool naturalCompletionHandled: false
    property var pendingAutomaticQueueItem: ({})
    property int pendingAutomaticQueueIndex: -1
    property bool developmentRenderDiagnostics: false
    property bool developmentDanmakuPreview: false
    property bool developmentPlaybackQueuePreview: false
    property string developmentDanmakuSearchQuery: ""
    property double developmentDanmakuPreviewFontSize: 0
    property bool developmentDisableDanmakuAfterLoad: false
    property bool developmentReenableDanmakuAfterDisable: false
    property bool developmentSyntheticDanmaku: false
    property int developmentDanmakuStyleStressCount: 1
    property int developmentDanmakuToggleStressCount: 0
    property bool developmentDanmakuStyleTriggered: false
    property double initialPlaybackTargetSeconds: 0
    readonly property bool chromeVisible: controls.opacity > 0.05
        || topChrome.opacity > 0.05
    readonly property bool popupOpened: subtitleMenu.opened
        || audioMenu.opened || qualityMenu.opened || danmakuMenu.opened
        || queueMenu.opened || volumeControl.opened
    readonly property bool chromeInteractionActive: controlsHover.hovered
        || topChromeHover.hovered || skipIntroButton.hovered || popupOpened
    readonly property bool playbackShortcutsEnabled: root.visible
        && root.playbackActive
        && !PopupCoordinator.blocksApplicationShortcuts
        && !subtitleMenu.opened
        && !audioMenu.opened
        && !qualityMenu.opened
        && !danmakuMenu.opened
        && !queueMenu.opened
        && !volumeControl.keyboardInteractionActive
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
    signal episodeSwitchRequested(string itemId, var context,
                                  double positionSeconds, bool paused)
    signal replayRequested(string itemId, var context, string title)
    signal queueRefreshRequested(string itemId, var context)

    onPlaybackQueueChanged: {
        if (developmentPlaybackQueuePreview && playbackQueue
                && playbackQueue.length > 0)
            developmentQueuePreviewTimer.restart()
    }

    Rectangle { anchors.fill: parent; color: "black" }

    PlaybackAdvancePolicy { id: playbackAdvancePolicy }

    MpvVideoItem {
        id: player
        anchors.fill: parent
        Component.onCompleted: app.playback.attachPlayer(player)
        onPlaybackError: message => {
            // closePlayback() clears identity before the Loader is destroyed;
            // discard any libmpv event already queued for that closed load.
            if (root.mediaUrl.toString().length === 0
                    || root.currentItemId.length === 0)
                return
            if (root.automaticAdvanceOpening) {
                root.automaticAdvanceOpening = false
                root.switchingEpisode = false
                root.showPlaybackEndState(true, message)
                return
            }
            errorLabel.text = message
            root.errorOccurred(message)
        }
        onFileLoaded: {
            if (root.mediaUrl.toString().length === 0
                    || root.currentItemId.length === 0) {
                player.stop()
                return
            }
            const automaticOpenCompleted = root.automaticAdvanceOpening
            root.automaticAdvanceOpening = false
            if (automaticOpenCompleted) {
                // keep-open and any stale runtime pause must never strand the
                // automatically selected entry on its first frame.
                player.paused = false
                root.automaticAdvancePending = false
                root.pendingAutomaticQueueItem = ({})
                root.pendingAutomaticQueueIndex = -1
            }
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
            root.playbackActive = true
            if (!root.developmentSyntheticDanmaku)
                app.playback.beginSession(root.reportSessionId, root.embyTracks)
            root.playbackLoaded()
            introActivationTimer.restart()
            if (root.developmentSyntheticDanmaku) {
                const syntheticComments = []
                for (let index = 0; index < 180; ++index) {
                    syntheticComments.push({
                        "id": "dev-" + index,
                        "time": (index % 60) * 0.45,
                        "mode": index % 17 === 0 ? "top"
                              : index % 23 === 0 ? "bottom" : "scroll",
                        "color": index % 5 === 0 ? 0xff7aa2 : 0xffffff,
                        "text": "Danmaku overlay " + (index + 1)
                    })
                }
                root.danmakuComments = syntheticComments
                if (root.developmentDanmakuToggleStressCount > 0) {
                    developmentDanmakuToggleStress.remaining =
                        root.developmentDanmakuToggleStressCount * 2
                    developmentDanmakuToggleStress.restart()
                }
                if (root.developmentDanmakuPreviewFontSize > 0
                        && !root.developmentDanmakuStyleTriggered) {
                    root.developmentDanmakuStyleTriggered = true
                    developmentDanmakuStylePreview.restart()
                }
            } else if (root.developmentDanmakuSearchQuery.length > 0) {
                Qt.callLater(function() {
                    danmakuMenu.open()
                    app.danmaku.search(root.currentItemId,
                                       root.developmentDanmakuSearchQuery)
                })
            } else {
                root.requestAutomaticDanmaku()
            }
            if (root.developmentDanmakuPreview)
                Qt.callLater(danmakuMenu.open)
        }
        onFileEnded: {
            root.playbackActive = false
        }
        onPlaybackCompleted: root.handlePlaybackCompleted()
    }

    DanmakuOverlay {
        id: danmakuOverlay
        anchors.fill: parent
        z: 0.5
        comments: root.danmakuComments
        danmakuEnabled: root.danmakuEnabled
        mediaPosition: player.position
        paused: player.paused
        buffering: player.playbackState === MpvVideoItem.Loading
            || player.playbackState === MpvVideoItem.Buffering
        fontSize: danmakuMenu.fontSize
        commentOpacity: danmakuMenu.opacityValue
        scrollDuration: danmakuMenu.scrollDuration
        displayArea: danmakuMenu.displayArea
        density: danmakuMenu.density
        timeOffset: danmakuMenu.timeOffset
        blockedTerms: danmakuMenu.blockedTerms
        showScroll: danmakuMenu.showScroll
        showTop: danmakuMenu.showTop
        showBottom: danmakuMenu.showBottom
        topMargin: danmakuMenu.danmakuTopMargin
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
        active: root.loadingPreview || root.preparingPlayback
            || root.switchingEpisode || root.automaticAdvancePending
            || player.playbackState === MpvVideoItem.Loading
            || player.playbackState === MpvVideoItem.Buffering
    }

    PlaybackEndOverlay {
        id: playbackEndOverlay
        z: 20
        anchors.fill: parent
        shown: root.playbackEndVisible
        retryMode: root.playbackEndRetryMode
        heading: root.playbackEndHeading()
        detail: root.playbackEndDetail()
        onDoneRequested: root.closeRequested()
        onReplayRequested: root.replayCurrentItem()
        onRetryRequested: root.retryAutomaticAdvance()
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
                        && String((root.previousQueueItem || {}).id || "").length > 0
                    Accessible.name: root.previousActionLabel()
                    toolTipText: root.previousQueueItem.title
                        ? Accessible.name + " · " + root.previousQueueItem.title
                        : Accessible.name
                    onClicked: root.playQueueEntry(root.previousQueueItem,
                                                   root.currentQueueIndex - 1)
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
                        && String((root.nextQueueItem || {}).id || "").length > 0
                    Accessible.name: root.nextActionLabel()
                    toolTipText: root.nextQueueItem.title
                        ? Accessible.name + " · " + root.nextQueueItem.title
                        : Accessible.name
                    onClicked: root.playQueueEntry(root.nextQueueItem,
                                                   root.currentQueueIndex + 1)
                }
                AppButton {
                    id: seekBackwardButton
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
                    id: danmakuButton
                    kind: "ghost"
                    text: qsTr("Danmaku")
                    controlSize: 38
                    checkable: true
                    checked: root.danmakuEnabled
                    function finishToggleInteraction() {
                        danmakuHoverOpen.stop()
                        danmakuHoverClose.stop()
                        danmakuMenu.close()
                        root.revealChrome()
                    }
                    function triggerFromOverlayClick() {
                        const popup = PopupCoordinator.topPopup()
                        if (popup && popup !== danmakuMenu)
                            popup.requestDismiss("overlay-action")
                        root.danmakuEnabled = !root.danmakuEnabled
                        finishToggleInteraction()
                    }
                    onToggled: root.danmakuEnabled = checked
                    onClicked: finishToggleInteraction()
                    onHoveredChanged: {
                        if (hovered) {
                            danmakuHoverClose.stop()
                            root.revealChrome()
                            if (!danmakuMenu.opened)
                                danmakuHoverOpen.restart()
                        } else {
                            danmakuHoverOpen.stop()
                            danmakuHoverClose.restart()
                        }
                    }
                    Component.onCompleted:
                        PopupCoordinator.registerOverlayClickTarget(danmakuButton)
                    Component.onDestruction:
                        PopupCoordinator.unregisterOverlayClickTarget(danmakuButton)
                }
                AppPopupButton {
                    id: subtitleButton
                    kind: "ghost"
                    text: player.subtitleTracks.length > 0
                        ? qsTr("Subtitles · %1").arg(player.subtitleTracks.length)
                        : qsTr("Subtitles")
                    controlSize: 38
                    popupTarget: subtitleMenu
                    peerPopups: [audioMenu, qualityMenu, queueMenu, danmakuMenu]
                    onPopupActivated: {
                        root.revealChrome()
                        volumeControl.closePopup()
                    }
                }
                AppPopupButton {
                    id: audioButton
                    kind: "ghost"
                    text: player.audioTracks.length > 0
                        ? qsTr("Audio · %1").arg(player.audioTracks.length)
                        : qsTr("Audio")
                    controlSize: 38
                    popupTarget: audioMenu
                    peerPopups: [subtitleMenu, qualityMenu, queueMenu, danmakuMenu]
                    onPopupActivated: {
                        root.revealChrome()
                        volumeControl.closePopup()
                    }
                }
                AppPopupButton {
                    id: qualityButton
                    kind: "ghost"
                    text: qsTr("Original")
                    controlSize: 38
                    popupTarget: qualityMenu
                    peerPopups: [subtitleMenu, audioMenu, queueMenu, danmakuMenu]
                    onPopupActivated: {
                        root.revealChrome()
                        volumeControl.closePopup()
                    }
                }
                AppPopupButton {
                    id: queueButton
                    kind: "ghost"
                    iconOnly: true
                    iconName: "queue"
                    iconSize: 18
                    controlSize: 38
                    enabled: root.playbackQueue && root.playbackQueue.length > 0
                    popupTarget: queueMenu
                    peerPopups: [subtitleMenu, audioMenu, qualityMenu, danmakuMenu]
                    Accessible.name: qsTr("Play queue")
                    toolTipVisible: hovered && !queueMenu.opened
                    toolTipText: root.currentQueueIndex >= 0
                        ? qsTr("Play queue · %1 of %2")
                            .arg(root.currentQueueIndex + 1)
                            .arg(root.playbackQueue.length)
                        : Accessible.name
                    onPopupActivated: {
                        root.revealChrome()
                        volumeControl.closePopup()
                    }
                }
                VolumeControl {
                    id: volumeControl
                    Layout.preferredWidth: 38
                    Layout.preferredHeight: 38
                    volume: player.volume
                    onVolumeRequested: value => player.setVolume(value)
                    onOpenedChanged: {
                        if (opened) {
                            hideTimer.stop()
                        } else {
                            root.revealChrome()
                        }
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
                    toolTipText: Accessible.name
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

        PlaybackQueueMenu {
            id: queueMenu
            parent: queueButton
            x: queueButton.width - width
            y: -height - 14
            width: Math.min(404, Math.max(300, root.width - 48))
            maximumHeight: Math.min(520,
                Math.max(210, root.height - controls.height - 72))
            playbackContext: root.playbackContext
            queueItems: root.playbackQueue
            currentIndex: root.currentQueueIndex
            switching: root.switchingEpisode
            paused: player.paused
            onInteractionStarted: hideTimer.stop()
            onInteractionEnded: root.revealChrome()
            onItemRequested: (item, queueIndex) => {
                close()
                root.playQueueEntry(item, queueIndex)
            }
        }

        Timer {
            id: developmentQueuePreviewTimer
            interval: 500
            repeat: false
            onTriggered: {
                root.revealChrome()
                queueMenu.open()
            }
        }

        DanmakuMenu {
            id: danmakuMenu
            parent: danmakuButton
            x: danmakuButton.width - width
            y: -height - 14
            height: Math.min(570, Math.max(320, root.height - 112))
            animeSuggestion: root.danmakuSearchAnime
            episodeSuggestion: root.danmakuSearchEpisode
            working: app.danmaku.working
            onOpened: {
                subtitleMenu.close()
                audioMenu.close()
                qualityMenu.close()
                queueMenu.close()
                volumeControl.closePopup()
                danmakuHoverClose.stop()
                hideTimer.stop()
            }
            onClosed: root.revealChrome()
            onPointerInsideChanged: {
                if (pointerInside)
                    danmakuHoverClose.stop()
                else if (!danmakuButton.hovered)
                    danmakuHoverClose.restart()
            }
            onSearchRequested: anime => {
                if (root.currentItemId.length > 0)
                    app.danmaku.search(root.currentItemId, anime)
            }
            onMatchRequested: (match, style) => {
                if (root.currentItemId.length > 0)
                    app.danmaku.applyMatch(root.currentItemId, match, style)
            }
            onStyleRequested: style => {
                // Style properties are bound directly to DanmakuOverlay. No
                // ASS file generation or libmpv subtitle command is involved.
            }
        }

        Behavior on opacity { NumberAnimation { duration: 180 } }
    }

    Timer {
        id: danmakuHoverOpen
        interval: 260
        onTriggered: {
            if (danmakuButton.hovered && !danmakuMenu.opened)
                danmakuMenu.open()
        }
    }

    Timer {
        id: danmakuHoverClose
        interval: 360
        onTriggered: {
            if (!danmakuButton.hovered && !danmakuMenu.pointerInside)
                danmakuMenu.close()
        }
    }

    Timer {
        id: developmentDanmakuStylePreview
        interval: 300
        onTriggered: {
            developmentDanmakuStyleStress.remaining =
                Math.max(1, root.developmentDanmakuStyleStressCount)
            developmentDanmakuStyleStress.restart()
        }
    }

    Timer {
        id: developmentDanmakuStyleStress
        property int remaining: 0
        interval: 25
        repeat: true
        onTriggered: {
            if (remaining <= 0) {
                stop()
                if (root.developmentDisableDanmakuAfterLoad)
                    developmentDanmakuDisable.restart()
                return
            }
            const baseSize = root.developmentDanmakuPreviewFontSize > 0
                ? root.developmentDanmakuPreviewFontSize : 42
            danmakuMenu.previewDevelopmentStyle(
                Math.max(18, Math.min(72, baseSize + (remaining % 5) * 2 - 4)))
            --remaining
            if (remaining <= 0 && root.developmentDisableDanmakuAfterLoad) {
                stop()
                root.danmakuEnabled = false
                danmakuMenu.close()
                if (root.developmentReenableDanmakuAfterDisable)
                    developmentDanmakuReenable.restart()
            }
        }
    }

    Timer {
        id: developmentDanmakuDisable
        interval: root.developmentDanmakuPreviewFontSize > 0 ? 80 : 900
        onTriggered: {
            root.danmakuEnabled = false
            danmakuMenu.close()
            if (root.developmentReenableDanmakuAfterDisable)
                developmentDanmakuReenable.restart()
        }
    }

    Timer {
        id: developmentDanmakuReenable
        interval: 350
        onTriggered: root.danmakuEnabled = true
    }

    Timer {
        id: developmentDanmakuToggleStress
        property int remaining: 0
        interval: 45
        repeat: true
        onTriggered: {
            if (remaining <= 0) {
                stop()
                root.danmakuEnabled = true
                return
            }
            root.danmakuEnabled = !root.danmakuEnabled
            --remaining
        }
    }

    Timer {
        id: introActivationTimer
        interval: 700
        onTriggered: {
            if (!root.playbackActive || root.mediaUrl.toString().length === 0)
                return
            if (player.playbackState === MpvVideoItem.Loading
                    || player.playbackState === MpvVideoItem.Buffering) {
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

    onMediaUrlChanged: {
        developmentDanmakuStylePreview.stop()
        developmentDanmakuStyleTriggered = false
        if (mediaUrl.toString().length > 0) {
            const openingAutomatically = automaticAdvancePending
            resetPlaybackEndState(openingAutomatically)
            automaticAdvanceOpening = openingAutomatically
            switchingEpisode = false
            introOfferArmed = false
            introOfferDismissed = false
            introActivationTimer.stop()
            introOfferTimeout.stop()
            errorLabel.text = ""
            player.open(mediaUrl, requestHeaders)
        }
    }

    onDanmakuEnabledChanged: {
        // The overlay is independent from libmpv. Toggling it must never
        // mutate subtitle tracks or interrupt the video's demux/cache state.
        if (developmentRenderDiagnostics)
            console.info("danmaku-overlay visibility=" + danmakuEnabled
                         + " comments=" + danmakuComments.length)
    }

    onCanSkipIntroChanged: {
        if (canSkipIntro)
            introOfferTimeout.restart()
        else
            introOfferTimeout.stop()
    }

    onVisibleChanged: {
        if (!visible)
            PopupCoordinator.closeScope(root, true)
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

    function requestAutomaticDanmaku() {
        if (!app.danmaku.configured || currentItemId.length === 0)
            return
        danmakuMenu.loadStatus = "matching"
        danmakuMenu.animeMatches = []
        danmakuMenu.selectedAnime = ({})
        // Matching/fetching returns one immutable structured timeline. Visual
        // style lives entirely in the overlay and is never sent through ASS.
        app.danmaku.loadAutomatically(currentItemId)
    }

    function handleDanmakuResult(result) {
        if (!result)
            return
        danmakuMenu.loadStatus = String(result.status || "idle")
        if (result.animeSuggestion || result.episodeSuggestion)
            danmakuMenu.setSuggestions(result.animeSuggestion || danmakuSearchAnime,
                                        result.episodeSuggestion || danmakuSearchEpisode)
        if (result.matches)
            danmakuMenu.matches = result.matches
        if (result.animes) {
            danmakuMenu.matches = []
            danmakuMenu.showAnimeResults(result.animes)
            if (root.developmentDanmakuSearchQuery.length > 0
                    && result.animes.length > 0)
                danmakuMenu.chooseAnime(result.animes[0])
        }
        if (result.status === "loaded" && result.comments) {
            danmakuComments = result.comments
            if (developmentRenderDiagnostics)
                console.info("danmaku-overlay timeline-loaded comments="
                             + result.comments.length)
            danmakuMenu.loadedTitle = String(result.title || "")
            danmakuMenu.commentCount = Number(result.commentCount || 0)
            danmakuMenu.matches = []
            danmakuMenu.animeMatches = []
            danmakuMenu.selectedAnime = ({})
            if (root.developmentDisableDanmakuAfterLoad
                    && root.developmentDanmakuPreviewFontSize <= 0)
                developmentDanmakuDisable.restart()
            if (root.developmentDanmakuPreviewFontSize > 0
                    && !root.developmentDanmakuStyleTriggered) {
                root.developmentDanmakuStyleTriggered = true
                developmentDanmakuStylePreview.restart()
            }
            if (root.developmentDanmakuToggleStressCount > 0
                    && !developmentDanmakuToggleStress.running) {
                developmentDanmakuToggleStress.remaining =
                    root.developmentDanmakuToggleStressCount * 2
                developmentDanmakuToggleStress.restart()
            }
        }
        if ((result.status === "choice-required" || result.status === "no-match")
                && !danmakuMenu.opened)
            danmakuMenu.open()
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
        if (playbackActive && !developmentSyntheticDanmaku)
            app.playback.stopSession()
        playbackActive = false
        introActivationTimer.stop()
        introOfferTimeout.stop()
        developmentSkipIntroTimer.stop()
        player.stop()
        mediaUrl = ""
        requestHeaders = ({})
        danmakuComments = []
        currentItemId = ""
        danmakuSearchAnime = ""
        danmakuSearchEpisode = ""
        danmakuMenu.loadedTitle = ""
        danmakuMenu.commentCount = 0
        danmakuMenu.matches = []
        danmakuMenu.loadStatus = "idle"
        externalSubtitles = []
        reportSessionId = ""
        embyTracks = []
        resumeTicks = 0
        introStartSeconds = -1
        introEndSeconds = -1
        introOfferArmed = false
        introOfferDismissed = false
        initialPlaybackTargetSeconds = 0
        previousItem = ({})
        nextItem = ({})
        playbackContext = ({})
        playbackQueue = []
        currentQueueIndex = -1
        queueResolutionSucceeded = false
        switchingEpisode = false
        preparingPlayback = false
        automaticAdvanceOpening = false
        resetPlaybackEndState()
        errorLabel.text = ""
    }

    function beginPreparation(itemId, title) {
        closePlayback()
        currentItemId = String(itemId || "")
        mediaTitle = String(title || "").trim()
        if (mediaTitle.length === 0)
            mediaTitle = qsTr("Preparing playback")
        preparingPlayback = true
        errorLabel.text = ""
        revealChrome()
    }

    function failPreparation(message) {
        if (!preparingPlayback)
            return
        preparingPlayback = false
        errorLabel.text = String(message || qsTr("Playback could not be prepared."))
        revealChrome()
    }

    function resetPlaybackEndState(preserveAutomaticAdvance) {
        const preserveAutomatic = preserveAutomaticAdvance === true
        playbackEndVisible = false
        playbackEndRetryMode = false
        playbackEndMessage = ""
        playbackEndRetryAction = ""
        automaticQueueRefreshPending = false
        naturalCompletionHandled = false
        if (!preserveAutomatic) {
            automaticAdvancePending = false
            pendingAutomaticQueueItem = ({})
            pendingAutomaticQueueIndex = -1
        }
    }

    function playbackEndHeading() {
        if (playbackEndRetryMode
                && playbackEndRetryAction === "resolve-next")
            return qsTr("Could not check for another item")
        if (playbackEndRetryMode)
            return qsTr("The next item could not be played")
        const kind = String((playbackContext || {}).kind || "").toLowerCase()
        if (kind === "series")
            return qsTr("You’ve reached the final episode")
        if (kind === "playlist")
            return qsTr("The play queue has ended")
        return qsTr("Playback complete")
    }

    function playbackEndDetail() {
        if (playbackEndRetryMode)
            return playbackEndMessage
        return String(mediaTitle || "").trim()
    }

    function showPlaybackEndState(retryMode, message, retryAction) {
        playbackActive = false
        preparingPlayback = false
        automaticAdvancePending = false
        automaticAdvanceOpening = false
        automaticQueueRefreshPending = false
        playbackEndRetryMode = retryMode === true
        playbackEndMessage = String(message || "")
        playbackEndRetryAction = playbackEndRetryMode
            ? String(retryAction || "open-next") : ""
        if (!playbackEndRetryMode
                || playbackEndRetryAction !== "open-next") {
            pendingAutomaticQueueItem = ({})
            pendingAutomaticQueueIndex = -1
        }
        errorLabel.text = ""
        introActivationTimer.stop()
        introOfferTimeout.stop()
        PopupCoordinator.closeScope(root, true)
        hideChrome()
        playbackEndVisible = true
    }

    function startAutomaticAdvance(decision) {
        const item = (decision || {}).item || ({})
        const itemId = String(item.id || "")
        if (itemId.length === 0)
            return false
        const queueIndex = Number((decision || {}).queueIndex)
        pendingAutomaticQueueItem = item
        pendingAutomaticQueueIndex = Number.isInteger(queueIndex)
            ? queueIndex : -1
        automaticAdvancePending = true
        playbackEndVisible = false
        playbackEndRetryMode = false
        playbackEndMessage = ""
        playbackEndRetryAction = ""
        PopupCoordinator.closeScope(root, true)
        hideChrome()
        const completedItemId = currentItemId
        Qt.callLater(function() {
            if (!root.automaticAdvancePending
                    || root.currentItemId !== completedItemId)
                return
            if (!root.playQueueEntry(item,
                                     root.pendingAutomaticQueueIndex, true)) {
                root.showPlaybackEndState(
                    true, qsTr("The next item could not be played."),
                    "open-next")
            }
        })
        return true
    }

    function requestAutomaticQueueRefresh() {
        const itemId = String(currentItemId || "")
        if (itemId.length === 0 || automaticQueueRefreshPending)
            return false
        automaticQueueRefreshPending = true
        automaticAdvancePending = true
        preparingPlayback = true
        playbackEndVisible = false
        playbackEndRetryMode = false
        playbackEndMessage = ""
        playbackEndRetryAction = ""
        PopupCoordinator.closeScope(root, true)
        hideChrome()
        const requestedContext = playbackContext || ({})
        Qt.callLater(function() {
            if (root.automaticQueueRefreshPending
                    && root.currentItemId === itemId)
                root.queueRefreshRequested(itemId, requestedContext)
        })
        return true
    }

    function consumeQueueRefresh(descriptor) {
        if (!automaticQueueRefreshPending)
            return false
        automaticQueueRefreshPending = false
        automaticAdvancePending = false
        preparingPlayback = false
        previousItem = descriptor.previousItem || ({})
        nextItem = descriptor.nextItem || ({})
        playbackContext = descriptor.playbackContext || playbackContext || ({})
        playbackQueue = descriptor.playbackQueue || []
        currentQueueIndex = Number.isInteger(descriptor.currentQueueIndex)
            ? descriptor.currentQueueIndex : -1
        queueResolutionSucceeded = descriptor.queueResolutionSucceeded === true
        const decision = playbackAdvancePolicy.decide(
            playbackQueue, currentQueueIndex, nextItem,
            queueResolutionSucceeded)
        if (decision.action === "open-next")
            startAutomaticAdvance(decision)
        else if (decision.action === "complete")
            showPlaybackEndState(false, "", "")
        else
            showPlaybackEndState(
                true,
                qsTr("Try again to check whether another item is available."),
                "resolve-next")
        return true
    }

    function recoverQueueRefreshFailure(message) {
        if (!automaticQueueRefreshPending)
            return
        queueResolutionSucceeded = false
        showPlaybackEndState(
            true,
            String(message
                   || qsTr("Try again to check whether another item is available.")),
            "resolve-next")
    }

    function handlePlaybackCompleted() {
        if (mediaUrl.toString().length === 0 || currentItemId.length === 0
                || naturalCompletionHandled || switchingEpisode
                || preparingPlayback)
            return
        naturalCompletionHandled = true
        playbackActive = false
        introActivationTimer.stop()
        introOfferTimeout.stop()
        const decision = playbackAdvancePolicy.decide(
            playbackQueue, currentQueueIndex, nextItem,
            queueResolutionSucceeded)
        if (decision.action === "open-next")
            startAutomaticAdvance(decision)
        else if (decision.action === "refresh-queue")
            requestAutomaticQueueRefresh()
        else
            showPlaybackEndState(false, "", "")
    }

    function retryAutomaticAdvance() {
        if (playbackEndRetryAction === "resolve-next") {
            requestAutomaticQueueRefresh()
            return
        }
        const item = pendingAutomaticQueueItem || ({})
        const itemId = String(item.id || "")
        if (itemId.length === 0) {
            showPlaybackEndState(false, "", "")
            return
        }
        if (!playQueueEntry(item, pendingAutomaticQueueIndex, true)) {
            showPlaybackEndState(
                true, qsTr("The next item could not be played."),
                "open-next")
        }
    }

    function replayCurrentItem() {
        const itemId = String(currentItemId || "")
        if (itemId.length === 0)
            return
        resetPlaybackEndState()
        player.paused = false
        replayRequested(itemId, playbackContext || ({}), mediaTitle)
    }

    function playAdjacent(item) {
        const requestedIndex = Number((item || {}).queueIndex)
        playQueueEntry(item, Number.isInteger(requestedIndex)
                       ? requestedIndex : -1)
    }

    function queueEntryAt(queueIndex) {
        if (!Number.isInteger(queueIndex) || queueIndex < 0 || !playbackQueue)
            return ({})
        for (let index = 0; index < playbackQueue.length; ++index) {
            const item = playbackQueue[index] || ({})
            const itemIndex = Number(item.queueIndex)
            if ((Number.isInteger(itemIndex) ? itemIndex : index) === queueIndex)
                return item
        }
        return ({})
    }

    function previousActionLabel() {
        return String((playbackContext || {}).kind || "").toLowerCase() === "series"
            ? qsTr("Previous episode") : qsTr("Previous item")
    }

    function nextActionLabel() {
        return String((playbackContext || {}).kind || "").toLowerCase() === "series"
            ? qsTr("Next episode") : qsTr("Next item")
    }

    function playQueueEntry(item, queueIndex, automaticAdvance) {
        const itemId = String(item && item.id ? item.id : "")
        if (itemId.length === 0 || switchingEpisode)
            return false
        const automatic = automaticAdvance === true
        if (automatic) {
            pendingAutomaticQueueItem = item || ({})
            pendingAutomaticQueueIndex = Number.isInteger(queueIndex)
                ? queueIndex : -1
            automaticAdvancePending = true
            automaticAdvanceOpening = false
        } else {
            automaticAdvancePending = false
            automaticAdvanceOpening = false
            pendingAutomaticQueueItem = ({})
            pendingAutomaticQueueIndex = -1
        }
        playbackEndVisible = false
        playbackEndRetryMode = false
        playbackEndMessage = ""
        const baseContext = (item && item.playbackContext)
            ? item.playbackContext : (playbackContext || ({}))
        const requestedContext = ({})
        for (const key in baseContext)
            requestedContext[key] = baseContext[key]
        if (Number.isInteger(queueIndex) && queueIndex >= 0)
            requestedContext.queueIndex = queueIndex
        const entryId = String((item || {}).playlistEntryId
            || (item || {}).queueEntryId || "")
        if (entryId.length > 0)
            requestedContext.playlistEntryId = entryId
        const lastPosition = player.position
        const wasPaused = automatic ? false : player.paused
        if (automatic)
            player.paused = false
        playbackActive = false
        introActivationTimer.stop()
        introOfferTimeout.stop()
        switchingEpisode = true
        // A naturally completed file is kept open by libmpv so its final
        // frame can remain behind preparation and any recoverable error UI.
        if (player.playbackState !== MpvVideoItem.Ended)
            player.stop()
        mediaUrl = ""
        episodeSwitchRequested(itemId, requestedContext, lastPosition, wasPaused)
        return true
    }

    function recoverFromPlaybackSwitchFailure(message) {
        if (!switchingEpisode)
            return
        const automatic = automaticAdvancePending
        switchingEpisode = false
        automaticAdvanceOpening = false
        automaticAdvancePending = false
        if (automatic) {
            showPlaybackEndState(
                true,
                String(message || qsTr("The next item could not be played.")),
                "open-next")
        } else {
            errorLabel.text = String(message || "")
            revealChrome()
        }
    }

    function skipIntro() {
        if (!canSkipIntro)
            return
        player.seek(introEndSeconds)
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
