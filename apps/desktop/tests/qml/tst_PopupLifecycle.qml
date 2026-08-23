import QtQuick
import QtQuick.Controls.Basic
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    property int navigationCount: 0
    property int surfaceClickCount: 0
    property int firstPopupActivationCount: 0
    property int secondPopupActivationCount: 0
    property int firstMenuActivationCount: 0
    property int secondMenuActivationCount: 0
    property real requestedVolume: -1

    name: "PopupLifecycle"
    width: 640
    height: 480
    visible: true
    when: windowShown

    Item {
        id: surface
        anchors.fill: parent

        MouseArea {
            anchors.fill: parent
            onClicked: ++testCase.surfaceClickCount
        }

        TextInput {
            id: openerFocusTarget
            x: 12
            y: 410
            z: 1
            width: 120
            height: 36
            text: "opener"
        }

        TextInput {
            id: outsideFocusTarget
            x: 480
            y: 410
            z: 1
            width: 120
            height: 36
            text: "outside"
        }
    }

    Item {
        id: popupSwitchFixture
        x: 180
        y: 390
        z: 10
        width: 280
        height: 44
        visible: false

        Row {
            spacing: 12

            AppPopupButton {
                id: firstPopupButton
                text: "Audio"
                controlSize: 38
                popupTarget: firstSwitchPopup
                peerPopups: [secondSwitchPopup]
                onPopupActivated: {
                    ++testCase.firstPopupActivationCount
                    nonExclusiveSwitchPopup.close()
                }
            }

            AppPopupButton {
                id: secondPopupButton
                text: "Subtitles"
                controlSize: 38
                popupTarget: secondSwitchPopup
                peerPopups: [firstSwitchPopup]
                onPopupActivated: {
                    ++testCase.secondPopupActivationCount
                    nonExclusiveSwitchPopup.close()
                }
            }
        }

        AppTransientPopup {
            id: firstSwitchPopup
            parent: firstPopupButton
            x: 0
            y: -height - 8
            width: 120
            height: 90
            padding: 0
            background: Rectangle { color: "#20242C" }
            contentItem: Rectangle { color: "#303640" }
        }

        AppTransientPopup {
            id: secondSwitchPopup
            parent: secondPopupButton
            x: secondPopupButton.width - width
            y: -height - 8
            width: 120
            height: 90
            padding: 0
            background: Rectangle { color: "#20242C" }
            contentItem: Rectangle { color: "#303640" }
        }

        AppTransientPopup {
            id: nonExclusiveSwitchPopup
            parent: popupSwitchFixture
            x: 80
            y: -height - 16
            width: 100
            height: 72
            padding: 0
            exclusiveWithinScope: false
            takesFocus: false
            blocksShortcuts: false
            background: Rectangle { color: "#252A33" }
            contentItem: Rectangle { color: "#343B46" }
        }
    }

    Item {
        id: menuSwitchFixture
        x: 20
        y: 330
        z: 10
        width: 600
        height: 44
        visible: false

        AppPopupButton {
            id: firstMenuButton
            x: 0
            text: "First menu"
            controlSize: 38
            popupTarget: firstSemanticMenu
            peerPopups: [secondSemanticMenu]
            onPopupActivated: ++testCase.firstMenuActivationCount

            TrackMenu {
                id: firstSemanticMenu
                parent: firstMenuButton
                x: 0
                y: -height - 8
                heading: "First"
                tracks: [{ id: 1, label: "One" }]
            }
        }

        AppPopupButton {
            id: secondMenuButton
            x: 430
            text: "Second menu"
            controlSize: 38
            popupTarget: secondSemanticMenu
            peerPopups: [firstSemanticMenu]
            onPopupActivated: ++testCase.secondMenuActivationCount

            TrackMenu {
                id: secondSemanticMenu
                parent: secondMenuButton
                x: secondMenuButton.width - width
                y: -height - 8
                heading: "Second"
                tracks: [{ id: 2, label: "Two" }]
            }
        }
    }

    VolumeControl {
        id: volumeControlFixture
        parent: surface
        x: 560
        y: 390
        z: 10
        visible: false
        volume: 64
        onVolumeRequested: value => testCase.requestedVolume = value
    }

    Rectangle {
        id: applicationChromeFixture
        parent: surface
        x: 500
        y: 12
        width: 100
        height: 32
        visible: false
        color: "#303640"

        Component.onCompleted:
            PopupCoordinator.registerApplicationChromeItem(
                applicationChromeFixture)
        Component.onDestruction:
            PopupCoordinator.unregisterApplicationChromeItem(
                applicationChromeFixture)
    }

    SmoothFlickable {
        id: wheelFixture
        x: 20
        y: 20
        z: 10
        width: 120
        height: 100
        visible: false
        contentWidth: width
        contentHeight: 400

        Rectangle {
            width: wheelFixture.width
            height: wheelFixture.contentHeight
            color: "#303640"
        }
    }

    TrackMenu {
        id: keyboardTrackMenu
        parent: surface
        heading: "Tracks"
        allowOff: true
        tracks: [
            { id: 1, label: "First" },
            { id: 2, label: "Last" }
        ]
    }

    SignalSpy { id: trackSelectedSpy; target: keyboardTrackMenu; signalName: "trackSelected" }

    MediaContextMenu {
        id: keyboardContextMenu
        isAdministrator: true
        canDelete: true
    }

    SignalSpy {
        id: contextActionSpy
        target: keyboardContextMenu
        signalName: "playbackRequested"
    }

    LibraryCard {
        id: keyboardLibraryCard
        parent: surface
        width: implicitWidth
        height: implicitHeight
        visible: false
        title: "Library"
        subtitle: "3 items"
        mediaItem: ({ itemId: "library-1", itemType: "CollectionFolder" })
    }

    PosterCard {
        id: keyboardPosterCard
        parent: surface
        width: implicitWidth
        height: implicitHeight
        visible: false
        title: "Series"
        subtitle: "2026"
        mediaItem: ({ itemId: "series-1", itemType: "Series" })
    }

    EpisodeCard {
        id: keyboardEpisodeCard
        parent: surface
        width: implicitWidth
        height: implicitHeight
        visible: false
        title: "Episode"
        subtitle: "S01 E01"
        mediaItem: ({ itemId: "episode-1", itemType: "Episode" })
    }

    RecentEpisodeCard {
        id: keyboardRecentCard
        parent: surface
        width: implicitWidth
        height: implicitHeight
        visible: false
        title: "Recent"
        subtitle: "S01 E02"
        mediaItem: ({ itemId: "episode-2", itemType: "Episode" })
    }

    SignalSpy { id: libraryActivatedSpy; target: keyboardLibraryCard; signalName: "activated" }
    SignalSpy { id: libraryContextSpy; target: keyboardLibraryCard; signalName: "contextMenuRequested" }
    SignalSpy { id: posterActivatedSpy; target: keyboardPosterCard; signalName: "activated" }
    SignalSpy { id: posterPlaySpy; target: keyboardPosterCard; signalName: "playRequested" }
    SignalSpy { id: posterContextSpy; target: keyboardPosterCard; signalName: "contextMenuRequested" }
    SignalSpy { id: episodePlaySpy; target: keyboardEpisodeCard; signalName: "playRequested" }
    SignalSpy { id: episodeContextSpy; target: keyboardEpisodeCard; signalName: "contextMenuRequested" }
    SignalSpy { id: recentPlaySpy; target: keyboardRecentCard; signalName: "playRequested" }
    SignalSpy { id: recentContextSpy; target: keyboardRecentCard; signalName: "contextMenuRequested" }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: PopupCoordinator.dismissTopOrNavigate(function() {
            ++testCase.navigationCount
        })
    }

    AppTransientPopup {
        id: transientPopup
        parent: surface
        x: 220
        y: 150
        width: 180
        height: 120
        padding: 0
        initialFocusTarget: transientFocusTarget
        focusReturnTarget: openerFocusTarget

        background: Rectangle { color: "#20242C" }
        contentItem: Rectangle {
            color: "#303640"

            TextInput {
                id: transientFocusTarget
                anchors.centerIn: parent
                width: 120
                height: 36
                text: "popup"
            }
        }
    }

    AppModalPopup {
        id: safeModal
        x: Math.round((parent.width - width) / 2)
        y: Math.round((parent.height - height) / 2)
        width: 220
        height: 150
        padding: 0

        background: Rectangle { color: "#20242C" }
        contentItem: Rectangle { color: "#303640" }
    }

    AppModalPopup {
        id: lowerModal
        x: 140
        y: 100
        width: 300
        height: 220
        padding: 0

        background: Rectangle { color: "#20242C" }
        contentItem: Rectangle { color: "#303640" }
    }

    AppModalPopup {
        id: upperConfirm
        x: 190
        y: 140
        width: 240
        height: 160
        padding: 0
        popupRole: PopupCoordinator.confirmRole

        background: Rectangle { color: "#282D36" }
        contentItem: Rectangle { color: "#383F4A" }
    }

    AppModalPopup {
        id: lockedModal
        x: 190
        y: 140
        width: 240
        height: 160
        padding: 0

        background: Rectangle { color: "#20242C" }
        contentItem: Rectangle { color: "#303640" }
    }

    AppModalPopup {
        id: dirtyModal
        x: 190
        y: 140
        width: 240
        height: 160
        padding: 0
        dirty: true
        confirmDirtyDismiss: true

        background: Rectangle { color: "#20242C" }
        contentItem: Rectangle { color: "#303640" }
    }

    SignalSpy {
        id: dirtyDiscardSpy
        target: dirtyModal
        signalName: "discardRequested"
    }

    function closePopup(popup) {
        popup.dismissBlocked = false
        if (popup.visible || popup.opened)
            popup.forceDismiss()
        tryCompare(popup, "opened", false)
    }

    function menuVisualSurface(item) {
        if (!item)
            return null
        const nested = findChild(item, "menu-hover-surface")
        return nested || item.background
    }

    function compareMenuVisuals(menu, visualIndex, showFocusFrame) {
        let fillCount = 0
        let frameCount = 0
        for (let index = 0; index < menu.count; ++index) {
            const surface = menuVisualSurface(menu.itemAt(index))
            verify(surface !== null, "missing menu visual at " + index)
            const filled = surface.color.a > 0.001
            const framed = surface.border.width > 0
            if (filled)
                ++fillCount
            if (framed)
                ++frameCount
            compare(filled, index === visualIndex,
                    "unexpected menu fill at " + index)
            compare(surface.border.width,
                    showFocusFrame && index === visualIndex ? 2 : 0,
                    "unexpected menu focus frame at " + index)
        }
        compare(fillCount, visualIndex >= 0 ? 1 : 0)
        compare(frameCount, showFocusFrame ? 1 : 0)
    }

    function compareMenuVisualsAcrossFrame(
            menu, visualIndex, showFocusFrame) {
        compareMenuVisuals(menu, visualIndex, showFocusFrame)
        const renderWindow = menu.contentItem.Window.window
        verify(renderWindow !== null, "menu has no render window")
        const originalOpacity = menu.contentItem.opacity
        // requestUpdate() alone need not visit an otherwise clean item on
        // every render backend. A visually negligible opacity change makes
        // this exact menu subtree dirty so waitForRendering() is meaningful.
        menu.contentItem.opacity = 0.999999
        renderWindow.requestUpdate()
        verify(waitForRendering(menu.contentItem, 500),
               "menu did not render after the input transition")
        compareMenuVisuals(menu, visualIndex, showFocusFrame)
        menu.contentItem.opacity = originalOpacity
    }

    function verifyMediaCardInputPresentation(card, activationSpy) {
        card.visible = true
        InputModality.noteKeyboardNavigation()
        card.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(card, "activeFocus", true)
        tryCompare(card, "navigationFocusVisible", true)

        const cardSurface = findChild(card, "media-card-surface")
        const hitArea = findChild(card, "media-card-hit-area")
        const pointerScrim = findChild(card, "media-card-pointer-scrim")
        const focusFrame = findChild(card, "media-card-focus-frame")
        verify(cardSurface !== null, "missing media card surface")
        verify(hitArea !== null, "missing stable media card hit area")
        verify(pointerScrim !== null, "missing media card pointer scrim")
        verify(focusFrame !== null, "missing media card focus frame")
        compare(hitArea.parent, card,
                "press animation must not transform the pointer hit area")
        compare(cardSurface.border.width, 1,
                "the original surface outline must remain below artwork")
        compare(focusFrame.visible, true)
        compare(focusFrame.border.width, 2)
        verify(focusFrame.z > pointerScrim.z,
               "focus frame must render above card content")
        const restingScrimColor = pointerScrim.color.toString()
        const restingScrimAlpha = pointerScrim.color.a

        // Two distinct global positions avoid the platform service filtering
        // a synthetic move that happens to match the existing cursor point.
        mouseMove(card, card.width - 3, cardSurface.height - 3)
        mouseMove(card, 18, 18)
        tryCompare(InputModality, "modality", InputModality.Pointer)
        tryCompare(card, "pointerHovered", true)
        compare(card.navigationFocusVisible, false)
        compare(focusFrame.visible, false)
        compare(pointerScrim.color.toString(), restingScrimColor,
                "pointer hover must not add a mismatched fill layer")

        mousePress(card, 18, 18, Qt.LeftButton)
        tryCompare(card, "pointerPressed", true)
        compare(card.activeFocus, true)
        compare(card.navigationFocusVisible, false)
        compare(focusFrame.visible, false)
        tryVerify(function() {
            return cardSurface.scale < 1
        })
        tryVerify(function() {
            return pointerScrim.color.a > restingScrimAlpha
        }, 500, "pointer press must still provide shade feedback")
        verify(Math.abs(pointerScrim.color.r - pointerScrim.color.g) < 0.001
               && Math.abs(pointerScrim.color.g - pointerScrim.color.b) < 0.001,
               "pointer press scrim must remain neutral")

        mouseRelease(card, 18, 18, Qt.LeftButton)
        tryCompare(card, "pointerPressed", false)
        tryCompare(activationSpy, "count", 1)

        // Hold at the very edge until the visual press scale has settled.
        // The release must still activate through the unscaled hit target.
        const edgeX = hitArea.x + 1
        const edgeY = hitArea.y + hitArea.height / 2
        mousePress(card, edgeX, edgeY, Qt.LeftButton)
        tryCompare(card, "pointerPressed", true)
        tryVerify(function() { return cardSurface.scale < 1 })
        mouseRelease(card, edgeX, edgeY, Qt.LeftButton)
        tryCompare(card, "pointerPressed", false)
        tryCompare(activationSpy, "count", 2)

        // The semantic focus acquired by the pointer remains useful for the
        // next navigation input, but pointer hover itself must disappear.
        InputModality.noteKeyboardNavigation()
        compare(card.activeFocus, true)
        tryCompare(card, "navigationFocusVisible", true)
        compare(focusFrame.visible, true)
        compare(card.pointerHovered, false)
        compare(focusFrame.border.width, 2)

        mouseMove(surface, surface.width - 4, surface.height - 4)
        tryCompare(InputModality, "modality", InputModality.Pointer)
        tryCompare(card, "navigationFocusVisible", false)
        compare(focusFrame.visible, false)
        card.visible = false
    }

    function verifyMediaCardProgressAlignment(card) {
        card.visible = true
        card.progress = 42

        const cardSurface = findChild(card, "media-card-surface")
        const progressStrip = findChild(card, "media-card-progress-strip")
        const progressTrack = findChild(card, "media-card-progress-track")
        verify(cardSurface !== null, "missing media card surface")
        verify(progressStrip !== null, "missing shared media card progress strip")
        verify(progressTrack !== null, "missing media card progress track")
        compare(progressStrip.x, 0)
        compare(progressStrip.y, 0)
        compare(progressStrip.width, cardSurface.width)
        compare(progressStrip.height, cardSurface.height)
        compare(progressTrack.x, 0)
        compare(progressTrack.width, cardSurface.width)
        compare(progressTrack.y + progressTrack.height, cardSurface.height,
                "progress track must sit exactly on the card bottom edge")
        compare(progressTrack.height, 4,
                "media card progress tracks must share one thickness")

        card.progress = 0
        card.visible = false
    }

    function init() {
        testCase.navigationCount = 0
        testCase.surfaceClickCount = 0
        testCase.firstPopupActivationCount = 0
        testCase.secondPopupActivationCount = 0
        testCase.firstMenuActivationCount = 0
        testCase.secondMenuActivationCount = 0
        testCase.requestedVolume = -1
        keyboardLibraryCard.visible = false
        keyboardPosterCard.visible = false
        keyboardEpisodeCard.visible = false
        keyboardRecentCard.visible = false
        libraryActivatedSpy.clear()
        libraryContextSpy.clear()
        posterActivatedSpy.clear()
        posterPlaySpy.clear()
        posterContextSpy.clear()
        episodePlaySpy.clear()
        episodeContextSpy.clear()
        recentPlaySpy.clear()
        recentContextSpy.clear()
        trackSelectedSpy.clear()
        contextActionSpy.clear()
        dirtyDiscardSpy.clear()
        closePopup(dirtyModal)
        dirtyModal.dirty = true
        closePopup(upperConfirm)
        closePopup(lowerModal)
        closePopup(lockedModal)
        closePopup(safeModal)
        closePopup(transientPopup)
        closePopup(keyboardTrackMenu)
        closePopup(keyboardContextMenu)
        closePopup(secondSemanticMenu)
        closePopup(firstSemanticMenu)
        closePopup(nonExclusiveSwitchPopup)
        closePopup(secondSwitchPopup)
        closePopup(firstSwitchPopup)
        volumeControlFixture.closePopup()
        volumeControlFixture.volume = 64
        volumeControlFixture.visible = false
        popupSwitchFixture.visible = false
        menuSwitchFixture.visible = false
        applicationChromeFixture.visible = false
        wheelFixture.visible = false
        wheelFixture.contentY = 0
        tryCompare(PopupCoordinator, "hasOpenPopup", false)
    }

    function cleanup() {
        init()
    }

    function test_insideAndOutsideDismissal() {
        transientPopup.open()
        tryCompare(transientPopup, "opened", true)

        mouseClick(transientPopup.contentItem,
                   transientPopup.contentItem.width / 2,
                   transientPopup.contentItem.height / 2)
        compare(transientPopup.opened, true)

        mouseClick(surface, 20, 100)
        // Qt's modeless Popup consumes an arbitrary outside click. QtTest also
        // bypasses the sibling Overlay handler when targeting surface, so feed
        // the same window point through the classifier explicitly.
        transientPopup.handleOverlayTap(
            transientPopup.Overlay.overlay,
            surface.mapToItem(transientPopup.Overlay.overlay, 20, 100))
        tryCompare(transientPopup, "opened", false)
        compare(testCase.surfaceClickCount, 0)

        // Both transient and modal overlays consume arbitrary outside clicks;
        // AppPopupButton has a dedicated single-click switch path above.
        testCase.surfaceClickCount = 0
        safeModal.open()
        tryCompare(safeModal, "opened", true)

        // Route through the test window rather than targeting the content
        // item directly. This exercises the real Overlay path used when a
        // user clicks a blank area inside a modal popup.
        mouseClick(testCase,
                   safeModal.x + safeModal.width / 2,
                   safeModal.y + safeModal.height / 2)
        compare(safeModal.opened, true)

        mouseClick(surface, 20, 20)
        tryCompare(safeModal, "opened", false)
        compare(testCase.surfaceClickCount, 0)
    }

    function test_transientPopupButtonsSwitchAndToggle() {
        popupSwitchFixture.visible = true

        mouseClick(firstPopupButton,
                   firstPopupButton.width / 2, firstPopupButton.height / 2)
        compare(testCase.firstPopupActivationCount, 1)
        tryCompare(firstSwitchPopup, "opened", true)
        compare(firstPopupButton.checked, true)
        compare(secondSwitchPopup.opened, false)
        compare(secondPopupButton.checked, false)

        mouseClick(secondPopupButton,
                   secondPopupButton.width / 2, secondPopupButton.height / 2)
        compare(testCase.secondPopupActivationCount, 1)
        tryCompare(firstSwitchPopup, "opened", false)
        tryCompare(secondSwitchPopup, "opened", true)
        compare(firstPopupButton.checked, false)
        compare(secondPopupButton.checked, true)

        mouseClick(secondPopupButton,
                   secondPopupButton.width / 2, secondPopupButton.height / 2)
        tryCompare(secondSwitchPopup, "opened", false)
        compare(secondPopupButton.checked, false)
    }

    function test_popupButtonOpensFromReturnKey() {
        popupSwitchFixture.visible = true
        firstPopupButton.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(firstPopupButton, "activeFocus", true)

        keyClick(Qt.Key_Return)
        tryCompare(firstSwitchPopup, "opened", true)
        compare(testCase.firstPopupActivationCount, 1)
    }

    function test_volumeOverlayClickMutesAndClosesPopupInOneClick() {
        volumeControlFixture.visible = true
        volumeControlFixture.showVolume()
        tryCompare(volumeControlFixture, "opened", true)

        const popup = PopupCoordinator.topPopup()
        verify(popup !== null)
        const overlay = popup.Overlay.overlay
        const buttonPoint = volumeControlFixture.mapToItem(
            overlay, volumeControlFixture.width / 2,
            volumeControlFixture.height / 2)
        compare(PopupCoordinator.overlayClickTargetAt(overlay, buttonPoint),
                volumeControlFixture)

        mouseClick(overlay, buttonPoint.x, buttonPoint.y)
        tryCompare(volumeControlFixture, "opened", false)
        compare(testCase.requestedVolume, 0)
    }

    function test_transientTopBlankIsNotAssumedToBeWindowChrome() {
        transientPopup.open()
        tryCompare(transientPopup, "opened", true)

        transientPopup.handleOverlayTap(
            transientPopup.Overlay.overlay,
            surface.mapToItem(transientPopup.Overlay.overlay, 20, 20))
        tryCompare(transientPopup, "opened", false)
    }

    function test_onlyTopTransientForwardsPopupButtonActivation() {
        popupSwitchFixture.visible = true

        mouseClick(firstPopupButton,
                   firstPopupButton.width / 2, firstPopupButton.height / 2)
        compare(testCase.firstPopupActivationCount, 1)
        tryCompare(firstSwitchPopup, "opened", true)

        nonExclusiveSwitchPopup.open()
        tryCompare(nonExclusiveSwitchPopup, "opened", true)
        verify(PopupCoordinator.isTop(nonExclusiveSwitchPopup))

        mouseClick(firstPopupButton,
                   firstPopupButton.width / 2, firstPopupButton.height / 2)
        compare(testCase.firstPopupActivationCount, 2)
        tryCompare(nonExclusiveSwitchPopup, "opened", false)
        tryCompare(firstSwitchPopup, "opened", false)
        compare(firstPopupButton.checked, false)
    }

    function test_smoothFlickableConsumesWheelByScrolling() {
        wheelFixture.visible = true
        compare(wheelFixture.contentY, 0)

        mouseWheel(wheelFixture,
                   wheelFixture.width / 2, wheelFixture.height / 2,
                   0, -120)
        tryVerify(function() { return wheelFixture.contentY > 0 })
    }

    function test_mediaCardsExposeCompleteKeyboardActions() {
        keyboardLibraryCard.visible = true
        keyboardLibraryCard.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(keyboardLibraryCard, "activeFocus", true)
        keyClick(Qt.Key_Return)
        compare(libraryActivatedSpy.count, 1)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        compare(libraryContextSpy.count, 1)
        keyboardLibraryCard.visible = false

        keyboardPosterCard.visible = true
        keyboardPosterCard.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(keyboardPosterCard, "activeFocus", true)
        keyClick(Qt.Key_Return)
        compare(posterActivatedSpy.count, 1)
        keyClick(Qt.Key_Space)
        compare(posterPlaySpy.count, 1)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        compare(posterContextSpy.count, 1)
        keyboardPosterCard.visible = false

        keyboardEpisodeCard.visible = true
        keyboardEpisodeCard.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(keyboardEpisodeCard, "activeFocus", true)
        keyClick(Qt.Key_Return)
        compare(episodePlaySpy.count, 1)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        compare(episodeContextSpy.count, 1)
        keyboardEpisodeCard.visible = false

        keyboardRecentCard.visible = true
        keyboardRecentCard.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(keyboardRecentCard, "activeFocus", true)
        keyClick(Qt.Key_Space)
        compare(recentPlaySpy.count, 1)
        keyClick(Qt.Key_F10, Qt.ShiftModifier)
        compare(recentContextSpy.count, 1)
    }

    function test_mediaCardsSeparatePointerPressFromNavigationFocus() {
        const cards = [keyboardLibraryCard, keyboardPosterCard,
                       keyboardEpisodeCard, keyboardRecentCard]
        const activationSpies = [libraryActivatedSpy, posterActivatedSpy,
                                 episodePlaySpy, recentPlaySpy]
        for (let index = 0; index < cards.length; ++index)
            verifyMediaCardInputPresentation(cards[index], activationSpies[index])
    }

    function test_mediaCardProgressTracksAlignWithArtworkBottomEdge() {
        verifyMediaCardProgressAlignment(keyboardPosterCard)
        verifyMediaCardProgressAlignment(keyboardEpisodeCard)
        verifyMediaCardProgressAlignment(keyboardRecentCard)
    }

    function test_trackMenuFocusesADelegateAndHandlesBoundaryKeys() {
        // Reopening in the same event-loop turn must not let the previous
        // close's deferred focus restoration win over the preferred item.
        keyboardTrackMenu.openPreferred(true)
        tryCompare(keyboardTrackMenu, "opened", true)
        keyboardTrackMenu.focusItem(2)
        tryCompare(keyboardTrackMenu, "currentIndex", 2)
        wait(20)
        compare(keyboardTrackMenu.currentIndex, 2,
                "deferred initial focus overwrote user navigation")
        keyboardTrackMenu.forceDismiss()
        tryCompare(keyboardTrackMenu, "opened", false)

        keyboardTrackMenu.openPreferred(true)
        tryCompare(keyboardTrackMenu, "opened", true)
        tryCompare(keyboardTrackMenu, "currentIndex", 0)
        verify(keyboardTrackMenu.itemAt(0) !== null)
        tryCompare(keyboardTrackMenu.itemAt(0), "activeFocus", true)
        tryCompare(keyboardTrackMenu.itemAt(0), "highlighted", true)
        compare(keyboardTrackMenu.itemAt(0).background.border.width, 2)
        keyClick(Qt.Key_End)
        tryCompare(keyboardTrackMenu, "currentIndex", 2)
        verify(keyboardTrackMenu.itemAt(2) !== null)
        tryCompare(keyboardTrackMenu.itemAt(2), "activeFocus", true)
        tryCompare(keyboardTrackMenu.itemAt(2), "highlighted", true)
        keyClick(Qt.Key_Return)
        tryCompare(trackSelectedSpy, "count", 1)
        compare(trackSelectedSpy.signalArguments[0][0], 2)
        tryCompare(keyboardTrackMenu, "opened", false)
    }

    function test_mediaContextMenuUsesSemanticCurrentItems() {
        openerFocusTarget.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(openerFocusTarget, "activeFocus", true)
        keyboardContextMenu.openFor({
            itemId: "movie-1",
            itemType: "Movie",
            title: "Movie"
        }, openerFocusTarget, 10, 10, true)
        tryCompare(keyboardContextMenu, "opened", true)
        const requestedPoint = openerFocusTarget.mapToItem(
            keyboardContextMenu.parent, 10, 10)
        const expectedX = Math.max(10, Math.min(
            requestedPoint.x,
            keyboardContextMenu.parent.width - keyboardContextMenu.width - 10))
        const desiredHeight = Math.min(
            keyboardContextMenu.height,
            keyboardContextMenu.parent.height - 20)
        const expectedY = Math.max(10, Math.min(
            requestedPoint.y,
            keyboardContextMenu.parent.height - desiredHeight - 10))
        compare(Math.round(keyboardContextMenu.x), Math.round(expectedX))
        compare(Math.round(keyboardContextMenu.y), Math.round(expectedY))
        tryCompare(keyboardContextMenu, "currentIndex", 0)
        verify(keyboardContextMenu.itemAt(0) !== null)
        tryCompare(keyboardContextMenu.itemAt(0), "activeFocus", true)
        tryCompare(keyboardContextMenu.itemAt(0), "highlighted", true)
        const contextSurface = findChild(
            keyboardContextMenu.itemAt(0), "menu-hover-surface")
        verify(contextSurface !== null)
        compare(contextSurface.border.width, 2)

        keyClick(Qt.Key_Down)
        tryCompare(keyboardContextMenu, "currentIndex", 1)
        keyClick(Qt.Key_End)
        tryCompare(keyboardContextMenu, "currentIndex",
                   keyboardContextMenu.count - 1)
        keyClick(Qt.Key_Home)
        tryCompare(keyboardContextMenu, "currentIndex", 0)
        keyClick(Qt.Key_Return)
        tryCompare(contextActionSpy, "count", 1)
        compare(contextActionSpy.signalArguments[0][0].itemId, "movie-1")
        compare(contextActionSpy.signalArguments[0][1], false)
        tryCompare(keyboardContextMenu, "opened", false)
        tryCompare(openerFocusTarget, "activeFocus", true)
    }

    function test_menuInputModalitiesHaveExactlyOneVisualTarget() {
        mouseMove(surface, surface.width - 4, surface.height - 4)

        keyboardTrackMenu.openPreferred()
        tryCompare(keyboardTrackMenu, "opened", true)
        const selectedTrackRow = keyboardTrackMenu.itemAt(0)
        const hoveredTrackRow = keyboardTrackMenu.itemAt(1)
        const focusTrackRow = keyboardTrackMenu.itemAt(2)
        verify(selectedTrackRow !== null)
        verify(hoveredTrackRow !== null)
        verify(focusTrackRow !== null)
        tryCompare(selectedTrackRow, "activeFocus", true)
        compareMenuVisualsAcrossFrame(keyboardTrackMenu, -1, false)

        mouseMove(hoveredTrackRow, hoveredTrackRow.width / 2,
                  hoveredTrackRow.height / 2)
        tryCompare(hoveredTrackRow, "hovered", true)
        compare(InputModality.modality, InputModality.Pointer)
        compareMenuVisualsAcrossFrame(keyboardTrackMenu, 1, false)

        // The physical pointer stays on row 1. Keyboard navigation must hide
        // that stale hover and render only the semantic current row.
        keyClick(Qt.Key_End)
        tryCompare(focusTrackRow, "activeFocus", true)
        compare(hoveredTrackRow.hovered, true)
        compare(InputModality.modality, InputModality.Keyboard)
        compareMenuVisualsAcrossFrame(keyboardTrackMenu, 2, true)

        const trackKeys = [Qt.Key_Home, Qt.Key_Down, Qt.Key_End,
                           Qt.Key_Up, Qt.Key_Home, Qt.Key_End]
        const trackIndices = [0, 1, 2, 1, 0, 2]
        for (let step = 0; step < trackKeys.length; ++step) {
            keyClick(trackKeys[step])
            compare(InputModality.modality, InputModality.Keyboard)
            compareMenuVisualsAcrossFrame(
                        keyboardTrackMenu, trackIndices[step], true)
        }

        // Controller navigation uses the same single focus visual while
        // remaining a distinct modality for future prompt glyphs.
        InputModality.noteControllerNavigation()
        keyboardTrackMenu.focusItem(1)
        compare(InputModality.modality, InputModality.Controller)
        compareMenuVisualsAcrossFrame(keyboardTrackMenu, 1, true)

        // A wheel gesture is pointer input even when the cursor does not
        // leave its already-hovered row.
        mouseWheel(hoveredTrackRow, hoveredTrackRow.width / 2,
                   hoveredTrackRow.height / 2, 0, 120)
        tryCompare(keyboardTrackMenu, "keyboardFocusVisible", false)
        compare(InputModality.modality, InputModality.Pointer)
        compareMenuVisualsAcrossFrame(keyboardTrackMenu, 1, false)
        closePopup(keyboardTrackMenu)

        mouseMove(surface, surface.width - 4, surface.height - 4)
        keyboardContextMenu.openFor({
            itemId: "movie-hover",
            itemType: "Movie",
            title: "Hover"
        }, openerFocusTarget, 10, 10)
        tryCompare(keyboardContextMenu, "opened", true)
        const hoveredContextRow = keyboardContextMenu.itemAt(0)
        const focusContextRow = keyboardContextMenu.itemAt(
            keyboardContextMenu.count - 1)
        verify(hoveredContextRow !== null)
        verify(focusContextRow !== null)
        const hoveredContextSurface = findChild(
            hoveredContextRow, "menu-hover-surface")
        const focusContextSurface = findChild(
            focusContextRow, "menu-hover-surface")
        verify(hoveredContextSurface !== null)
        verify(focusContextSurface !== null)
        tryCompare(hoveredContextRow, "activeFocus", true)
        compareMenuVisualsAcrossFrame(keyboardContextMenu, -1, false)

        mouseMove(hoveredContextRow, hoveredContextRow.width / 2,
                  hoveredContextRow.height / 2)
        tryCompare(hoveredContextRow, "hovered", true)
        compareMenuVisualsAcrossFrame(keyboardContextMenu, 0, false)
        keyClick(Qt.Key_End)
        tryCompare(focusContextRow, "activeFocus", true)
        compare(hoveredContextRow.hovered, true)
        compare(InputModality.modality, InputModality.Keyboard)
        compareMenuVisualsAcrossFrame(
                    keyboardContextMenu,
                    keyboardContextMenu.count - 1,
                    true)

        const contextKeys = [Qt.Key_Home, Qt.Key_Down, Qt.Key_End,
                             Qt.Key_Up, Qt.Key_Home, Qt.Key_End]
        const contextIndices = [0, 1, keyboardContextMenu.count - 1,
                                keyboardContextMenu.count - 2, 0,
                                keyboardContextMenu.count - 1]
        for (let step = 0; step < contextKeys.length; ++step) {
            keyClick(contextKeys[step])
            compare(InputModality.modality, InputModality.Keyboard)
            compareMenuVisualsAcrossFrame(
                        keyboardContextMenu, contextIndices[step], true)
        }

        InputModality.noteControllerNavigation()
        keyboardContextMenu.focusItem(1)
        const controllerContextSurface = findChild(
            keyboardContextMenu.itemAt(1), "menu-hover-surface")
        verify(controllerContextSurface !== null)
        compare(InputModality.modality, InputModality.Controller)
        compareMenuVisualsAcrossFrame(keyboardContextMenu, 1, true)

        mouseMove(hoveredContextRow, hoveredContextRow.width / 2 + 8,
                  hoveredContextRow.height / 2)
        tryCompare(keyboardContextMenu, "keyboardFocusVisible", false)
        compare(InputModality.modality, InputModality.Pointer)
        compareMenuVisualsAcrossFrame(keyboardContextMenu, 0, false)
    }

    function test_appMenuOutsideAndChromeClassification() {
        applicationChromeFixture.visible = true
        keyboardTrackMenu.openPreferred()
        tryCompare(keyboardTrackMenu, "opened", true)
        verify(keyboardTrackMenu._overlayTapHandler !== null)

        const overlay = keyboardTrackMenu.Overlay.overlay
        const inside = keyboardTrackMenu.background.mapToItem(
            overlay, keyboardTrackMenu.background.width / 2,
            keyboardTrackMenu.background.height / 2)
        keyboardTrackMenu.handleOverlayTap(overlay, inside)
        compare(keyboardTrackMenu.opened, true)

        const chrome = applicationChromeFixture.mapToItem(
            overlay, applicationChromeFixture.width / 2,
            applicationChromeFixture.height / 2)
        keyboardTrackMenu.handleOverlayTap(overlay, chrome)
        compare(keyboardTrackMenu.opened, true)

        const outside = surface.mapToItem(overlay, 20, 300)
        keyboardTrackMenu.handleOverlayTap(overlay, outside)
        tryCompare(keyboardTrackMenu, "opened", false)
        tryCompare(keyboardTrackMenu, "_overlayTapHandler", null)
    }

    function test_appMenuButtonsSwitchToggleAndRestoreFocus() {
        menuSwitchFixture.visible = true
        firstMenuButton.forceActiveFocus(Qt.TabFocusReason)
        tryCompare(firstMenuButton, "activeFocus", true)

        keyClick(Qt.Key_Return)
        tryCompare(firstSemanticMenu, "opened", true)
        compare(firstSemanticMenu.keyboardFocusVisible, true)
        const firstMenuOrigin = firstSemanticMenu.background.mapToItem(
            firstMenuButton, 0, 0)
        compare(Math.round(firstMenuOrigin.x), 0)
        compare(Math.round(firstMenuOrigin.y),
                Math.round(-firstSemanticMenu.height - 8))
        compare(testCase.firstMenuActivationCount, 1)
        compare(firstSemanticMenu.focusReturnTarget, firstMenuButton)

        const overlay = firstSemanticMenu.Overlay.overlay
        const secondPoint = secondMenuButton.mapToItem(
            overlay, secondMenuButton.width / 2, secondMenuButton.height / 2)
        compare(PopupCoordinator.overlayClickTargetAt(overlay, secondPoint),
                secondMenuButton)

        // Drive the Overlay itself so the dynamically installed TapHandler
        // and the production overlay-click classifier both participate.
        mouseClick(overlay, secondPoint.x, secondPoint.y)
        tryCompare(firstSemanticMenu, "opened", false)
        tryCompare(secondSemanticMenu, "opened", true)
        compare(secondSemanticMenu.keyboardFocusVisible, false)
        compare(testCase.secondMenuActivationCount, 1)
        compare(secondSemanticMenu.focusReturnTarget, secondMenuButton)

        keyClick(Qt.Key_Escape)
        tryCompare(secondSemanticMenu, "opened", false)
        tryCompare(secondMenuButton, "activeFocus", true)

        mouseClick(secondMenuButton,
                   secondMenuButton.width / 2, secondMenuButton.height / 2)
        tryCompare(secondSemanticMenu, "opened", true)
        compare(secondSemanticMenu.keyboardFocusVisible, false)
        const secondOverlay = secondSemanticMenu.Overlay.overlay
        const sameButtonPoint = secondMenuButton.mapToItem(
            secondOverlay, secondMenuButton.width / 2,
            secondMenuButton.height / 2)
        mouseClick(secondOverlay, sameButtonPoint.x, sameButtonPoint.y)
        tryCompare(secondSemanticMenu, "opened", false)
        compare(testCase.secondMenuActivationCount, 3)
        tryCompare(secondMenuButton, "activeFocus", true)
    }

    function test_escapeUsesSingleCoordinatorRoute() {
        transientPopup.open()
        tryCompare(transientPopup, "opened", true)

        keyClick(Qt.Key_Escape)
        tryCompare(transientPopup, "opened", false)
        compare(testCase.navigationCount, 0)

        keyClick(Qt.Key_Escape)
        tryCompare(testCase, "navigationCount", 1)
    }

    function test_stackClosesOnlyTheTopPopup() {
        lowerModal.open()
        upperConfirm.open()
        tryCompare(lowerModal, "opened", true)
        tryCompare(upperConfirm, "opened", true)
        verify(upperConfirm.z > lowerModal.z)
        verify(PopupCoordinator.isTop(upperConfirm))

        keyClick(Qt.Key_Escape)
        tryCompare(upperConfirm, "opened", false)
        compare(lowerModal.opened, true)
        verify(PopupCoordinator.isTop(lowerModal))

        keyClick(Qt.Key_Escape)
        tryCompare(lowerModal, "opened", false)
    }

    function test_mutationLockConsumesDismissal() {
        lockedModal.dismissBlocked = true
        lockedModal.open()
        tryCompare(lockedModal, "opened", true)

        mouseClick(surface, 20, 20)
        compare(lockedModal.opened, true)

        keyClick(Qt.Key_Escape)
        compare(lockedModal.opened, true)
        compare(testCase.navigationCount, 0)
        compare(lockedModal.requestDismiss("cancel"), false)

        // Prove the same outside path is genuinely exercised; otherwise the
        // assertions above could pass simply because outside presses are lost.
        lockedModal.dismissBlocked = false
        mouseClick(surface, 20, 20)
        tryCompare(lockedModal, "opened", false)
    }

    function test_dirtyPopupRequestsDiscardWithoutClosing() {
        dirtyModal.open()
        tryCompare(dirtyModal, "opened", true)

        mouseClick(surface, 20, 20)
        tryCompare(dirtyDiscardSpy, "count", 1)
        compare(dirtyDiscardSpy.signalArguments[0][0], "outside")
        compare(dirtyModal.opened, true)
        compare(testCase.surfaceClickCount, 0)

        keyClick(Qt.Key_Escape)
        tryCompare(dirtyDiscardSpy, "count", 2)
        compare(dirtyDiscardSpy.signalArguments[1][0], "escape")
        compare(dirtyModal.opened, true)
        compare(testCase.navigationCount, 0)

        dirtyModal.dirty = false
        dirtyModal.forceDismiss()
        tryCompare(dirtyModal, "opened", false)
    }

    function test_applicationChromeOutranksEveryPopupRole() {
        verify(PopupCoordinator.toolTipZ
               > PopupCoordinator.layerBase(PopupCoordinator.errorRole)
                   + 100000)
        verify(PopupCoordinator.applicationChromeZ
               > PopupCoordinator.toolTipZ)
    }

    function test_applicationChromePressDoesNotDismissModal() {
        safeModal.open()
        tryCompare(safeModal, "opened", true)

        PopupCoordinator.noteApplicationChromePress()
        mouseClick(surface, 20, 20)
        wait(80)
        compare(safeModal.opened, true)

        wait(100)
        mouseClick(surface, 20, 20)
        tryCompare(safeModal, "opened", false)
    }

    function test_focusRestoresAfterEscapeAndOutsideDismissal() {
        openerFocusTarget.forceActiveFocus()
        tryCompare(openerFocusTarget, "activeFocus", true)

        transientPopup.open()
        tryCompare(transientPopup, "opened", true)
        tryCompare(transientFocusTarget, "activeFocus", true)
        keyClick(Qt.Key_Escape)
        tryCompare(transientPopup, "opened", false)
        tryCompare(openerFocusTarget, "activeFocus", true)

        transientPopup.open()
        tryCompare(transientFocusTarget, "activeFocus", true)
        const outsideX = outsideFocusTarget.width / 2
        const outsideY = outsideFocusTarget.height / 2
        mouseClick(outsideFocusTarget, outsideX, outsideY)
        transientPopup.handleOverlayTap(
            transientPopup.Overlay.overlay,
            outsideFocusTarget.mapToItem(
                transientPopup.Overlay.overlay, outsideX, outsideY))
        tryCompare(transientPopup, "opened", false)
        tryCompare(openerFocusTarget, "activeFocus", true)
        compare(outsideFocusTarget.activeFocus, false)
    }
}
