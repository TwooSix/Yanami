import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami
import Yanami.Ui

ApplicationWindow {
    id: window
    width: 1240
    height: 800
    minimumWidth: 900
    minimumHeight: 620
    visible: true
    title: "Yanami"
    color: Theme.background
    flags: Qt.Window
        | Qt.FramelessWindowHint
        | Qt.WindowSystemMenuHint
        | Qt.WindowMinimizeButtonHint
        | Qt.WindowMaximizeButtonHint
        | Qt.WindowCloseButtonHint

    readonly property bool fullScreenMode: windowShell.fullScreen
    readonly property bool roundedFrame: !fullScreenMode
        && visibility !== Window.Maximized

    property int currentPage: 0
    property int playerReturnPage: 0
    readonly property PlayerPage playerPage: playerLoader.item as PlayerPage
    readonly property SearchPage searchPage: searchLoader.item as SearchPage
    readonly property FavoritesPage favoritesPage: favoritesLoader.item as FavoritesPage
    readonly property SettingsPage settingsPage: settingsLoader.item as SettingsPage
    property bool searchLoaded: false
    property bool settingsLoaded: false
    property bool favoritesLoaded: false
    property bool aboutLoaded: false
    property double developmentSeekSeconds: 0
    property int developmentAutoStopMs: 0
    property bool developmentLoadingPreview: false
    property bool developmentAutoSkipIntro: false
    property bool developmentRenderDiagnostics: false
    property bool developmentDanmakuPreview: false
    property bool developmentPlaybackQueuePreview: false
    property string developmentDanmakuSearchQuery: ""
    property double developmentDanmakuPreviewFontSize: 0
    property bool developmentDisableDanmakuAfterLoad: false
    property bool developmentReenableDanmakuAfterDisable: false
    property url developmentLocalMediaUrl
    property int developmentDanmakuStyleStressCount: 1
    property int developmentDanmakuToggleStressCount: 0
    property string developmentSearchQuery
    property string developmentMediaMenuPreview: ""
    property string developmentCollectionSequence: ""
    property int developmentCollectionStep: 0
    property bool developmentScrollRegression: false
    property real developmentLibraryScanProgress: -1
    readonly property bool developmentForceAdminMenu:
        developmentMediaMenuPreview === "admin-context"
            || developmentMediaMenuPreview === "admin-library-context"
    property bool forcePlaybackFromBeginning: false
    property var pendingDeleteItem: ({})
    property int focusTrackedPage: 0
    readonly property var controllerPageOrder: [0, 1, 4, 5, 3]

    onCurrentPageChanged: {
        if (focusTrackedPage !== currentPage) {
            if (InputModality.focusNavigationActive && window.activeFocusItem)
                focusNavigator.remember(focusTrackedPage, window.activeFocusItem)
            focusTrackedPage = currentPage
        }
        if (currentPage === 0) {
            if (app.session.connected)
                app.home.ensureActivityFresh()
        } else if (currentPage === 1)
            searchLoaded = true
        else if (currentPage === 3)
            settingsLoaded = true
        else if (currentPage === 4) {
            favoritesLoaded = true
            app.favorites.load()
        } else if (currentPage === 5)
            aboutLoaded = true

        if (currentPage !== 2 && InputModality.focusNavigationActive)
            Qt.callLater(function() { window.focusCurrentPage(false) })
    }

    onDevelopmentSearchQueryChanged: {
        if (developmentSearchQuery.length === 0)
            return
        searchLoaded = true
        currentPage = 1
        Qt.callLater(function() {
            if (window.searchPage)
                window.searchPage.query = developmentSearchQuery
        })
    }
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
        const page = window.playerPage
        console.info("playback_ui_close",
                     "itemId=", page ? page.currentItemId : "",
                     "returnPage=", window.playerReturnPage)
        if (page && (page.preparingPlayback || page.switchingEpisode))
            app.playback.cancelPreparation()
        if (page)
            page.closePlayback()
        windowShell.exitFullScreen()
        window.currentPage = window.playerReturnPage
        Qt.callLater(function() {
            if (window.currentPage !== 2)
                playerLoader.active = false
        })
    }

    function ensurePlayerPage() {
        playerLoader.active = true
    }

    function showBackendError() {
        if (!app.status.ready && app.status.error
                && app.status.message.length > 0)
            errorDialog.show(app.status.message)
    }

    function showActionToast(message, tone) {
        if (message && message.length > 0)
            actionToast.show(message, tone)
    }

    function playbackWarningMessage(warnings) {
        const values = warnings || []
        if (values.length === 0)
            return ""
        let unavailableSubtitles = 0
        for (let index = 0; index < values.length; ++index) {
            if (String((values[index] || {}).code || "")
                    === "external_subtitle_unavailable")
                ++unavailableSubtitles
        }
        if (unavailableSubtitles === 1 && values.length === 1)
            return qsTr("An external subtitle is unavailable.")
        if (unavailableSubtitles === values.length)
            return qsTr("Some external subtitles are unavailable.")
        return qsTr("Playback started with a non-fatal warning.")
    }

    function reportMediaActionFailure(message, nonModal, handledInPlace) {
        if (nonModal && !handledInPlace)
            window.showActionToast(message, "error")
    }

    function deliverDanmakuResult(itemId, result) {
        const page = window.playerPage
        if (page && page.currentItemId === itemId)
            page.handleDanmakuResult(result)
    }

    function reportDanmakuFailure(itemId, message, nonModal) {
        const page = window.playerPage
        if (nonModal && window.currentPage === 2 && page
                && page.currentItemId === itemId)
            window.showActionToast(message, "error")
    }

    function navigateBack() {
        if (window.currentPage === 2) {
            if (window.playerPage && window.playerPage.consumeBack())
                return
            if (window.playerPage && window.playerPage.playbackEndVisible)
                window.closePlayer()
            else if (window.fullScreenMode)
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

    function activePageItem() {
        if (window.currentPage === 0)
            return homePage
        if (window.currentPage === 1)
            return searchLoader.item
        if (window.currentPage === 3)
            return settingsLoader.item
        if (window.currentPage === 4)
            return favoritesLoader.item
        if (window.currentPage === 5)
            return aboutLoader.item
        return null
    }

    function selectedNavigationButton() {
        if (window.currentPage === 1)
            return searchNavButton
        if (window.currentPage === 3)
            return settingsNavButton
        if (window.currentPage === 4)
            return favoritesNavButton
        if (window.currentPage === 5)
            return aboutNavButton
        return homeNavButton
    }

    function focusCurrentPage(forceDefault) {
        if (!InputModality.focusNavigationActive || window.currentPage === 2
                || PopupCoordinator.hasOpenPopup)
            return false
        const page = window.activePageItem()
        if (!page)
            return focusNavigator.focusItem(window.selectedNavigationButton())

        let target = null
        if (forceDefault !== true)
            target = focusNavigator.pageBookmarks[String(window.currentPage)]
        if ((!target || forceDefault === true)
                && typeof page.controllerDefaultFocusItem === "function") {
            target = page.controllerDefaultFocusItem()
        }
        if (focusNavigator.focusItem(target))
            return true
        if (window.currentPage === 1
                && typeof page.focusSearch === "function") {
            page.focusSearch()
            return true
        }
        if (focusNavigator.focusFirst(page))
            return true
        return focusNavigator.focusItem(window.selectedNavigationButton())
    }

    function selectControllerPage(page) {
        const destination = Number(page)
        if (destination === 0)
            homePage.goHome()
        if (destination === 1)
            window.searchLoaded = true
        else if (destination === 3)
            window.settingsLoaded = true
        else if (destination === 4)
            window.favoritesLoaded = true
        else if (destination === 5)
            window.aboutLoaded = true
        window.currentPage = destination
    }

    function switchControllerPage(step) {
        const currentIndex = window.controllerPageOrder.indexOf(
            window.currentPage)
        if (currentIndex < 0)
            return false
        const targetIndex = Math.max(0, Math.min(
            window.controllerPageOrder.length - 1, currentIndex + step))
        if (targetIndex === currentIndex)
            return false
        window.selectControllerPage(window.controllerPageOrder[targetIndex])
        return true
    }

    function openControllerMenu() {
        if (window.currentPage === 2 || PopupCoordinator.hasOpenPopup)
            return false
        controllerMenu.focusReturnTarget = window.activeFocusItem
        controllerMenu.openPreferred(true)
        return true
    }

    function requestPlayback(itemId, fromBeginning, playbackContext, title) {
        console.info("playback_ui_request",
                     "itemId=", itemId,
                     "fromBeginning=", fromBeginning === true,
                     "currentPage=", window.currentPage,
                     "sessionConnected=", app.session.connected)
        window.forcePlaybackFromBeginning = fromBeginning === true
        if (window.currentPage !== 2)
            window.playerReturnPage = window.currentPage
        window.ensurePlayerPage()
        window.playerPage.beginPreparation(itemId, title)
        window.currentPage = 2
        console.info("playback_ui_navigated",
                     "itemId=", itemId, "page=", 2,
                     "phase=preparing")
        app.playback.prepareInContext(itemId, playbackContext || ({}))
    }

    function requestMediaTargets(item, opener) {
        const itemId = String(item.id || "")
        if (itemId.length === 0)
            return false
        mediaTargetDialog.focusReturnTarget = opener || null
        return app.mediaTarget.load(item)
    }

    function openContextItem(item) {
        const type = String(item.itemType || "")
        const previousPage = window.currentPage
        if (type === "CollectionFolder" || type === "UserView" || type === "Folder"
                || type === "AggregateFolder") {
            window.currentPage = 0
            homePage.openLibraryView(item)
        } else if (type === "Playlist") {
            window.currentPage = 0
            if (previousPage !== 0)
                homePage.openExternalItem(item, previousPage)
            else
                homePage.openLibraryItem(item)
        } else if (type === "Series") {
            const cameFromExternalPage = previousPage !== 0
            window.currentPage = 0
            if (cameFromExternalPage)
                homePage.openExternalItem(item, previousPage)
            else
                homePage.openLibraryItem(item)
        } else if (type === "Season") {
            window.currentPage = 0
            if (previousPage !== 0)
                homePage.openExternalSeason(item, previousPage)
            else
                homePage.openSeason(item)
        } else if (type === "Episode" || type === "Movie"
                   || type === "Video" || type === "MusicVideo") {
            window.requestPlayback(
                item.id, false, item.playbackContext || ({}), item.title)
        } else {
            window.currentPage = 0
            if (previousPage !== 0)
                homePage.openExternalItem(item, previousPage)
            else
                homePage.openLibraryItem(item)
        }
    }

    function tryDevelopmentMediaMenuPreview() {
        if (window.developmentMediaMenuPreview === "context"
                || window.developmentMediaMenuPreview === "admin-context")
            return homePage.openDevelopmentContextPreview()
        if (window.developmentMediaMenuPreview === "admin-library-context")
            return homePage.openDevelopmentLibraryContextPreview()
        if (window.developmentMediaMenuPreview === "metadata"
                || window.developmentMediaMenuPreview === "metadata-ids") {
            const item = homePage.developmentPreviewItem()
            if (!item || !item.id || !app.session.administrator)
                return false
            metadataEditor.beginLoading(item)
            return true
        }
        if (window.developmentMediaMenuPreview === "metadata-mock"
                || window.developmentMediaMenuPreview === "metadata-ids-mock") {
            metadataEditor.openFor({
                id: "preview",
                itemType: "Series",
                title: "Metadata preview",
                originalTitle: "Original title",
                sortName: "Metadata preview",
                overview: "Preview of the administrator metadata editor.",
                productionYear: 2026,
                premiereDate: "2026-08-12",
                officialRating: "TV-14",
                communityRating: 8.6,
                genres: ["Animation", "Comedy"],
                tags: ["Preview"],
                editableFields: ["title", "originalTitle", "sortName", "overview",
                    "productionYear", "premiereDate", "officialRating",
                    "communityRating", "genres", "tags"],
                externalIds: [
                    { key: "Tmdb", name: "TheMovieDb", value: "12345", supportedAsIdentifier: true },
                    { key: "Tvdb", name: "TheTVDB", value: "67890", supportedAsIdentifier: true },
                    { key: "Imdb", name: "IMDb", value: "tt1234567", supportedAsIdentifier: true }
                ]
            })
            if (window.developmentMediaMenuPreview === "metadata-ids-mock")
                metadataEditor.scrollToExternalIdentifiers()
            return true
        }
        if (window.developmentMediaMenuPreview === "images") {
            const item = homePage.developmentPreviewItem()
            if (!item || !item.id || !app.session.administrator)
                return false
            imageEditor.beginLoading(item)
            return true
        }
        if (window.developmentMediaMenuPreview === "images-mock"
                || window.developmentMediaMenuPreview === "image-search-mock") {
            const previewUrl = Qt.resolvedUrl(
                "qrc:/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")
            imageEditor.openFor({
                id: "preview",
                itemType: "Series",
                title: "Image editor preview",
                imageTypes: ["Primary", "Backdrop", "Thumb", "Banner", "Logo", "Art", "Disc"],
                images: [
                    { imageType: "Primary", imageIndex: 0, width: 1000, height: 1500,
                      previewUrl: previewUrl },
                    { imageType: "Primary", imageIndex: 1, width: 1000, height: 1500,
                      previewUrl: previewUrl }
                ],
                providers: [
                    { name: "TheMovieDb", supportedImages: ["Primary", "Backdrop", "Logo"] },
                    { name: "TheTVDB", supportedImages: ["Primary", "Backdrop", "Banner"] }
                ]
            })
            if (window.developmentMediaMenuPreview === "image-search-mock") {
                imageEditor.showSearchPreview({
                    images: [
                        { providerName: "TheMovieDb", width: 2000, height: 3000,
                          language: "zh", displayLanguage: "中文", imageUrl: "https://example.test/1.jpg",
                          previewUrl: previewUrl },
                        { providerName: "TheTVDB", width: 1000, height: 1500,
                          language: "en", displayLanguage: "English", imageUrl: "https://example.test/2.jpg",
                          previewUrl: previewUrl }
                    ]
                }, "Primary")
            }
            return true
        }
        if (window.developmentMediaMenuPreview === "refresh-metadata-mock") {
            const item = homePage.developmentPreviewItem()
            if (!item)
                return false
            refreshMetadataDialog.openFor(item)
            return true
        }
        if (window.developmentMediaMenuPreview === "playlist-targets") {
            const item = homePage.developmentPreviewItem()
            if (!item || !item.id)
                return false
            return window.requestMediaTargets(item, null)
        }
        if (window.developmentMediaMenuPreview === "action-error-mock") {
            window.showActionToast(
                "The server could not complete this action. Please try again.",
                "error")
            return true
        }
        return true
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
        Keys.priority: Keys.AfterItem
        Keys.onPressed: event => {
            if (!InputModality.focusNavigationActive
                    || window.currentPage === 2
                    || PopupCoordinator.hasOpenPopup)
                return
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
                event.accepted = focusNavigator.move(direction)
        }

        SpatialFocusNavigator {
            id: focusNavigator
            navigationRoot: appFrame
        }

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
                        id: homeNavButton
                        iconName: "home"
                        accessibleName: qsTr("Home")
                        selected: window.currentPage === 0
                        onClicked: {
                            homePage.goHome()
                            window.currentPage = 0
                        }
                        KeyNavigation.down: searchNavButton
                    }
                    NavButton {
                        id: searchNavButton
                        iconName: "search"
                        accessibleName: qsTr("Search")
                        selected: window.currentPage === 1
                        onClicked: {
                            window.searchLoaded = true
                            window.currentPage = 1
                            Qt.callLater(function() {
                                if (window.searchPage)
                                    window.searchPage.focusSearch()
                            })
                        }
                        KeyNavigation.up: homeNavButton
                        KeyNavigation.down: favoritesNavButton
                    }
                    NavButton {
                        id: favoritesNavButton
                        iconName: "heart"
                        accessibleName: qsTr("Favorites")
                        selected: window.currentPage === 4
                        onClicked: {
                            window.favoritesLoaded = true
                            window.currentPage = 4
                        }
                        KeyNavigation.up: searchNavButton
                        KeyNavigation.down: aboutNavButton
                    }
                    Item { Layout.fillHeight: true }
                    NavButton {
                        id: aboutNavButton
                        iconName: "info"
                        accessibleName: qsTr("About")
                        selected: window.currentPage === 5
                        onClicked: {
                            window.aboutLoaded = true
                            window.currentPage = 5
                        }
                        KeyNavigation.up: favoritesNavButton
                        KeyNavigation.down: settingsNavButton
                    }
                    NavButton {
                        id: settingsNavButton
                        iconName: "settings"
                        accessibleName: qsTr("Settings")
                        selected: window.currentPage === 3
                        onClicked: {
                            window.settingsLoaded = true
                            window.currentPage = 3
                        }
                        KeyNavigation.up: aboutNavButton
                    }
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
                    collectionParent: app.home.collectionParent
                    developmentLibraryScanProgress: window.developmentLibraryScanProgress
                    onPlayRequested: (itemId, title, playbackContext) =>
                        window.requestPlayback(
                            itemId, false, playbackContext, title)
                    onMediaContextRequested: (item, sourceItem, x, y,
                                              keyboardInvocation) =>
                        mediaContextMenu.openFor(
                            item, sourceItem, x, y, keyboardInvocation)
                    onSettingsRequested: window.currentPage = 3
                    onExternalReturnRequested: page => window.currentPage = page
                    onControllerFocusRequested:
                        Qt.callLater(function() {
                            window.focusCurrentPage(true)
                        })
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Loader {
                        id: searchLoader
                        anchors.fill: parent
                        active: window.searchLoaded
                        asynchronous: true
                        sourceComponent: SearchPage {
                            developmentDiagnostics: window.developmentSearchQuery.length > 0
                            controllerExitTarget: searchNavButton
                            onItemRequested: item => window.openContextItem(item)
                            onPlayRequested: (itemId, title, playbackContext) =>
                                window.requestPlayback(
                                    itemId, false, playbackContext, title)
                            onMediaContextRequested: (item, sourceItem, x, y,
                                                      keyboardInvocation) =>
                                mediaContextMenu.openFor(
                                    item, sourceItem, x, y,
                                    keyboardInvocation)
                        }
                        onLoaded: {
                            if (window.developmentSearchQuery.length > 0)
                                item.query = window.developmentSearchQuery
                            if (window.currentPage === 1)
                                item.focusSearch()
                        }
                    }

                    LoadingIndicator {
                        anchors.centerIn: parent
                        visible: searchLoader.active
                            && searchLoader.status === Loader.Loading
                    }
                }
                Loader {
                    id: playerLoader
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    active: false

                    sourceComponent: PlayerPage {
                        globalToastBottom: actionToast.visible
                            ? actionToast.y + actionToast.height : 0
                        developmentSeekSeconds: window.developmentSeekSeconds
                        developmentAutoSkipIntro: window.developmentAutoSkipIntro
                        developmentRenderDiagnostics: window.developmentRenderDiagnostics
                        developmentDanmakuPreview: window.developmentDanmakuPreview
                        developmentPlaybackQueuePreview:
                            window.developmentPlaybackQueuePreview
                        developmentDanmakuSearchQuery: window.developmentDanmakuSearchQuery
                        developmentDanmakuPreviewFontSize: window.developmentDanmakuPreviewFontSize
                        developmentDisableDanmakuAfterLoad: window.developmentDisableDanmakuAfterLoad
                        developmentReenableDanmakuAfterDisable: window.developmentReenableDanmakuAfterDisable
                        developmentSyntheticDanmaku: window.developmentLocalMediaUrl.toString().length > 0
                        developmentDanmakuStyleStressCount: window.developmentDanmakuStyleStressCount
                        developmentDanmakuToggleStressCount: window.developmentDanmakuToggleStressCount
                        loadingPreview: window.developmentLoadingPreview
                        windowFullScreen: window.fullScreenMode
                        onPlaybackLoaded: {
                            if (window.developmentAutoStopMs > 0)
                                developmentStopTimer.restart()
                        }
                        onCloseRequested: window.closePlayer()
                        onToggleFullScreenRequested: window.toggleFullScreen()
                        onEpisodeSwitchRequested: (itemId, playbackContext,
                                                   positionSeconds, paused) => {
                            app.playback.switchToInContext(
                                itemId, playbackContext, positionSeconds, paused)
                        }
                        onReplayRequested: (itemId, playbackContext, title) => {
                            window.requestPlayback(
                                itemId, true, playbackContext, title)
                        }
                        onQueueRefreshRequested: (itemId, playbackContext) => {
                            app.playback.prepareInContext(
                                itemId, playbackContext || ({}))
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Loader {
                        id: settingsLoader
                        anchors.fill: parent
                        active: window.settingsLoaded
                        asynchronous: true
                        sourceComponent: SettingsPage {
                            pageActive: window.currentPage === 3
                        }
                        onLoaded: {
                            if (window.currentPage === 3
                                    && InputModality.focusNavigationActive) {
                                Qt.callLater(function() {
                                    window.focusCurrentPage(false)
                                })
                            }
                        }
                    }

                    LoadingIndicator {
                        anchors.centerIn: parent
                        visible: settingsLoader.active
                            && settingsLoader.status === Loader.Loading
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Loader {
                        id: favoritesLoader
                        anchors.fill: parent
                        active: window.favoritesLoaded
                        asynchronous: true
                        sourceComponent: FavoritesPage {
                            refreshing: app.favorites.refreshing
                            loadFailed: app.favorites.loadFailed
                            onItemRequested: item => window.openContextItem(item)
                            onPlayRequested: (itemId, title) =>
                                window.requestPlayback(itemId, false, ({}), title)
                            onMediaContextRequested: (item, sourceItem, x, y,
                                                      keyboardInvocation) =>
                                mediaContextMenu.openFor(
                                    item, sourceItem, x, y,
                                    keyboardInvocation)
                            onRetryRequested: app.favorites.refresh()
                        }
                        onLoaded: {
                            if (window.currentPage === 4
                                    && InputModality.focusNavigationActive) {
                                Qt.callLater(function() {
                                    window.focusCurrentPage(false)
                                })
                            }
                        }
                    }

                    LoadingIndicator {
                        anchors.centerIn: parent
                        visible: favoritesLoader.active
                            && favoritesLoader.status === Loader.Loading
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Loader {
                        id: aboutLoader
                        anchors.fill: parent
                        active: window.aboutLoaded
                        asynchronous: true
                        sourceComponent: AboutPage {}
                        onLoaded: {
                            if (window.currentPage === 5
                                    && InputModality.focusNavigationActive) {
                                Qt.callLater(function() {
                                    window.focusCurrentPage(false)
                                })
                            }
                        }
                    }

                    LoadingIndicator {
                        anchors.centerIn: parent
                        visible: aboutLoader.active
                            && aboutLoader.status === Loader.Loading
                    }
                }
            }
        }

        LoadingOverlay {
            anchors.fill: parent
            z: 100
            active: window.currentPage !== 2
                && window.developmentLoadingPreview
        }

        WindowTitleBar {
            parent: Overlay.overlay
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            z: PopupCoordinator.applicationChromeZ
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
        }

        MediaContextMenu {
            id: mediaContextMenu
            isAdministrator: window.developmentForceAdminMenu || app.session.administrator
            canDelete: window.developmentForceAdminMenu || app.session.canDelete
            onOpenRequested: item => window.openContextItem(item)
            onPlaybackRequested: (item, fromBeginning) =>
                window.requestPlayback(
                    item.id, fromBeginning,
                    item.playbackContext || ({}), item.title)
            onPlayedChangeRequested: (item, played) =>
                app.mediaActions.setPlayed(item.id, played)
            onFavoriteChangeRequested: (item, favorite) =>
                app.mediaActions.setFavorite(item.id, favorite)
            onPlaylistAddRequested: (item, opener) =>
                window.requestMediaTargets(item, opener)
            onMetadataEditRequested: (item, opener) => {
                metadataEditor.focusReturnTarget = opener || null
                metadataEditor.beginLoading(item)
            }
            onImageEditRequested: (item, opener) => {
                imageEditor.focusReturnTarget = opener || null
                imageEditor.beginLoading(item)
            }
            onLibraryScanRequested: item =>
                app.mediaActions.scanLibraryFiles(item.id)
            onMetadataRefreshRequested: (item, opener) => {
                refreshMetadataDialog.focusReturnTarget = opener || null
                refreshMetadataDialog.openFor(item)
            }
            onDeleteRequested: (item, opener) => {
                deleteConfirm.focusReturnTarget = opener || null
                window.pendingDeleteItem = item
                deleteConfirm.show(
                    qsTr("Delete media item?"),
                    qsTr("This permanently deletes \"%1\" from the Emby server. This action cannot be undone.").arg(item.title || ""),
                    qsTr("Delete"))
            }
        }

        MetadataEditorDialog {
            id: metadataEditor
            viewModel: app.metadataEditor
            onValidationError: message => errorDialog.show(message)
            onActionFailure: (message, nonModal, handledInPlace) =>
                window.reportMediaActionFailure(
                    message, nonModal, handledInPlace)
        }

        ImageEditorDialog {
            id: imageEditor
            viewModel: app.imageEditor
            onActionFailure: (message, nonModal, handledInPlace) =>
                window.reportMediaActionFailure(
                    message, nonModal, handledInPlace)
        }

        RefreshMetadataDialog {
            id: refreshMetadataDialog
            onRefreshRequested: (itemId, mode, replaceImages) =>
                app.mediaActions.refreshMetadata(itemId, mode, replaceImages)
        }

        MediaTargetDialog {
            id: mediaTargetDialog
            viewModel: app.mediaTarget
            onValidationError: message => errorDialog.show(message)
            onActionFailure: (message, nonModal, handledInPlace) =>
                window.reportMediaActionFailure(
                    message, nonModal, handledInPlace)
            onAdded: itemId => window.showActionToast(
                qsTr("Added to playlist"), "success")
        }

        AppConfirmDialog {
            id: deleteConfirm
            closeOnConfirm: false
            onConfirmed: {
                const itemId = String(window.pendingDeleteItem.id || "")
                if (itemId.length > 0)
                    app.mediaActions.deleteItem(itemId)
            }
            onRejected: window.pendingDeleteItem = ({})
        }

        StatusToast {
            id: actionToast
            z: 540
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 86
            minimumWidth: Math.min(300, maximumWidth)
            maximumWidth: Math.max(260, Math.min(520, parent.width - 48))
            defaultTone: "info"
            timeout: 4200
        }

        Rectangle {
            id: controllerHint
            objectName: "activeControllerHint"
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.rightMargin: 18
            anchors.bottomMargin: 12
            z: 80
            visible: window.currentPage !== 2
                && (InputModality.modality === InputModality.Controller
                    || InputModality.modality === InputModality.Remote)
            width: Math.min(parent.width - 36, hintRow.implicitWidth + 22)
            height: 34
            radius: 17
            color: "#D9161922"
            border.width: 1
            border.color: String(InputModality.activeSupportTier) === "verified"
                ? "#5274DBA4" : Theme.outline

            Row {
                id: hintRow
                anchors.centerIn: parent
                spacing: 10

                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: InputModality.activeDeviceName
                        || qsTr("Controller")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1
                    height: 14
                    color: Theme.outline
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: InputModality.promptForAction(InputModality.Menu)
                        + "  " + qsTr("Menu")
                        + "   ·   "
                        + InputModality.promptForAction(InputModality.Back)
                        + "  " + qsTr("Back")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 10
                }
            }
        }

        TrackMenu {
            id: controllerMenu
            parent: appFrame
            x: 88
            y: Math.max(54, appFrame.height - height - 18)
            heading: qsTr("Navigate")
            selectedId: window.currentPage
            tracks: [
                { "id": 0, "label": qsTr("Home") },
                { "id": 1, "label": qsTr("Search") },
                { "id": 4, "label": qsTr("Favorites") },
                { "id": 5, "label": qsTr("About") },
                { "id": 3, "label": qsTr("Settings") },
                { "id": 100, "label": qsTr("Minimize window") },
                { "id": 101,
                  "label": window.visibility === Window.Maximized
                      ? qsTr("Restore window") : qsTr("Maximize window") },
                { "id": 102, "label": qsTr("Close Yanami") }
            ]
            onTrackSelected: action => {
                if (action >= 0 && action <= 5) {
                    window.selectControllerPage(action)
                } else if (action === 100) {
                    window.showMinimized()
                } else if (action === 101) {
                    if (window.visibility === Window.Maximized)
                        window.showNormal()
                    else
                        window.showMaximized()
                } else if (action === 102) {
                    window.close()
                }
            }
        }
    }

    Connections {
        target: InputModality

        function onModalityChanged() {
            if (InputModality.controllerInputTestActive)
                return
            if (InputModality.focusNavigationActive
                    && window.currentPage !== 2) {
                if (focusNavigator.isFocusable(window.activeFocusItem))
                    return
                if (!window.focusCurrentPage(false))
                    Qt.callLater(function() { window.focusCurrentPage(false) })
            }
        }

        function onActionPressed(action, repeated) {
            if (InputModality.controllerInputTestActive
                    || window.currentPage === 2
                    || PopupCoordinator.hasOpenPopup)
                return
            if (action === InputModality.Menu && !repeated) {
                window.openControllerMenu()
            } else if (action === InputModality.Search && !repeated) {
                window.selectControllerPage(1)
                Qt.callLater(function() {
                    if (window.searchPage)
                        window.searchPage.focusSearch()
                })
            } else if (action === InputModality.PagePrevious && !repeated) {
                window.switchControllerPage(-1)
            } else if (action === InputModality.PageNext && !repeated) {
                window.switchControllerPage(1)
            } else if (action === InputModality.PageUp) {
                focusNavigator.scroll(window.activePageItem(), 0, -1, true)
            } else if (action === InputModality.PageDown) {
                focusNavigator.scroll(window.activePageItem(), 0, 1, true)
            } else if (action === InputModality.ScrollUp) {
                focusNavigator.scroll(window.activePageItem(), 0, -1, false)
            } else if (action === InputModality.ScrollDown) {
                focusNavigator.scroll(window.activePageItem(), 0, 1, false)
            } else if (action === InputModality.ScrollLeft) {
                focusNavigator.scroll(window.activePageItem(), -1, 0, false)
            } else if (action === InputModality.ScrollRight) {
                focusNavigator.scroll(window.activePageItem(), 1, 0, false)
            }
        }
    }

    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: PopupCoordinator.dismissTopOrNavigate(function() {
            window.navigateBack()
        })
    }

    Shortcut {
        sequence: "F11"
        context: Qt.ApplicationShortcut
        enabled: window.currentPage === 2
            && (!window.playerPage || !window.playerPage.playbackEndVisible)
        onActivated: window.toggleFullScreen()
    }

    MediaActionHost {
        hostWindow: window
        refreshMetadataDialog: refreshMetadataDialog
        homePage: homePage
        deleteConfirm: deleteConfirm
    }

    PlaybackHost {
        hostWindow: window
    }

    ApplicationLifecycleHost {
        hostWindow: window
    }

    Timer {
        interval: 300
        running: window.developmentLocalMediaUrl.toString().length > 0
        onTriggered: {
            window.ensurePlayerPage()
            const page = window.playerPage
            page.mediaTitle = "Danmaku visibility regression"
            page.currentItemId = "development-local"
            window.currentPage = 2
            Qt.callLater(function() {
                page.mediaUrl = window.developmentLocalMediaUrl
            })
        }
    }

    Timer {
        id: developmentCollectionTimer
        interval: 180
        repeat: true
        onTriggered: {
            const parts = window.developmentCollectionSequence.split(",")
            if (window.developmentCollectionStep >= parts.length) {
                stop()
                return
            }
            const libraryViewsModel = app.home.mediaStore.libraryViewsModel
            if (libraryViewsModel.count === 0)
                return
            if (window.developmentCollectionStep === 0
                    && app.home.libraryRefreshing)
                return
            const index = Number(parts[window.developmentCollectionStep])
            if (!Number.isInteger(index) || index < 0 || index >= libraryViewsModel.count) {
                stop()
                return
            }
            homePage.openLibraryView(libraryViewsModel.get(index))
            ++window.developmentCollectionStep
            if (window.developmentScrollRegression)
                developmentScrollRegressionTimer.restart()
            if (window.developmentCollectionStep >= parts.length)
                stop()
            else if (window.developmentScrollRegression)
                developmentCollectionTimer.stop()
        }
    }

    onDevelopmentCollectionSequenceChanged: {
        window.developmentCollectionStep = 0
        if (developmentCollectionSequence.length > 0)
            developmentCollectionTimer.restart()
    }

    Timer {
        id: developmentScrollRegressionTimer
        interval: 900
        onTriggered: {
            homePage.runDevelopmentScrollRegression()
            if (window.developmentCollectionStep
                    < window.developmentCollectionSequence.split(",").length)
                developmentCollectionTimer.restart()
        }
    }

    Timer {
        id: developmentMediaMenuTimer
        interval: 250
        repeat: true
        running: window.developmentMediaMenuPreview.length > 0
        onTriggered: {
            if (window.tryDevelopmentMediaMenuPreview())
                stop()
        }
    }

    Timer {
        id: developmentStopTimer
        interval: Math.max(250, window.developmentAutoStopMs)
        onTriggered: {
            const page = window.playerPage
            if (page)
                page.closeRequested()
        }
    }

    onClosing: {
        const page = window.playerPage
        if (window.currentPage === 2 && page)
            page.closePlayback()
    }

    Component.onCompleted: Qt.callLater(window.showBackendError)
}
