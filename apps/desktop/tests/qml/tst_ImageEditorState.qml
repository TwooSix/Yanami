import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    name: "ImageEditorState"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    ImageEditorDialog {
        id: editor
    }

    QtObject {
        id: imageViewModel
        property int searchCalls: 0
        property int retryCalls: 0
        property var lastFilters: ({})

        function selectTarget(imageType, imageIndex, mode) {
            return {
                imageType: imageType,
                imageIndex: imageIndex,
                mode: mode
            }
        }
        function search(filters) {
            ++searchCalls
            lastFilters = filters
            return { searchGeneration: 41 }
        }
        function retry() { ++retryCalls }
    }

    function editorPayload(id) {
        return {
            id: id,
            title: "Test item " + id,
            itemType: "Series",
            imageTypes: ["Primary", "Logo", "Thumb", "Banner", "Disc", "Art", "Backdrop"],
            images: [],
            providers: []
        }
    }

    function closeEditor() {
        editor.dismissBlocked = false
        if (editor.visible || editor.opened)
            editor.forceDismiss()
        tryCompare(editor, "opened", false)
    }

    function init() {
        closeEditor()
        editor.viewModel = null
        imageViewModel.searchCalls = 0
        imageViewModel.retryCalls = 0
        imageViewModel.lastFilters = ({})
    }

    function cleanup() {
        closeEditor()
    }

    function test_shellIsVisibleSynchronously() {
        const started = Date.now()
        const generation = editor.beginLoading({
            id: "shell-item",
            title: "Shell",
            itemType: "Series"
        })

        verify(generation > 0)
        verify(editor.visible)
        verify(Date.now() - started < 100)
        tryCompare(editor, "opened", true)
        verify(editor.loading)
    }

    function test_shellOpenP95StaysBelow100Ms() {
        const samples = []
        for (let index = 0; index < 20; ++index) {
            const started = Date.now()
            editor.beginLoading({
                id: "shell-p95-" + index,
                title: "Shell " + index,
                itemType: "Series"
            })
            samples.push(Date.now() - started)
            editor.forceDismiss()
        }
        samples.sort(function(left, right) { return left - right })
        const p95 = samples[Math.ceil(samples.length * 0.95) - 1]
        verify(p95 < 100, "shell P95 was " + p95 + "ms")
    }

    function test_dismissedAndSupersededResultsCannotReopenOrOverwrite() {
        const generationA = editor.beginLoading({ id: "A", title: "A" })
        tryCompare(editor, "opened", true)
        editor.forceDismiss()
        tryCompare(editor, "opened", false)

        compare(editor.applyEditor(editorPayload("A"), generationA), false)
        compare(editor.opened, false)

        const generationB = editor.beginLoading({ id: "B", title: "B" })
        tryCompare(editor, "opened", true)
        compare(editor.applyEditor(editorPayload("A"), generationA), false)
        compare(editor.currentItemId, "B")
        compare(editor.applyEditor(editorPayload("B"), generationB), true)
        compare(editor.editorData.id, "B")
    }

    function test_providerResponseCanArriveBeforeCurrentImages() {
        const generation = editor.beginLoading({ id: "race", title: "Race" })
        tryCompare(editor, "opened", true)

        compare(editor.applyProviders({
            id: "race",
            providers: [
                { name: "Provider A", supportedImages: ["Primary"] }
            ]
        }, generation), true)
        compare(editor.loading, true)
        compare(editor.loaded, false)
        compare(editor.editorData.providers.length, 1)

        const imagePayload = editorPayload("race")
        delete imagePayload.providers
        imagePayload.images = [
            { imageType: "Primary", imageIndex: 0, previewUrl: "" }
        ]
        compare(editor.applyEditor(imagePayload, generation), true)
        compare(editor.loaded, true)
        compare(editor.editorData.images.length, 1)
        compare(editor.editorData.providers.length, 1)
        compare(editor.editorData.providers[0].name, "Provider A")
    }

    function test_providerFailureDoesNotBlockCurrentImages() {
        const generation = editor.beginLoading({ id: "provider-fail", title: "Fail" })
        tryCompare(editor, "opened", true)
        const imagePayload = editorPayload("provider-fail")
        delete imagePayload.providers
        imagePayload.images = [
            { imageType: "Primary", imageIndex: 0, previewUrl: "" }
        ]
        compare(editor.applyEditor(imagePayload, generation), true)
        compare(editor.loaded, true)

        compare(editor.providersFailed("provider-fail", "provider unavailable"), true)
        compare(editor.loaded, true)
        compare(editor.loading, false)
        compare(editor.editorData.images.length, 1)
        compare(editor.editorData.images[0].previewUrl, "")
        compare(editor.providersError, "provider unavailable")
    }

    function test_imageGroupingAndBackdropIndex() {
        const payload = editorPayload("groups")
        payload.images = [
            { imageType: "Primary", imageIndex: 2, previewUrl: "" },
            { imageType: "Primary", imageIndex: 0, previewUrl: "" },
            { imageType: "Backdrop", imageIndex: 3, previewUrl: "" },
            { imageType: "Backdrop", imageIndex: 0, previewUrl: "" },
            { imageType: "Backdrop", imageIndex: 1, previewUrl: "" }
        ]
        editor.openFor(payload)
        tryCompare(editor, "opened", true)

        const primary = editor.imagesForType("Primary")
        compare(primary.length, 2)
        compare(primary[0].imageIndex, 0)
        compare(primary[1].imageIndex, 2)
        const backdrops = editor.imagesForType("Backdrop")
        compare(backdrops.length, 3)
        compare(backdrops[0].imageIndex, 0)
        compare(backdrops[2].imageIndex, 3)
        compare(editor.nextBackdropIndex(), 2)
    }

    function test_searchGenerationAndMutationReconcileAreNonBlocking() {
        const payload = editorPayload("mutation")
        payload.images = [
            { imageType: "Primary", imageIndex: 0, width: 10, height: 20,
              previewUrl: "" }
        ]
        editor.openFor(payload)
        tryCompare(editor, "opened", true)

        editor.pendingContext = {
            imageType: "Primary",
            imageIndex: 0,
            mode: "replace"
        }
        editor.pendingMutation = {
            kind: "apply",
            context: editor.pendingContext,
            image: { width: 100, height: 200,
                     previewUrl: "qrc:/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png" }
        }
        editor.searchMode = true
        editor.working = true
        editor.workingKind = "mutation"

        compare(editor.mutationSucceeded(), true)
        compare(editor.searchMode, false)
        compare(editor.working, false)
        compare(editor.reconciling, true)
        compare(editor.imagesForType("Primary")[0].previewUrl,
                "qrc:/qt/qml/Yanami/Ui/qml/assets/yanami-logo.png")

        const staleSearchGeneration = editor.activeSearchGeneration + 1
        editor.searchMode = true
        editor.working = true
        editor.workingKind = "search"
        editor.pendingContext = { imageType: "Primary", imageIndex: 0 }
        editor.currentImageType = "Primary"
        editor.activeSearchGeneration = staleSearchGeneration + 1
        compare(editor.presentSearchResults({ imageType: "Primary", images: [] },
                                            staleSearchGeneration), false)

        compare(editor.applyEditor(editorPayload("mutation"),
                                   editor.activeViewGeneration), true)
        compare(editor.reconciling, false)
    }

    function test_searchIntentEmitsARequestAndEntersSecondaryView() {
        const payload = editorPayload("search-intent")
        payload.providers = [
            { name: "Provider A", supportedImages: ["Primary"] }
        ]
        editor.openFor(payload)
        tryCompare(editor, "opened", true)
        editor.viewModel = imageViewModel

        const generation = editor.beginRemoteSearch("Primary", 0, "replace")
        verify(generation > 0)
        compare(editor.searchMode, true)
        compare(editor.working, true)
        compare(editor.workingKind, "search")
        compare(imageViewModel.searchCalls, 1)
        compare(imageViewModel.lastFilters.imageType, "Primary")
        compare(generation, 41)
    }

    function test_reconcileFailureRetryStaysNonBlocking() {
        editor.openFor(editorPayload("reconcile-retry"))
        tryCompare(editor, "opened", true)
        editor.reconciling = true

        compare(editor.handleReconciliationFailure("temporary failure"), true)
        compare(editor.lastFailedKind, "reconcile")
        compare(editor.working, false)
        compare(editor.dismissBlocked, false)

        editor.viewModel = imageViewModel
        editor.retryFailure()
        compare(editor.reconciling, true)
        compare(editor.working, false)
        compare(editor.dismissBlocked, false)
        compare(imageViewModel.retryCalls, 1)
    }

    function test_responsiveCardGeometryDoesNotOverflow() {
        const widths = [720, 900, 1240, 3840]
        const expectedColumns = [3, 4, 6, 6]
        for (let index = 0; index < widths.length; ++index) {
            editor.width = widths[index]
            compare(editor.artworkColumns, expectedColumns[index])
            verify(editor.artworkCardWidth > 0)
            const occupied = editor.artworkColumns * editor.artworkCardWidth
                + (editor.artworkColumns - 1) * 12
            verify(occupied <= editor.overviewContentWidth)

            verify(editor.backdropColumns >= 1)
            const backdropOccupied = editor.backdropColumns * editor.backdropCardWidth
                + (editor.backdropColumns - 1) * 12
            verify(backdropOccupied <= editor.overviewContentWidth)
        }
    }
}
