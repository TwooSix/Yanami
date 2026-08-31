import QtQuick
import QtTest
import Yanami.Ui

TestCase {
    id: testCase

    name: "MetadataEditorState"
    width: 1000
    height: 700
    visible: true
    when: windowShown

    MetadataEditorDialog {
        id: editor
    }

    Loader {
        id: deferredEditorLoader
        active: false
        sourceComponent: Component {
            MetadataEditorDialog {
                objectName: "deferredMetadataEditor"
            }
        }
    }

    function payload(id, title) {
        return {
            id: id,
            itemType: "Series",
            title: title,
            editableFields: ["Name", "Overview"],
            overview: "Overview",
            externalIds: []
        }
    }

    function closeEditor() {
        if (editor.visible || editor.opened) {
            editor.saving = false
            editor.loaded = false
            editor.forceDismiss()
        }
        tryCompare(editor, "opened", false)
    }

    function init() {
        closeEditor()
    }

    function cleanup() {
        closeEditor()
        if (deferredEditorLoader.item
                && (deferredEditorLoader.item.visible
                    || deferredEditorLoader.item.opened)) {
            deferredEditorLoader.item.saving = false
            deferredEditorLoader.item.loaded = false
            deferredEditorLoader.item.forceDismiss()
            tryCompare(deferredEditorLoader.item, "opened", false)
        }
        deferredEditorLoader.active = false
        tryCompare(deferredEditorLoader, "item", null)
    }

    function test_deferredEditorOpensOnFirstActivation() {
        compare(deferredEditorLoader.item, null)
        deferredEditorLoader.active = true

        const deferredEditor = deferredEditorLoader.item
        verify(deferredEditor)
        const generation = deferredEditor.beginLoading({
            id: "first-deferred-item",
            itemType: "Series"
        })
        tryCompare(deferredEditor, "opened", true)
        verify(deferredEditor.applyMetadata(
                   payload("first-deferred-item", "First deferred title"),
                   generation))
        compare(deferredEditor.loaded, true)
    }

    function test_shellAndSameItemGenerationGuard() {
        const started = Date.now()
        const oldGeneration = editor.beginLoading({
            id: "same-item",
            itemType: "Series"
        })
        verify(editor.visible)
        verify(Date.now() - started < 100)
        tryCompare(editor, "opened", true)

        editor.forceDismiss()
        tryCompare(editor, "opened", false)
        const newGeneration = editor.beginLoading({
            id: "same-item",
            itemType: "Series"
        })
        tryCompare(editor, "opened", true)
        verify(newGeneration > oldGeneration)
        compare(editor.applyMetadata(
                    payload("same-item", "stale"), oldGeneration), false)
        compare(editor.loaded, false)
        compare(editor.applyMetadata(
                    payload("same-item", "fresh"), newGeneration), true)
        compare(editor.loaded, true)
    }

    function test_controllerCanReachSafeHeaderActions() {
        const generation = editor.beginLoading({
            id: "controller-item",
            itemType: "Series"
        })
        tryCompare(editor, "opened", true)
        verify(editor.applyMetadata(
                   payload("controller-item", "Controller title"),
                   generation))
        InputModality.noteControllerNavigation()
        tryCompare(InputModality, "modality", InputModality.Controller)

        const navigator = findChild(editor, "metadataControllerNavigator")
        const cancelButton = findChild(editor, "metadataCancelButton")
        const saveButton = findChild(editor, "metadataSaveButton")
        verify(navigator)
        verify(cancelButton)
        verify(saveButton)
        verify(saveButton.enabled)

        // Initial focus remains the first field so controller users can edit
        // immediately. A single Up enters the safe header-action row.
        verify(navigator.move("up"))
        verify(cancelButton.activeFocus || saveButton.activeFocus)
        if (cancelButton.activeFocus)
            verify(navigator.move("right"))
        else
            verify(navigator.move("left"))
        verify(cancelButton.activeFocus || saveButton.activeFocus)

        // Both actions are on the same explicit spatial row and reachable
        // from each other without traversing text fields.
        const firstWasCancel = cancelButton.activeFocus
        verify(navigator.move(firstWasCancel ? "right" : "left"))
        compare(cancelButton.activeFocus, !firstWasCancel)
        compare(saveButton.activeFocus, firstWasCancel)
    }
}
