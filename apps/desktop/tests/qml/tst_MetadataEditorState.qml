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
}
