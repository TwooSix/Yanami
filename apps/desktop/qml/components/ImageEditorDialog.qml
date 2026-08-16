import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Dialogs
import QtQuick.Layouts
import Yanami.Ui

AppModalPopup {
    id: root

    property var viewModel: null
    property var editorData: ({})
    property var remoteImages: []
    property string currentItemId: ""
    // Mirrors the image type selected by the active card/search operation.
    property string currentImageType: "Primary"
    property string searchProvider: ""
    property bool includeAllLanguages: false
    property bool enableSeriesImages: false
    property bool searchMode: false
    property bool loading: false
    property bool loaded: false
    property bool providersLoading: false
    property string providersError: ""
    property bool working: false
    property string workingKind: ""
    property bool reconciling: false
    property string lastFailedKind: ""
    property string inlineError: ""
    property var pendingContext: ({})
    property var pendingMutation: ({})
    property var pendingDeleteImage: ({})
    property string expandedImageType: ""
    property int viewGeneration: 0
    property int activeViewGeneration: 0
    property int searchGeneration: 0
    property int activeSearchGeneration: 0
    property var initialRequestContext: ({})
    property var searchRequestContext: ({})
    property var mutationRequestContext: ({})
    property double intentStartedAt: 0
    property bool firstPreviewReported: false
    property bool modelApplyReported: false
    property var ordinaryImageTypes: [
        "Primary", "Logo", "Thumb", "Banner", "Disc", "Art"
    ]

    readonly property real overviewContentWidth: Math.max(0, width - 48)
    readonly property int artworkColumns: overviewContentWidth >= 1080 ? 6
        : overviewContentWidth >= 900 ? 5
        : overviewContentWidth >= 720 ? 4
        : overviewContentWidth >= 500 ? 3 : 2
    readonly property real artworkCardWidth: Math.floor(
        (overviewContentWidth - (artworkColumns - 1) * 12) / artworkColumns)
    readonly property int expandedColumns: overviewContentWidth >= 900 ? 4
        : overviewContentWidth >= 650 ? 3 : 2
    readonly property real expandedCardWidth: Math.floor(
        (overviewContentWidth - (expandedColumns - 1) * 12) / expandedColumns)
    readonly property int backdropColumns: overviewContentWidth >= 900 ? 3
        : overviewContentWidth >= 600 ? 2 : 1
    readonly property real backdropCardWidth: Math.floor(
        (overviewContentWidth - (backdropColumns - 1) * 12) / backdropColumns)

    signal focusCardRequested(string imageType, var imageIndex)
    signal actionFailure(string message, bool nonModal, bool handledInPlace)

    function imageTypeLabel(type) {
        switch (String(type || "")) {
        case "Primary": return qsTr("Primary")
        case "Backdrop": return qsTr("Backdrop")
        case "Thumb": return qsTr("Thumb")
        case "Banner": return qsTr("Banner")
        case "Logo": return qsTr("Logo")
        case "Art": return qsTr("Art")
        case "Disc": return qsTr("Disc")
        default: return String(type || "")
        }
    }

    function normalizedImageIndex(value) {
        if (value === undefined || value === null || value === "")
            return null
        const result = Number(value)
        return Number.isFinite(result) && result >= 0 ? result : null
    }

    function contextFor(type, imageIndex, mode) {
        return {
            imageType: String(type || "Primary"),
            imageIndex: root.normalizedImageIndex(imageIndex),
            mode: String(mode || "replace")
        }
    }

    function imagesForType(type) {
        let source = root.editorData.images || []
        const result = []
        const wantedType = String(type || "")
        if (root.viewModel && root.viewModel.opened && root.loaded) {
            if (wantedType === "Backdrop") {
                source = root.viewModel.backdropsModel || []
            } else {
                const slots = root.viewModel.slotsModel || []
                for (let slotIndex = 0; slotIndex < slots.length; ++slotIndex) {
                    if (String(slots[slotIndex].imageType || "") === wantedType) {
                        source = slots[slotIndex].images || []
                        break
                    }
                }
            }
        }
        for (let index = 0; index < source.length; ++index) {
            if (String(source[index].imageType || "") === wantedType)
                result.push(source[index])
        }
        result.sort(function(left, right) {
            const leftIndex = root.normalizedImageIndex(left.imageIndex)
            const rightIndex = root.normalizedImageIndex(right.imageIndex)
            // The unindexed image is Emby's canonical slot. Keep it as the
            // representative when a server unexpectedly exposes more than one
            // image for a normally single-image type.
            return Number(leftIndex === null ? -1 : leftIndex)
                - Number(rightIndex === null ? -1 : rightIndex)
        })
        return result
    }

    function resolutionText(image) {
        const value = image || ({})
        const width = Number(value.width || 0)
        const height = Number(value.height || 0)
        return width > 0 && height > 0 ? width + " × " + height : ""
    }

    function providersForType(type) {
        const source = root.editorData.providers || []
        const result = [{ name: "", label: qsTr("All sources") }]
        const wantedType = String(type || root.currentImageType)
        for (let index = 0; index < source.length; ++index) {
            const types = source[index].supportedImages || []
            if (types.indexOf(wantedType) >= 0) {
                result.push({
                    name: String(source[index].name || ""),
                    label: String(source[index].name || "")
                })
            }
        }
        return result
    }

    function hasProviderForType(type) {
        return root.providersForType(type).length > 1
    }

    function nextBackdropIndex() {
        if (root.viewModel && root.viewModel.opened && root.loaded)
            return Number(root.viewModel.nextBackdropIndex || 0)
        const images = root.imagesForType("Backdrop")
        const used = ({})
        for (let index = 0; index < images.length; ++index) {
            const imageIndex = root.normalizedImageIndex(images[index].imageIndex)
            // A few Emby-compatible servers omit the first backdrop index.
            // Treat its list position as occupied so Add never overwrites it.
            used[String(imageIndex === null ? index : imageIndex)] = true
        }
        let next = 0
        while (used[String(next)] === true)
            ++next
        return next
    }

    function beginLoading(item) {
        const value = item || ({})
        root.intentStartedAt = Date.now()
        root.firstPreviewReported = false
        root.modelApplyReported = false
        root.initialRequestContext = root.viewModel
            ? root.viewModel.open(value) : ({})
        if (Number(root.initialRequestContext.viewGeneration || 0) > 0) {
            root.viewGeneration = Number(root.initialRequestContext.viewGeneration)
        } else {
            ++root.viewGeneration
        }
        root.activeViewGeneration = root.viewGeneration
        root.currentItemId = String(value.id || "")
        root.currentImageType = "Primary"
        root.editorData = {
            id: root.currentItemId,
            title: String(value.title || value.name || ""),
            itemType: String(value.itemType || ""),
            imageTypes: root.ordinaryImageTypes.concat(["Backdrop"]),
            images: [],
            providers: []
        }
        root.remoteImages = []
        root.pendingContext = ({})
        root.pendingMutation = ({})
        root.searchRequestContext = ({})
        root.mutationRequestContext = ({})
        root.pendingDeleteImage = ({})
        root.expandedImageType = ""
        root.searchProvider = ""
        root.includeAllLanguages = false
        root.enableSeriesImages = false
        root.searchMode = false
        root.loading = true
        root.loaded = false
        root.providersLoading = true
        root.providersError = ""
        root.working = false
        root.workingKind = ""
        root.reconciling = false
        root.lastFailedKind = ""
        root.inlineError = ""
        if (!root.opened)
            root.open()
        const generation = root.activeViewGeneration
        Qt.callLater(function() {
            if (root.opened && root.activeViewGeneration === generation) {
                console.info("ui_response",
                    "resourceKey=images:" + root.currentItemId,
                    "requestId=" + generation,
                    "metric=intent_to_shell",
                    "elapsedMs=" + (Date.now() - root.intentStartedAt))
            }
        })
        return root.activeViewGeneration
    }

    function reportFirstPreview() {
        if (root.firstPreviewReported || root.intentStartedAt <= 0)
            return
        root.firstPreviewReported = true
        console.info("ui_response",
            "resourceKey=images:" + root.currentItemId,
            "requestId=" + root.activeViewGeneration,
            "metric=first_preview",
            "elapsedMs=" + (Date.now() - root.intentStartedAt))
    }

    function responseMatches(value, generation) {
        const result = value || ({})
        const resultId = String(result.id || "")
        if (!root.opened || resultId.length === 0 || resultId !== root.currentItemId)
            return false
        if (generation !== undefined && generation !== null
                && Number(generation) !== root.activeViewGeneration)
            return false
        return true
    }

    function applyEditor(value, generation, stateAlreadyApplied) {
        if (!root.responseMatches(value, generation))
            return false
        const wasReconciling = root.reconciling
        const incoming = value || ({})
        const updated = Object.assign({}, incoming)
        // Provider discovery has its own lane and may finish before or after
        // the current-image request. Preserve an early provider response when
        // the image payload intentionally omits that independent resource.
        if (incoming.providers === undefined) {
            updated.providers = root.editorData.providers || []
        } else {
            root.providersLoading = false
            root.providersError = ""
        }
        if (stateAlreadyApplied !== true && root.viewModel
                && root.initialRequestContext.requestId
                && !root.viewModel.applyEditor(
                    updated, root.initialRequestContext)) {
            return false
        }
        root.editorData = updated
        root.loading = false
        root.loaded = true
        root.reconciling = false
        if (!wasReconciling && !root.modelApplyReported
                && root.intentStartedAt > 0) {
            root.modelApplyReported = true
            console.info("ui_response",
                "resourceKey=images:" + root.currentItemId,
                "requestId=" + root.activeViewGeneration,
                "metric=modelApply",
                "elapsedMs=" + (Date.now() - root.intentStartedAt))
        }
        // A background authoritative refresh must not throw the user out of a
        // search or disable a newer card operation that began after the
        // optimistic update was shown.
        if (wasReconciling) {
            if (stateAlreadyApplied !== true && root.viewModel
                    && root.mutationRequestContext.mutationId) {
                root.viewModel.mutationReconciled(root.mutationRequestContext)
            }
            root.mutationRequestContext = ({})
            return true
        }
        root.working = false
        root.workingKind = ""
        root.lastFailedKind = ""
        root.inlineError = ""
        root.searchMode = false
        root.remoteImages = []
        root.searchProvider = ""
        root.pendingContext = ({})
        root.pendingMutation = ({})
        root.activeSearchGeneration = 0
        return true
    }

    function applyProviders(value, generation, stateAlreadyApplied) {
        if (!root.responseMatches(value, generation))
            return false
        const updated = Object.assign({}, root.editorData)
        updated.providers = (value || ({})).providers || []
        if (stateAlreadyApplied !== true && root.viewModel && root.loaded
                && root.initialRequestContext.requestId) {
            root.viewModel.applyEditor(updated, root.initialRequestContext)
        }
        root.editorData = updated
        root.providersLoading = false
        root.providersError = ""
        return true
    }

    function providersFailed(itemId, message) {
        if (!root.opened || String(itemId || "") !== root.currentItemId)
            return false
        root.providersLoading = false
        root.providersError = String(message
            || qsTr("Online image sources are unavailable."))
        return true
    }

    function openFor(value) {
        const generation = root.beginLoading(value)
        return root.applyEditor(value, generation)
    }

    function beginRemoteSearch(type, imageIndex, mode) {
        if (!root.loaded || root.working)
            return 0
        root.pendingContext = root.contextFor(type, imageIndex, mode)
        if (root.viewModel) {
            root.pendingContext = root.viewModel.selectTarget(
                root.pendingContext.imageType,
                root.pendingContext.imageIndex,
                root.pendingContext.mode)
        }
        root.currentImageType = root.pendingContext.imageType
        root.searchProvider = ""
        root.remoteImages = []
        root.searchMode = true
        return root.beginSearch()
    }

    function beginSearch() {
        if (!root.opened || root.currentItemId.length === 0 || !root.searchMode
                || root.working)
            return 0
        const filters = {
            imageType: root.currentImageType,
            providerName: root.searchProvider,
            includeAllLanguages: root.includeAllLanguages,
            enableSeriesImages: root.enableSeriesImages,
            startIndex: 0,
            limit: 36
        }
        root.searchRequestContext = root.viewModel
            ? root.viewModel.search(filters) : ({})
        if (Number(root.searchRequestContext.searchGeneration || 0) > 0) {
            root.searchGeneration = Number(
                root.searchRequestContext.searchGeneration)
        } else {
            ++root.searchGeneration
        }
        root.activeSearchGeneration = root.searchGeneration
        root.working = true
        root.workingKind = "search"
        root.inlineError = ""
        root.remoteImages = []
        return root.activeSearchGeneration
    }

    function returnFromSearch() {
        if (root.viewModel) {
            root.viewModel.cancelSearch()
            root.viewModel.clearPendingContext()
        }
        ++root.searchGeneration
        root.activeSearchGeneration = 0
        if (root.workingKind === "search") {
            root.working = false
            root.workingKind = ""
        }
        root.searchMode = false
        root.remoteImages = []
        root.inlineError = ""
        root.pendingContext = ({})
        root.searchRequestContext = ({})
    }

    function presentSearchResults(value, generation, stateAlreadyApplied) {
        const result = value || ({})
        const resultType = String(result.imageType || root.currentImageType)
        if (!root.opened || !root.searchMode || root.workingKind !== "search"
                || resultType !== String(root.pendingContext.imageType || ""))
            return false
        if (generation !== undefined && generation !== null
                && Number(generation) !== root.activeSearchGeneration)
            return false
        if (stateAlreadyApplied !== true && root.viewModel
                && root.searchRequestContext.requestId
                && !root.viewModel.applySearch(
                    result, root.searchRequestContext)) {
            return false
        }
        root.remoteImages = result.images || []
        root.working = false
        root.workingKind = ""
        root.inlineError = ""
        return true
    }

    function showSearchPreview(value, type) {
        if (!root.opened || !root.loaded)
            return false
        root.pendingContext = root.contextFor(type || root.currentImageType, null, "replace")
        root.currentImageType = root.pendingContext.imageType
        root.searchMode = true
        root.working = true
        root.workingKind = "search"
        return root.presentSearchResults(value)
    }

    function beginUpload(type, imageIndex, mode) {
        if (!root.loaded || root.working)
            return
        root.pendingContext = root.contextFor(type, imageIndex, mode)
        root.currentImageType = root.pendingContext.imageType
        uploadDialog.open()
    }

    function confirmDelete(image) {
        if (!root.loaded || root.working)
            return
        const value = image || ({})
        root.pendingDeleteImage = value
        root.pendingContext = root.contextFor(
            value.imageType,
            value.imageIndex,
            "delete")
        deleteConfirm.show(
            qsTr("Delete image?"),
            qsTr("This removes the selected image from the Emby server."),
            qsTr("Delete"))
    }

    function beginApplyMutation(context, image) {
        root.mutationRequestContext = root.viewModel
            ? root.viewModel.applyRemote(context || ({}), image || ({}))
            : ({})
    }

    function beginUploadMutation(context, fileUrl) {
        root.mutationRequestContext = root.viewModel
            ? root.viewModel.upload(context || ({}), String(fileUrl || ""))
            : ({})
    }

    function beginDeleteMutation(context) {
        root.mutationRequestContext = root.viewModel
            ? root.viewModel.remove(context || ({}))
            : ({})
    }

    function applyRemoteImage(image) {
        if (!root.opened || root.working)
            return
        const value = image || ({})
        const context = root.pendingContext || ({})
        root.pendingMutation = {
            kind: "apply",
            image: value,
            context: context
        }
        root.beginApplyMutation(context, value)
        root.working = true
        root.workingKind = "mutation"
        root.inlineError = ""
    }

    function mutationSucceeded(stateAlreadyApplied) {
        if (!root.opened)
            return false
        const mutation = root.pendingMutation || ({})
        const context = mutation.context || root.pendingContext || ({})
        if (stateAlreadyApplied !== true && root.viewModel
                && root.mutationRequestContext.mutationId
                && !root.viewModel.mutationSucceeded(
                    root.mutationRequestContext)) {
            return false
        }
        const source = (root.editorData.images || []).slice()
        const next = []
        let replaced = false
        const targetIndex = root.normalizedImageIndex(context.imageIndex)
        let canonicalTargetExists = false
        if (targetIndex === null) {
            for (let index = 0; index < source.length; ++index) {
                const candidate = source[index]
                if (String(candidate.imageType || "")
                        === String(context.imageType || "")
                        && root.normalizedImageIndex(candidate.imageIndex) === null) {
                    canonicalTargetExists = true
                    break
                }
            }
        }
        for (let index = 0; index < source.length; ++index) {
            const image = source[index]
            const sameType = String(image.imageType || "")
                === String(context.imageType || "")
            const imageIndex = root.normalizedImageIndex(image.imageIndex)
            const sameIndex = targetIndex === null
                ? sameType && (canonicalTargetExists
                    ? imageIndex === null : !replaced)
                : sameType && imageIndex === targetIndex
            if (!sameIndex) {
                next.push(image)
                continue
            }
            replaced = true
            if (mutation.kind === "delete")
                continue
            const remote = mutation.image || ({})
            next.push({
                imageType: String(context.imageType || image.imageType || "Primary"),
                imageIndex: context.imageIndex === undefined ? null : context.imageIndex,
                width: Number(remote.width || image.width || 0),
                height: Number(remote.height || image.height || 0),
                previewUrl: mutation.kind === "upload"
                    ? String(mutation.fileUrl || "")
                    : String(remote.previewUrl || image.previewUrl || "")
            })
        }
        if (!replaced && mutation.kind !== "delete") {
            const remote = mutation.image || ({})
            next.push({
                imageType: String(context.imageType || "Primary"),
                imageIndex: context.imageIndex === undefined ? null : context.imageIndex,
                width: Number(remote.width || 0),
                height: Number(remote.height || 0),
                previewUrl: mutation.kind === "upload"
                    ? String(mutation.fileUrl || "")
                    : String(remote.previewUrl || "")
            })
        }
        const updated = Object.assign({}, root.editorData)
        updated.images = next
        root.editorData = updated
        root.working = false
        root.workingKind = ""
        root.reconciling = true
        root.inlineError = ""
        root.pendingMutation = ({})
        root.pendingContext = ({})
        if (root.viewModel)
            root.viewModel.clearPendingContext()
        if (root.searchMode) {
            ++root.searchGeneration
            root.activeSearchGeneration = 0
            root.searchMode = false
            root.remoteImages = []
            root.searchProvider = ""
        }
        const affectedType = String(context.imageType || "Primary")
        const affectedIndex = context.imageIndex === undefined
            ? null : context.imageIndex
        Qt.callLater(function() {
            if (root.opened)
                root.focusCardRequested(affectedType, affectedIndex)
        })
        return true
    }

    function handleRequestFailure(message) {
        if (!root.opened)
            return false
        root.lastFailedKind = root.loading ? "load" : root.workingKind
        root.loading = false
        root.working = false
        root.workingKind = ""
        root.inlineError = String(message || qsTr("The image operation failed."))
        return true
    }

    function handleReconciliationFailure(message) {
        if (!root.opened)
            return false
        root.reconciling = false
        if (root.viewModel && root.mutationRequestContext.mutationId) {
            root.viewModel.clearMutationState(root.mutationRequestContext)
            root.mutationRequestContext = ({})
        }
        root.lastFailedKind = "reconcile"
        root.inlineError = String(message
            || qsTr("The latest image is shown, but server reconciliation failed."))
        return true
    }

    function retryFailure() {
        const failedKind = root.lastFailedKind
        root.lastFailedKind = ""
        root.inlineError = ""
        if (root.viewModel)
            root.viewModel.retry()
        if (failedKind === "load") {
            root.loading = true
            return
        }
        if (failedKind === "search") {
            root.beginSearch()
            return
        }
        if (failedKind === "reconcile") {
            // Reconciliation is an authoritative background check. Retrying
            // it must not lock unrelated card operations or dismissal.
            root.reconciling = true
            return
        }
        const mutation = root.pendingMutation || ({})
        const context = mutation.context || root.pendingContext || ({})
        if (failedKind !== "mutation" || !mutation.kind)
            return
        root.working = true
        root.workingKind = "mutation"
        if (mutation.kind === "apply") {
            const image = mutation.image || ({})
            root.beginApplyMutation(context, image)
        } else if (mutation.kind === "upload") {
            root.beginUploadMutation(context, mutation.fileUrl)
        } else if (mutation.kind === "delete") {
            root.beginDeleteMutation(context)
        }
    }

    Connections {
        target: root.viewModel

        function onEditorReady(editor, viewGeneration) {
            root.applyEditor(editor, viewGeneration, true)
        }
        function onProvidersReady(providers, viewGeneration) {
            root.applyProviders(providers, viewGeneration, true)
        }
        function onSearchReady(result, searchGeneration) {
            root.presentSearchResults(result, searchGeneration, true)
        }
        function onMutationCommitted(requestContext) {
            if (Number(requestContext.mutationId || 0)
                    === Number(root.mutationRequestContext.mutationId || 0)) {
                root.mutationSucceeded(true)
            }
        }
        function onEditorRequestFailed(itemId, message, nonModal) {
            const handled = root.handleRequestFailure(message)
            root.actionFailure(message, nonModal, handled)
        }
        function onProvidersRequestFailed(itemId, message, nonModal) {
            const handled = root.providersFailed(itemId, message)
            root.actionFailure(message, nonModal, handled)
        }
        function onSearchRequestFailed(itemId, message, nonModal) {
            const handled = root.handleRequestFailure(message)
            root.actionFailure(message, nonModal, handled)
        }
        function onMutationRequestFailed(itemId, message, nonModal) {
            const handled = root.handleRequestFailure(message)
            root.actionFailure(message, nonModal, handled)
        }
        function onReconciliationRequestFailed(itemId, message, nonModal) {
            const handled = root.handleReconciliationFailure(message)
            root.actionFailure(message, nonModal, handled)
        }
    }

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(1180, Math.max(720, parent.width - 64))
    height: Math.min(760, Math.max(560, parent.height - 64))
    padding: 0
    scrimColor: "#78000000"
    dismissBlocked: root.working && root.workingKind === "mutation"
    initialFocusTarget: closeButton

    onClosed: {
        if (root.viewModel)
            root.viewModel.dismiss()
        ++root.viewGeneration
        root.activeViewGeneration = 0
        ++root.searchGeneration
        root.activeSearchGeneration = 0
        root.loading = false
        root.loaded = false
        root.providersLoading = false
        root.providersError = ""
        root.working = false
        root.workingKind = ""
        root.reconciling = false
        root.searchMode = false
        root.remoteImages = []
        root.pendingContext = ({})
        root.pendingMutation = ({})
        root.initialRequestContext = ({})
        root.searchRequestContext = ({})
        root.mutationRequestContext = ({})
        root.pendingDeleteImage = ({})
        root.expandedImageType = ""
    }

    background: Rectangle {
        radius: 28
        color: "#FA161920"
        border.width: 1
        border.color: "#4AFFFFFF"
    }

    contentItem: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 86
            Layout.leftMargin: 28
            Layout.rightMargin: 24
            spacing: 16

            Column {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: 3

                Text {
                    text: qsTr("Edit images")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }

                Text {
                    width: parent.width
                    text: String(root.editorData.title || "")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
            }

            AppButton {
                id: closeButton
                Layout.preferredWidth: 38
                Layout.preferredHeight: 38
                Layout.alignment: Qt.AlignVCenter
                iconOnly: true
                iconName: "window-close"
                kind: "ghost"
                controlSize: 38
                enabled: !root.dismissBlocked
                Accessible.name: qsTr("Close")
                onClicked: root.requestDismiss("close-button")
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: "#20FFFFFF"
        }

        Rectangle {
            visible: root.inlineError.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: 24
            Layout.rightMargin: 24
            Layout.topMargin: 12
            Layout.preferredHeight: visible ? 42 : 0
            radius: 13
            color: "#25FF6687"
            border.width: 1
            border.color: "#55FF6687"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 14
                anchors.rightMargin: 8
                spacing: 8

            Text {
                Layout.fillWidth: true
                verticalAlignment: Text.AlignVCenter
                text: root.inlineError
                color: "#FFFFF7"
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                elide: Text.ElideRight
            }

            AppButton {
                visible: root.lastFailedKind.length > 0
                kind: "ghost"
                text: qsTr("Retry")
                controlSize: 30
                onClicked: root.retryFailure()
            }
            }
        }

        Flickable {
            id: overviewScroll
            visible: !root.searchMode
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: width
            contentHeight: overviewColumn.implicitHeight + 36
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: AppScrollBar {
                policy: ScrollBar.AlwaysOn
                visible: overviewScroll.contentHeight > overviewScroll.height + 1
            }

            Column {
                id: overviewColumn
                x: 24
                y: 20
                width: Math.max(0, overviewScroll.width - 48)
                spacing: 14

                Text {
                    width: parent.width
                    text: qsTr("Current images")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }

                GridLayout {
                    id: artworkGrid
                    width: parent.width
                    columns: root.artworkColumns
                    columnSpacing: 12
                    rowSpacing: 12

                    Repeater {
                        model: root.ordinaryImageTypes

                        delegate: Rectangle {
                            id: artworkCard
                            required property var modelData
                            property string imageType: String(modelData)
                            property var images: root.imagesForType(imageType)
                            property var primaryImage: images.length > 0 ? images[0] : ({})
                            property bool hasImage: images.length > 0
                            property var targetImageIndex: hasImage
                                ? root.normalizedImageIndex(primaryImage.imageIndex) : null

                            Layout.fillWidth: true
                            Layout.preferredWidth: root.artworkCardWidth
                            Layout.preferredHeight: 238
                            activeFocusOnTab: true
                            radius: 18
                            color: "#0DFFFFFF"
                            border.width: 1
                            border.color: artworkCard.activeFocus
                                ? Theme.accent : "#24FFFFFF"
                            clip: true

                            Connections {
                                target: root

                                function onFocusCardRequested(imageType, imageIndex) {
                                    if (String(imageType) === artworkCard.imageType)
                                        artworkCard.forceActiveFocus(Qt.PopupFocusReason)
                                }
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 10
                                height: parent.height - 72
                                radius: 12
                                color: "#1AFFFFFF"
                                clip: true

                                LoadingPlaceholder {
                                    anchors.fill: parent
                                    running: root.loading
                                    visible: root.loading
                                    cornerRadius: 12
                                }

                                RoundedImage {
                                    anchors.fill: parent
                                    visible: !root.loading && artworkCard.hasImage
                                    source: artworkCard.primaryImage.previewUrl || ""
                                    radius: 12
                                    asynchronous: true
                                    fillMode: Image.PreserveAspectFit
                                    onStatusChanged: if (status === Image.Ready)
                                        root.reportFirstPreview()
                                }

                                Text {
                                    anchors.centerIn: parent
                                    visible: !root.loading && !artworkCard.hasImage
                                    text: root.imageTypeLabel(artworkCard.imageType)
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                }

                                Rectangle {
                                    visible: !root.loading && artworkCard.images.length > 1
                                    anchors.top: parent.top
                                    anchors.right: parent.right
                                    anchors.margins: 8
                                    width: countText.implicitWidth + 18
                                    height: 28
                                    radius: 14
                                    color: root.expandedImageType === artworkCard.imageType
                                        ? Theme.accent : "#D5222630"
                                    border.width: 1
                                    border.color: "#45FFFFFF"

                                    Text {
                                        id: countText
                                        anchors.centerIn: parent
                                        text: artworkCard.images.length
                                        color: Theme.text
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 12
                                        font.weight: Font.DemiBold
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            root.expandedImageType = root.expandedImageType
                                                    === artworkCard.imageType
                                                ? "" : artworkCard.imageType
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 62
                                color: "#B013161D"

                                Column {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 12
                                    anchors.right: cardActions.left
                                    anchors.rightMargin: 4
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Text {
                                        width: parent.width
                                        text: root.imageTypeLabel(artworkCard.imageType)
                                        color: Theme.text
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        width: parent.width
                                        text: artworkCard.hasImage
                                            ? root.resolutionText(artworkCard.primaryImage) : ""
                                        color: Theme.textMuted
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }

                                Row {
                                    id: cardActions
                                    anchors.right: parent.right
                                    anchors.rightMargin: 5
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1

                                    AppButton {
                                        iconOnly: true
                                        iconName: "search"
                                        kind: "ghost"
                                        controlSize: 30
                                        enabled: root.loaded && !root.working
                                            && root.hasProviderForType(artworkCard.imageType)
                                        Accessible.name: qsTr("Search online")
                                        onClicked: root.beginRemoteSearch(
                                            artworkCard.imageType,
                                            artworkCard.targetImageIndex,
                                            artworkCard.hasImage ? "replace" : "add")
                                    }

                                    AppButton {
                                        iconOnly: true
                                        iconName: "upload"
                                        kind: "ghost"
                                        controlSize: 30
                                        enabled: root.loaded && !root.working
                                        Accessible.name: qsTr("Upload image")
                                        onClicked: root.beginUpload(
                                            artworkCard.imageType,
                                            artworkCard.targetImageIndex,
                                            artworkCard.hasImage ? "replace" : "add")
                                    }

                                    AppButton {
                                        visible: artworkCard.hasImage
                                        iconOnly: true
                                        iconName: "trash"
                                        kind: "ghost"
                                        controlSize: 30
                                        enabled: root.loaded && !root.working
                                        Accessible.name: qsTr("Delete image")
                                        onClicked: root.confirmDelete(artworkCard.primaryImage)
                                    }
                                }
                            }
                        }
                    }
                }

                Column {
                    width: parent.width
                    visible: root.expandedImageType.length > 0
                        && root.imagesForType(root.expandedImageType).length > 1
                    height: visible ? implicitHeight : 0
                    spacing: 10

                    Text {
                        width: parent.width
                        text: qsTr("%1 images").arg(
                            root.imageTypeLabel(root.expandedImageType))
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }

                    Flow {
                        width: parent.width
                        spacing: 12

                        Repeater {
                            model: root.imagesForType(root.expandedImageType)

                            delegate: Rectangle {
                                id: extraCard
                                required property var modelData
                                width: root.expandedCardWidth
                                height: 178
                                activeFocusOnTab: true
                                radius: 16
                                color: "#0DFFFFFF"
                                border.width: 1
                                border.color: extraCard.activeFocus
                                    ? Theme.accent : "#24FFFFFF"
                                clip: true

                                Connections {
                                    target: root

                                    function onFocusCardRequested(imageType, imageIndex) {
                                        if (String(imageType) === root.expandedImageType
                                                && root.normalizedImageIndex(imageIndex)
                                                    === root.normalizedImageIndex(
                                                        extraCard.modelData.imageIndex))
                                            extraCard.forceActiveFocus(Qt.PopupFocusReason)
                                    }
                                }

                                RoundedImage {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 9
                                    height: parent.height - 58
                                    source: extraCard.modelData.previewUrl || ""
                                    radius: 11
                                    asynchronous: true
                                    fillMode: Image.PreserveAspectFit
                                }

                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    height: 48
                                    color: "#B013161D"

                                    Text {
                                        anchors.left: parent.left
                                        anchors.leftMargin: 12
                                        anchors.right: extraActions.left
                                        anchors.rightMargin: 6
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: root.resolutionText(extraCard.modelData)
                                        color: Theme.textMuted
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }

                                    Row {
                                        id: extraActions
                                        anchors.right: parent.right
                                        anchors.rightMargin: 7
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 1

                                        AppButton {
                                            iconOnly: true
                                            iconName: "search"
                                            kind: "ghost"
                                            controlSize: 28
                                            enabled: !root.working
                                                && root.hasProviderForType(
                                                    root.expandedImageType)
                                            Accessible.name: qsTr("Search online")
                                            onClicked: root.beginRemoteSearch(
                                                root.expandedImageType,
                                                extraCard.modelData.imageIndex,
                                                "replace")
                                        }

                                        AppButton {
                                            iconOnly: true
                                            iconName: "upload"
                                            kind: "ghost"
                                            controlSize: 28
                                            enabled: !root.working
                                            Accessible.name: qsTr("Upload image")
                                            onClicked: root.beginUpload(
                                                root.expandedImageType,
                                                extraCard.modelData.imageIndex,
                                                "replace")
                                        }

                                        AppButton {
                                            iconOnly: true
                                            iconName: "trash"
                                            kind: "ghost"
                                            controlSize: 28
                                            enabled: !root.working
                                            Accessible.name: qsTr("Delete image")
                                            onClicked: root.confirmDelete(extraCard.modelData)
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 1
                    color: "#16FFFFFF"
                }

                Item {
                    width: parent.width
                    height: Math.max(backdropTitle.implicitHeight, backdropActions.implicitHeight)

                    Text {
                        id: backdropTitle
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        text: root.imageTypeLabel("Backdrop")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 18
                        font.weight: Font.DemiBold
                    }

                    Row {
                        id: backdropActions
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 6

                        AppButton {
                            iconName: "search"
                            text: qsTr("Search online")
                            kind: "ghost"
                            enabled: root.loaded && !root.working
                                && root.hasProviderForType("Backdrop")
                            onClicked: root.beginRemoteSearch(
                                "Backdrop", root.nextBackdropIndex(), "add")
                        }

                        AppButton {
                            iconName: "upload"
                            text: qsTr("Upload image")
                            kind: "ghost"
                            enabled: root.loaded && !root.working
                            onClicked: root.beginUpload(
                                "Backdrop", root.nextBackdropIndex(), "add")
                        }
                    }
                }

                Flow {
                    width: parent.width
                    spacing: 12

                    Rectangle {
                        visible: root.imagesForType("Backdrop").length === 0
                        width: root.backdropCardWidth
                        height: visible ? 200 : 0
                        radius: 18
                        color: "#0DFFFFFF"
                        border.width: 1
                        border.color: "#24FFFFFF"

                        LoadingPlaceholder {
                            anchors.fill: parent
                            anchors.margins: 10
                            running: root.loading
                            visible: root.loading
                            cornerRadius: 12
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: !root.loading
                            text: root.imageTypeLabel("Backdrop")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                    }

                    Repeater {
                        model: root.imagesForType("Backdrop")

                        delegate: Rectangle {
                            id: backdropCard
                            required property var modelData
                            width: root.backdropCardWidth
                            height: 210
                            activeFocusOnTab: true
                            radius: 18
                            color: "#0DFFFFFF"
                            border.width: 1
                            border.color: backdropCard.activeFocus
                                ? Theme.accent : "#24FFFFFF"
                            clip: true

                            Connections {
                                target: root

                                function onFocusCardRequested(imageType, imageIndex) {
                                    if (String(imageType) === "Backdrop"
                                            && root.normalizedImageIndex(imageIndex)
                                                === root.normalizedImageIndex(
                                                    backdropCard.modelData.imageIndex))
                                        backdropCard.forceActiveFocus(Qt.PopupFocusReason)
                                }
                            }

                            RoundedImage {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                anchors.margins: 10
                                height: parent.height - 66
                                source: backdropCard.modelData.previewUrl || ""
                                radius: 12
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                onStatusChanged: if (status === Image.Ready)
                                    root.reportFirstPreview()
                            }

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                height: 56
                                color: "#B013161D"

                                Column {
                                    anchors.left: parent.left
                                    anchors.leftMargin: 13
                                    anchors.right: backdropCardActions.left
                                    anchors.rightMargin: 6
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Text {
                                        width: parent.width
                                        text: root.imageTypeLabel("Backdrop")
                                        color: Theme.text
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        elide: Text.ElideRight
                                    }

                                    Text {
                                        width: parent.width
                                        text: root.resolutionText(backdropCard.modelData)
                                        color: Theme.textMuted
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }

                                Row {
                                    id: backdropCardActions
                                    anchors.right: parent.right
                                    anchors.rightMargin: 8
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 1

                                    AppButton {
                                        iconOnly: true
                                        iconName: "search"
                                        kind: "ghost"
                                        controlSize: 29
                                        enabled: !root.working
                                            && root.hasProviderForType("Backdrop")
                                        Accessible.name: qsTr("Search online")
                                        onClicked: root.beginRemoteSearch(
                                            "Backdrop",
                                            backdropCard.modelData.imageIndex,
                                            "replace")
                                    }

                                    AppButton {
                                        iconOnly: true
                                        iconName: "upload"
                                        kind: "ghost"
                                        controlSize: 29
                                        enabled: !root.working
                                        Accessible.name: qsTr("Upload image")
                                        onClicked: root.beginUpload(
                                            "Backdrop",
                                            backdropCard.modelData.imageIndex,
                                            "replace")
                                    }

                                    AppButton {
                                        iconOnly: true
                                        iconName: "trash"
                                        kind: "ghost"
                                        controlSize: 29
                                        enabled: !root.working
                                        Accessible.name: qsTr("Delete image")
                                        onClicked: root.confirmDelete(backdropCard.modelData)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            visible: root.searchMode
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.topMargin: 14
                Layout.bottomMargin: 12
                spacing: 10

                AppButton {
                    kind: "ghost"
                    iconName: "back"
                    text: qsTr("Current images")
                    enabled: root.workingKind !== "mutation"
                    onClicked: root.returnFromSearch()
                }

                Text {
                    text: root.imageTypeLabel(root.currentImageType)
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Item { Layout.fillWidth: true }

                AppButton {
                    text: root.includeAllLanguages
                        ? qsTr("All languages") : qsTr("Preferred language")
                    iconName: "globe"
                    checkable: true
                    checked: root.includeAllLanguages
                    kind: "ghost"
                    enabled: !root.working
                    onClicked: {
                        root.includeAllLanguages = !root.includeAllLanguages
                        root.beginSearch()
                    }
                }

                AppButton {
                    visible: String(root.editorData.itemType || "") === "Season"
                    text: qsTr("Include series images")
                    checkable: true
                    checked: root.enableSeriesImages
                    kind: "ghost"
                    enabled: !root.working
                    onClicked: {
                        root.enableSeriesImages = !root.enableSeriesImages
                        root.beginSearch()
                    }
                }
            }

            Flickable {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                contentWidth: providerRow.implicitWidth + 48
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                Row {
                    id: providerRow
                    x: 24
                    spacing: 7

                    Repeater {
                        model: root.providersForType(root.currentImageType)

                        delegate: AppButton {
                            required property var modelData
                            text: modelData.label
                            controlSize: 34
                            checkable: true
                            checked: root.searchProvider === String(modelData.name || "")
                            kind: "ghost"
                            enabled: !root.working
                            onClicked: {
                                root.searchProvider = String(modelData.name || "")
                                root.beginSearch()
                            }
                        }
                    }
                }
            }

            GridView {
                id: searchGrid
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                Layout.bottomMargin: 18
                clip: true
                cellWidth: Math.max(220,
                    Math.floor(width / Math.max(1, Math.floor(width / 246))))
                cellHeight: 236
                model: root.remoteImages
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: AppScrollBar {
                    policy: ScrollBar.AlwaysOn
                    visible: searchGrid.contentHeight > searchGrid.height + 1
                }

                delegate: Item {
                    id: remoteCard
                    required property var modelData
                    width: searchGrid.cellWidth
                    height: searchGrid.cellHeight

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 7
                        radius: 18
                        color: remoteMouse.containsMouse ? "#18FFFFFF" : "#0DFFFFFF"
                        border.width: 1
                        border.color: remoteMouse.containsMouse ? "#45FFFFFF" : "#24FFFFFF"
                        clip: true

                        Behavior on color { ColorAnimation { duration: 130 } }
                        Behavior on border.color { ColorAnimation { duration: 130 } }

                        RoundedImage {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            height: parent.height - 62
                            source: remoteCard.modelData.previewUrl || ""
                            radius: 12
                            asynchronous: true
                            fillMode: Image.PreserveAspectFit
                        }

                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 52
                            color: "#B013161D"

                            Column {
                                anchors.left: parent.left
                                anchors.leftMargin: 13
                                anchors.right: applyButton.left
                                anchors.rightMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 2

                                Text {
                                    width: parent.width
                                    text: String(remoteCard.modelData.providerName
                                                 || qsTr("Online source"))
                                    color: Theme.text
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }

                                Text {
                                    width: parent.width
                                    text: {
                                        const resolution = root.resolutionText(remoteCard.modelData)
                                        const language = String(
                                            remoteCard.modelData.displayLanguage
                                            || remoteCard.modelData.language || "")
                                        return resolution
                                            + (resolution.length > 0 && language.length > 0
                                                ? " · " : "") + language
                                    }
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 11
                                    elide: Text.ElideRight
                                }
                            }

                            AppButton {
                                id: applyButton
                                anchors.right: parent.right
                                anchors.rightMargin: 9
                                anchors.verticalCenter: parent.verticalCenter
                                iconOnly: true
                                iconName: "download"
                                kind: "primary"
                                controlSize: 34
                                enabled: !root.working
                                Accessible.name: qsTr("Use image")
                                onClicked: root.applyRemoteImage(remoteCard.modelData)
                            }
                        }

                        MouseArea {
                            id: remoteMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            acceptedButtons: Qt.NoButton
                        }
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: searchGrid.count === 0 && !root.working
                    text: qsTr("No online images matched this type and source.")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 14
                }

                LoadingIndicator {
                    anchors.centerIn: parent
                    visible: root.working && root.workingKind === "search"
                    running: visible
                    indicatorSize: 32
                }
            }
        }
    }

    FileDialog {
        id: uploadDialog
        title: qsTr("Choose an image")
        fileMode: FileDialog.OpenFile
        nameFilters: [
            qsTr("Image files (*.jpg *.jpeg *.png *.webp *.gif)"),
            qsTr("All files (*)")
        ]
        onAccepted: {
            if (!root.opened)
                return
            const context = root.pendingContext || ({})
            root.pendingMutation = {
                kind: "upload",
                fileUrl: selectedFile.toString(),
                context: context
            }
            root.beginUploadMutation(context, selectedFile.toString())
            root.working = true
            root.workingKind = "mutation"
            root.inlineError = ""
        }
        onRejected: {
            root.pendingContext = ({})
            if (root.viewModel)
                root.viewModel.clearPendingContext()
        }
    }

    AppConfirmDialog {
        id: deleteConfirm

        onConfirmed: {
            const image = root.pendingDeleteImage || ({})
            const context = root.pendingContext || ({})
            root.pendingMutation = {
                kind: "delete",
                context: context
            }
            root.beginDeleteMutation(context)
            root.pendingDeleteImage = ({})
            root.pendingContext = ({})
            if (!root.opened)
                return
            root.working = true
            root.workingKind = "mutation"
            root.inlineError = ""
        }

        onRejected: reason => {
            root.pendingDeleteImage = ({})
            root.pendingContext = ({})
            if (root.viewModel)
                root.viewModel.clearPendingContext()
        }
    }
}
