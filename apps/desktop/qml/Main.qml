import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami
import Yanami.Ui

ApplicationWindow {
    id: window
    property bool bootstrapHandoffPending: bootstrapHandoffRequested
    property bool bootstrapHandoffReady: false
    property bool bootstrapHandoffTransitionVisible: bootstrapHandoffRequested

    width: 1240
    height: 800
    minimumWidth: 900
    minimumHeight: 620
    opacity: bootstrapHandoffPending ? 0.0 : 1.0
    visible: true
    title: "Yanami"
    color: bootstrapHandoffTransitionVisible ? "#080D17" : Theme.background
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
    readonly property var playerPage: playerLoader.item
    readonly property var searchPage: searchLoader.item
    readonly property var favoritesPage: favoritesLoader.item
    readonly property var settingsPage: settingsLoader.item
    readonly property var homePage: homeLoader.item
    property var pendingHomeRoute: null
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

    function invokeObject(target, methodName, args) {
        if (target && typeof target[methodName] === "function")
            return target[methodName].apply(target, args || [])
        return null
    }

    function invokeHome(methodName, args, queueIfLoading) {
        const page = window.homePage
        if (page)
            return window.invokeObject(page, methodName, args)
        if (queueIfLoading === true && app.session.connected) {
            window.pendingHomeRoute = {
                methodName: methodName,
                args: args || []
            }
        }
        return null
    }

    function ensureDeferredDialog(loader, viewModel, opener) {
        loader.active = true
        const dialog = loader.item
        if (!dialog)
            return null
        if (viewModel)
            dialog.viewModel = viewModel
        dialog.focusReturnTarget = opener || null
        return dialog
    }

    function metadataDialog(opener) {
        return window.ensureDeferredDialog(
            metadataEditorLoader, app.metadataEditor, opener)
    }

    function imageDialog(opener) {
        return window.ensureDeferredDialog(
            imageEditorLoader, app.imageEditor, opener)
    }

    function refreshDialog(opener) {
        return window.ensureDeferredDialog(
            refreshMetadataLoader, null, opener)
    }

    function targetDialog(opener) {
        return window.ensureDeferredDialog(
            mediaTargetLoader, app.mediaTarget, opener)
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
            if (window.homePage && window.homePage.depth > 0)
                window.homePage.goBack()
        } else {
            window.currentPage = 0
        }
    }

    function activePageItem() {
        if (window.currentPage === 0)
            return window.homePage || homeHost
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
        if (destination === 0 && window.homePage)
            window.homePage.goHome()
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
        if (!window.targetDialog(opener))
            return false
        return app.mediaTarget.load(item)
    }

    function openContextItem(item) {
        const type = String(item.itemType || "")
        const previousPage = window.currentPage
        if (type === "CollectionFolder" || type === "UserView" || type === "Folder"
                || type === "AggregateFolder") {
            window.currentPage = 0
            window.invokeHome("openLibraryView", [item], true)
        } else if (type === "Playlist") {
            window.currentPage = 0
            if (previousPage !== 0)
                window.invokeHome(
                    "openExternalItem", [item, previousPage], true)
            else
                window.invokeHome("openLibraryItem", [item], true)
        } else if (type === "Series") {
            const cameFromExternalPage = previousPage !== 0
            window.currentPage = 0
            if (cameFromExternalPage)
                window.invokeHome(
                    "openExternalItem", [item, previousPage], true)
            else
                window.invokeHome("openLibraryItem", [item], true)
        } else if (type === "Season") {
            window.currentPage = 0
            if (previousPage !== 0)
                window.invokeHome(
                    "openExternalSeason", [item, previousPage], true)
            else
                window.invokeHome("openSeason", [item], true)
        } else if (type === "Episode" || type === "Movie"
                   || type === "Video" || type === "MusicVideo") {
            window.requestPlayback(
                item.id, false, item.playbackContext || ({}), item.title)
        } else {
            window.currentPage = 0
            if (previousPage !== 0)
                window.invokeHome(
                    "openExternalItem", [item, previousPage], true)
            else
                window.invokeHome("openLibraryItem", [item], true)
        }
    }

    function tryDevelopmentMediaMenuPreview() {
        if (window.developmentMediaMenuPreview === "context"
                || window.developmentMediaMenuPreview === "admin-context")
            return Boolean(window.invokeHome(
                "openDevelopmentContextPreview", [], false))
        if (window.developmentMediaMenuPreview === "admin-library-context")
            return Boolean(window.invokeHome(
                "openDevelopmentLibraryContextPreview", [], false))
        if (window.developmentMediaMenuPreview === "metadata"
                || window.developmentMediaMenuPreview === "metadata-ids") {
            const item = window.invokeHome("developmentPreviewItem", [], false)
            if (!item || !item.id || !app.session.administrator)
                return false
            const editor = window.metadataDialog(null)
            if (!editor)
                return false
            editor.beginLoading(item)
            return true
        }
        if (window.developmentMediaMenuPreview === "metadata-mock"
                || window.developmentMediaMenuPreview === "metadata-ids-mock") {
            const editor = window.metadataDialog(null)
            if (!editor)
                return false
            editor.openFor({
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
                editor.scrollToExternalIdentifiers()
            return true
        }
        if (window.developmentMediaMenuPreview === "images") {
            const item = window.invokeHome("developmentPreviewItem", [], false)
            if (!item || !item.id || !app.session.administrator)
                return false
            const editor = window.imageDialog(null)
            if (!editor)
                return false
            editor.beginLoading(item)
            return true
        }
        if (window.developmentMediaMenuPreview === "images-mock"
                || window.developmentMediaMenuPreview === "image-search-mock") {
            const previewUrl = Qt.resolvedUrl(
                "qrc:/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")
            const editor = window.imageDialog(null)
            if (!editor)
                return false
            editor.openFor({
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
                editor.showSearchPreview({
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
            const item = window.invokeHome("developmentPreviewItem", [], false)
            if (!item)
                return false
            const dialog = window.refreshDialog(null)
            if (!dialog)
                return false
            dialog.openFor(item)
            return true
        }
        if (window.developmentMediaMenuPreview === "playlist-targets") {
            const item = window.invokeHome("developmentPreviewItem", [], false)
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
                            window.invokeHome("goHome", [], false)
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

                Item {
                    id: homeHost
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    function controllerDefaultFocusItem() {
                        if (!app.session.connected)
                            return disconnectedSettingsButton
                        return window.invokeHome(
                            "controllerDefaultFocusItem", [], false)
                    }

                    Loader {
                        id: homeLoader
                        anchors.fill: parent
                        active: app.session.connected
                        asynchronous: true
                        source: Qt.resolvedUrl("pages/HomePage.qml")

                        onLoaded: {
                            const pending = window.pendingHomeRoute
                            window.pendingHomeRoute = null
                            if (pending)
                                window.invokeHome(
                                    pending.methodName, pending.args, false)
                            if (window.currentPage === 0
                                    && InputModality.focusNavigationActive) {
                                Qt.callLater(function() {
                                    window.focusCurrentPage(false)
                                })
                            }
                        }
                        onActiveChanged: if (!active) window.pendingHomeRoute = null
                    }

                    Binding {
                        target: homeLoader.item
                        property: "collectionParent"
                        value: app.home.collectionParent
                        when: homeLoader.status === Loader.Ready
                    }
                    Binding {
                        target: homeLoader.item
                        property: "developmentLibraryScanProgress"
                        value: window.developmentLibraryScanProgress
                        when: homeLoader.status === Loader.Ready
                    }

                    Connections {
                        target: homeLoader.item
                        ignoreUnknownSignals: true

                        function onPlayRequested(itemId, title, playbackContext) {
                            window.requestPlayback(
                                itemId, false, playbackContext, title)
                        }
                        function onMediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation) {
                            mediaContextMenu.openFor(
                                item, sourceItem, x, y, keyboardInvocation)
                        }
                        function onSettingsRequested() {
                            window.currentPage = 3
                        }
                        function onExternalReturnRequested(page) {
                            window.currentPage = page
                        }
                        function onControllerFocusRequested() {
                            Qt.callLater(function() {
                                window.focusCurrentPage(true)
                            })
                        }
                    }

                    LoadingIndicator {
                        anchors.centerIn: parent
                        visible: app.session.connected
                            && homeLoader.status === Loader.Loading
                    }

                    Item {
                        id: disconnectedHome
                        anchors.fill: parent
                        visible: !app.session.connected

                        GlassPanel {
                            anchors.centerIn: parent
                            width: Math.min(disconnectedHome.width - 48, 620)
                            height: disconnectedContent.implicitHeight + 64
                            radius: Theme.radiusLarge
                            color: Theme.surfaceStrong

                            ColumnLayout {
                                id: disconnectedContent
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.margins: 32
                                spacing: 18

                                Rectangle {
                                    Layout.alignment: Qt.AlignHCenter
                                    Layout.preferredWidth: 64
                                    Layout.preferredHeight: 64
                                    radius: 21
                                    color: Theme.accentSoft
                                    border.width: 1
                                    border.color: "#52FF6687"

                                    Text {
                                        anchors.centerIn: parent
                                        text: "E"
                                        color: Theme.accent
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 23
                                        font.weight: Font.Bold
                                    }
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Connect your Emby server")
                                    color: Theme.text
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 26
                                    font.weight: Font.DemiBold
                                    horizontalAlignment: Text.AlignHCenter
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: qsTr("Set up Emby in Settings to start browsing your media library.")
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 14
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.WordWrap
                                }

                                AppButton {
                                    id: disconnectedSettingsButton
                                    Layout.alignment: Qt.AlignHCenter
                                    kind: "primary"
                                    iconName: "settings"
                                    text: qsTr("Go to Settings")
                                    onClicked: window.currentPage = 3
                                }
                            }
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Loader {
                        id: searchLoader
                        anchors.fill: parent
                        active: window.searchLoaded
                        asynchronous: true
                        source: Qt.resolvedUrl("pages/SearchPage.qml")
                        onLoaded: {
                            item.developmentDiagnostics = Qt.binding(function() {
                                return window.developmentSearchQuery.length > 0
                            })
                            item.controllerExitTarget = searchNavButton
                            if (window.developmentSearchQuery.length > 0)
                                item.query = window.developmentSearchQuery
                            if (window.currentPage === 1)
                                window.invokeObject(item, "focusSearch", [])
                        }
                    }

                    Connections {
                        target: searchLoader.item
                        ignoreUnknownSignals: true
                        function onItemRequested(item) {
                            window.openContextItem(item)
                        }
                        function onPlayRequested(
                                itemId, title, playbackContext) {
                            window.requestPlayback(
                                itemId, false, playbackContext, title)
                        }
                        function onMediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation) {
                            mediaContextMenu.openFor(
                                item, sourceItem, x, y,
                                keyboardInvocation)
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
                    source: Qt.resolvedUrl("pages/PlayerPage.qml")

                    onLoaded: {
                        item.globalToastBottom = Qt.binding(function() {
                            return actionToast.visible
                                ? actionToast.y + actionToast.height : 0
                        })
                        item.developmentSeekSeconds = Qt.binding(function() {
                            return window.developmentSeekSeconds
                        })
                        item.developmentAutoSkipIntro = Qt.binding(function() {
                            return window.developmentAutoSkipIntro
                        })
                        item.developmentRenderDiagnostics = Qt.binding(function() {
                            return window.developmentRenderDiagnostics
                        })
                        item.developmentDanmakuPreview = Qt.binding(function() {
                            return window.developmentDanmakuPreview
                        })
                        item.developmentPlaybackQueuePreview = Qt.binding(function() {
                            return window.developmentPlaybackQueuePreview
                        })
                        item.developmentDanmakuSearchQuery = Qt.binding(function() {
                            return window.developmentDanmakuSearchQuery
                        })
                        item.developmentDanmakuPreviewFontSize = Qt.binding(function() {
                            return window.developmentDanmakuPreviewFontSize
                        })
                        item.developmentDisableDanmakuAfterLoad = Qt.binding(function() {
                            return window.developmentDisableDanmakuAfterLoad
                        })
                        item.developmentReenableDanmakuAfterDisable = Qt.binding(function() {
                            return window.developmentReenableDanmakuAfterDisable
                        })
                        item.developmentSyntheticDanmaku = Qt.binding(function() {
                            return window.developmentLocalMediaUrl.toString().length > 0
                        })
                        item.developmentDanmakuStyleStressCount = Qt.binding(function() {
                            return window.developmentDanmakuStyleStressCount
                        })
                        item.developmentDanmakuToggleStressCount = Qt.binding(function() {
                            return window.developmentDanmakuToggleStressCount
                        })
                        item.loadingPreview = Qt.binding(function() {
                            return window.developmentLoadingPreview
                        })
                        item.windowFullScreen = Qt.binding(function() {
                            return window.fullScreenMode
                        })
                    }
                }

                Connections {
                    target: playerLoader.item
                    ignoreUnknownSignals: true
                    function onPlaybackLoaded() {
                        if (window.developmentAutoStopMs > 0)
                            developmentStopTimer.restart()
                    }
                    function onCloseRequested() { window.closePlayer() }
                    function onToggleFullScreenRequested() {
                        window.toggleFullScreen()
                    }
                    function onEpisodeSwitchRequested(
                            itemId, playbackContext, positionSeconds, paused) {
                        app.playback.switchToInContext(
                            itemId, playbackContext, positionSeconds, paused)
                    }
                    function onReplayRequested(
                            itemId, playbackContext, title) {
                        window.requestPlayback(
                            itemId, true, playbackContext, title)
                    }
                    function onQueueRefreshRequested(itemId, playbackContext) {
                        app.playback.prepareInContext(
                            itemId, playbackContext || ({}))
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
                        source: Qt.resolvedUrl("pages/SettingsPage.qml")
                        onLoaded: {
                            item.pageActive = Qt.binding(function() {
                                return window.currentPage === 3
                            })
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
                        source: Qt.resolvedUrl("pages/FavoritesPage.qml")
                        onLoaded: {
                            item.refreshing = Qt.binding(function() {
                                return app.favorites.refreshing
                            })
                            item.loadFailed = Qt.binding(function() {
                                return app.favorites.loadFailed
                            })
                            if (window.currentPage === 4
                                    && InputModality.focusNavigationActive) {
                                Qt.callLater(function() {
                                    window.focusCurrentPage(false)
                                })
                            }
                        }
                    }

                    Connections {
                        target: favoritesLoader.item
                        ignoreUnknownSignals: true
                        function onItemRequested(item) {
                            window.openContextItem(item)
                        }
                        function onPlayRequested(itemId, title) {
                            window.requestPlayback(itemId, false, ({}), title)
                        }
                        function onMediaContextRequested(
                                item, sourceItem, x, y, keyboardInvocation) {
                            mediaContextMenu.openFor(
                                item, sourceItem, x, y,
                                keyboardInvocation)
                        }
                        function onRetryRequested() {
                            app.favorites.refresh()
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
                        source: Qt.resolvedUrl("pages/AboutPage.qml")
                        onLoaded: {
                            if (window.currentPage === 5
                                    && InputModality.focusNavigationActive) {
                                Qt.callLater(function() {
                                    window.focusCurrentPage(false)
                                })
                            }
                        }
                    }

                    Connections {
                        target: aboutLoader.item
                        ignoreUnknownSignals: true
                        function onFeedbackRequested(message, tone) {
                            window.showActionToast(message, tone)
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
                const editor = window.metadataDialog(opener)
                if (editor)
                    editor.beginLoading(item)
            }
            onImageEditRequested: (item, opener) => {
                const editor = window.imageDialog(opener)
                if (editor)
                    editor.beginLoading(item)
            }
            onLibraryScanRequested: item =>
                app.mediaActions.scanLibraryFiles(item.id)
            onMetadataRefreshRequested: (item, opener) => {
                const dialog = window.refreshDialog(opener)
                if (dialog)
                    dialog.openFor(item)
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

        Loader {
            id: metadataEditorLoader
            active: false
            source: Qt.resolvedUrl("components/MetadataEditorDialog.qml")
        }
        Connections {
            target: metadataEditorLoader.item
            ignoreUnknownSignals: true
            function onValidationError(message) {
                errorDialog.show(message)
            }
            function onActionFailure(message, nonModal, handledInPlace) {
                window.reportMediaActionFailure(
                    message, nonModal, handledInPlace)
            }
        }

        Loader {
            id: imageEditorLoader
            active: false
            source: Qt.resolvedUrl("components/ImageEditorDialog.qml")
        }
        Connections {
            target: imageEditorLoader.item
            ignoreUnknownSignals: true
            function onActionFailure(message, nonModal, handledInPlace) {
                window.reportMediaActionFailure(
                    message, nonModal, handledInPlace)
            }
        }

        Loader {
            id: refreshMetadataLoader
            active: false
            source: Qt.resolvedUrl("components/RefreshMetadataDialog.qml")
        }
        Connections {
            target: refreshMetadataLoader.item
            ignoreUnknownSignals: true
            function onRefreshRequested(itemId, mode, replaceImages) {
                app.mediaActions.refreshMetadata(
                    itemId, mode, replaceImages)
            }
        }

        Loader {
            id: mediaTargetLoader
            active: false
            source: Qt.resolvedUrl("components/MediaTargetDialog.qml")
        }
        Connections {
            target: mediaTargetLoader.item
            ignoreUnknownSignals: true
            function onValidationError(message) {
                errorDialog.show(message)
            }
            function onActionFailure(message, nonModal, handledInPlace) {
                window.reportMediaActionFailure(
                    message, nonModal, handledInPlace)
            }
            function onAdded(itemId) {
                window.showActionToast(
                    qsTr("Added to playlist"), "success")
            }
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
        refreshMetadataDialog: refreshMetadataLoader.item
        homePage: window.homePage
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
            window.invokeHome(
                "openLibraryView", [libraryViewsModel.get(index)], true)
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
            window.invokeHome(
                "runDevelopmentScrollRegression", [], false)
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

    Loader {
        id: bootstrapTransitionLoader
        anchors.fill: parent
        active: window.bootstrapHandoffTransitionVisible
        z: 10000

        sourceComponent: Component {
            FocusScope {
                id: bootstrapTransition
                objectName: "bootstrapHandoffTransition"
                anchors.fill: parent
                focus: true
                opacity: 1.0

                Keys.onPressed: event => { event.accepted = true }
                Keys.onReleased: event => { event.accepted = true }

                Rectangle {
                    anchors.fill: parent
                    color: "#080D17"
                }

                Item {
                    id: bootstrapBrand
                    anchors.centerIn: parent
                    width: 540
                    height: 320

                    Image {
                        id: bootstrapLogo
                        objectName: "bootstrapHandoffLogo"
                        x: 214
                        y: 38
                        width: 112
                        height: 112
                        source: Qt.resolvedUrl(
                            "qrc:/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")
                        fillMode: Image.PreserveAspectFit
                        mipmap: true
                        smooth: true
                    }

                    Rectangle {
                        x: bootstrapLogo.x
                        y: bootstrapLogo.y
                        width: bootstrapLogo.width
                        height: bootstrapLogo.height
                        radius: 20
                        color: "transparent"
                        border.width: 1
                        border.color: "#1B2534"
                    }

                    Text {
                        objectName: "bootstrapHandoffTitle"
                        x: 0
                        y: 168
                        width: parent.width
                        height: 40
                        text: "Yanami"
                        color: "#F5F7FA"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        renderType: Text.NativeRendering
                        font.family: Qt.platform.os === "windows"
                            ? "Segoe UI" : Theme.fontForText(text)
                        font.pixelSize: 30
                        font.weight: Font.DemiBold
                    }

                    Text {
                        objectName: "bootstrapHandoffStatus"
                        x: 48
                        y: 216
                        width: parent.width - 96
                        height: 28
                        text: qsTr("Starting Yanami…")
                        color: "#9AA3B2"
                        elide: Text.ElideRight
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        renderType: Text.NativeRendering
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 16
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.AllButtons
                    hoverEnabled: true
                }

                Timer {
                    id: bootstrapHandoffHold
                    interval: 200
                    running: window.bootstrapHandoffReady
                    onTriggered: bootstrapHandoffFade.start()
                }

                NumberAnimation {
                    id: bootstrapHandoffFade
                    target: bootstrapTransition
                    property: "opacity"
                    from: 1.0
                    to: 0.0
                    duration: 180
                    easing.type: Easing.OutCubic
                    onFinished: {
                        window.bootstrapHandoffTransitionVisible = false
                        if (InputModality.focusNavigationActive)
                            Qt.callLater(function() {
                                window.focusCurrentPage(false)
                            })
                    }
                }
            }
        }
    }

    onClosing: {
        const page = window.playerPage
        if (window.currentPage === 2 && page)
            page.closePlayback()
    }

    Component.onCompleted: Qt.callLater(window.showBackendError)
}
