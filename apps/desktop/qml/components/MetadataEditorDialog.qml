import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui

AppModalPopup {
    id: root

    property var viewModel: null
    property string itemId: ""
    property string itemType: ""
    property var editableFields: []
    property bool saving: false
    property bool loading: false
    property bool loaded: false
    property string loadingError: ""
    property string baselineFormState: ""
    property int externalIdRevision: 0
    property string pendingDismissReason: ""
    property int viewGeneration: 0
    property int activeViewGeneration: 0
    signal validationError(string message)
    signal actionFailure(string message, bool nonModal, bool handledInPlace)

    dismissBlocked: root.saving
    confirmDirtyDismiss: true
    dirty: root.loaded && root.baselineFormState.length > 0
        && root.currentFormState() !== root.baselineFormState
    scrimColor: "#76000000"
    initialFocusTarget: titleField
    PopupControllerNavigator {
        id: controllerNavigator
        objectName: "metadataControllerNavigator"
        popup: root
    }
    onDiscardRequested: reason => {
        root.pendingDismissReason = reason
        discardConfirm.show(
            qsTr("Discard changes?"),
            qsTr("Your unsaved metadata changes will be lost."),
            qsTr("Discard"))
    }
    onClosed: {
        if (root.viewModel)
            root.viewModel.dismiss()
        ++root.viewGeneration
        root.activeViewGeneration = 0
    }

    ListModel {
        id: externalIdModel
    }

    function hasField(fieldName) {
        return root.editableFields.indexOf(fieldName) >= 0
    }

    function beginLoading(item) {
        const value = item || ({})
        ++root.viewGeneration
        const requestedGeneration = root.viewModel
            ? Number(root.viewModel.open(value) || 0) : root.viewGeneration
        root.activeViewGeneration = requestedGeneration > 0
            ? requestedGeneration : root.viewGeneration
        root.saving = false
        root.loading = true
        root.loaded = false
        root.loadingError = ""
        root.baselineFormState = ""
        root.externalIdRevision = 0
        root.itemId = String(value.id || "")
        root.itemType = String(value.itemType || "")
        root.editableFields = []
        externalIdModel.clear()
        root.open()
        console.info("metadata_editor", "phase=visible", "item=", root.itemId)
        return root.activeViewGeneration
    }

    function applyMetadata(metadata, generation) {
        const metadataId = String(metadata.id || "")
        if (!root.opened || root.loaded || metadataId.length === 0
                || metadataId !== root.itemId)
            return false
        if (generation !== undefined && generation !== null
                && Number(generation) !== root.activeViewGeneration)
            return false
        root.loading = false
        root.loaded = true
        root.loadingError = ""
        root.editableFields = metadata.editableFields || []
        titleField.text = String(metadata.title || "")
        originalTitleField.text = String(metadata.originalTitle || "")
        sortNameField.text = String(metadata.sortName || "")
        yearField.text = metadata.productionYear === null || metadata.productionYear === undefined
            ? "" : String(metadata.productionYear)
        premiereField.text = String(metadata.premiereDate || "")
        endDateField.text = String(metadata.endDate || "")
        ratingField.text = String(metadata.officialRating || "")
        communityField.text = metadata.communityRating === null || metadata.communityRating === undefined
            ? "" : String(metadata.communityRating)
        criticField.text = metadata.criticRating === null || metadata.criticRating === undefined
            ? "" : String(metadata.criticRating)
        statusField.text = String(metadata.status || "")
        airTimeField.text = String(metadata.airTime || "")
        airDaysField.text = metadata.airDays ? metadata.airDays.join(", ") : ""
        indexNumberField.text = metadata.indexNumber === null || metadata.indexNumber === undefined
            ? "" : String(metadata.indexNumber)
        parentIndexNumberField.text = metadata.parentIndexNumber === null
                || metadata.parentIndexNumber === undefined
            ? "" : String(metadata.parentIndexNumber)
        genresField.text = metadata.genres ? metadata.genres.join(", ") : ""
        tagsField.text = metadata.tags ? metadata.tags.join(", ") : ""
        overviewField.text = String(metadata.overview || "")
        externalIdModel.clear()
        const externalIds = metadata.externalIds || []
        for (let index = 0; index < externalIds.length; ++index) {
            const externalId = externalIds[index]
            externalIdModel.append({
                providerKey: String(externalId.key || ""),
                providerName: String(externalId.name || externalId.key || ""),
                identifierValue: String(externalId.value || ""),
                supportedAsIdentifier: externalId.supportedAsIdentifier === true
            })
        }
        ++root.externalIdRevision
        root.baselineFormState = root.currentFormState()
        console.info("metadata_editor", "phase=ready", "item=", root.itemId)
        return true
    }

    function openFor(metadata) {
        const generation = root.beginLoading(metadata)
        root.applyMetadata(metadata, generation)
    }

    function loadFailed(failedItemId, message) {
        if (!root.opened || String(failedItemId || "") !== root.itemId)
            return false
        root.loading = false
        root.loaded = false
        root.loadingError = String(message || "")
        return true
    }

    function retryLoading() {
        if (root.loading || root.itemId.length === 0)
            return
        root.loading = true
        root.loadingError = ""
        if (root.viewModel)
            root.viewModel.retry()
    }

    function scrollToExternalIdentifiers() {
        Qt.callLater(function() {
            editorScroll.scrollToContentY(editorScroll.contentHeight)
        })
    }

    function commaList(value) {
        const values = String(value || "").split(",")
        const result = []
        for (let index = 0; index < values.length; ++index) {
            const entry = values[index].trim()
            if (entry.length > 0)
                result.push(entry)
        }
        return result
    }

    function currentFormState() {
        // Reading the revision makes ListModel edits participate in the dirty
        // binding just like the directly referenced text properties below.
        const externalCount = externalIdModel.count
            + root.externalIdRevision * 0
        const externalIds = []
        for (let index = 0; index < externalCount; ++index) {
            const entry = externalIdModel.get(index)
            externalIds.push([
                String(entry.providerKey || ""),
                String(entry.identifierValue || "")
            ])
        }
        return JSON.stringify([
            titleField.text,
            originalTitleField.text,
            sortNameField.text,
            yearField.text,
            premiereField.text,
            endDateField.text,
            ratingField.text,
            communityField.text,
            criticField.text,
            statusField.text,
            airTimeField.text,
            airDaysField.text,
            indexNumberField.text,
            parentIndexNumberField.text,
            genresField.text,
            tagsField.text,
            overviewField.text,
            externalIds
        ])
    }

    function save() {
        if (root.saving || !root.loaded)
            return
        const title = titleField.text.trim()
        if (title.length === 0) {
            root.validationError(qsTr("Title cannot be empty."))
            titleField.focusInput()
            return
        }
        const yearText = yearField.text.trim()
        const communityText = communityField.text.trim()
        const criticText = criticField.text.trim()
        const indexText = indexNumberField.text.trim()
        const parentIndexText = parentIndexNumberField.text.trim()
        if (root.hasField("productionYear") && yearText.length > 0
                && (!Number.isInteger(Number(yearText)) || Number(yearText) < 0)) {
            root.validationError(qsTr("Production year must be a whole number."))
            yearField.focusInput()
            return
        }
        if (root.hasField("communityRating") && communityText.length > 0
                && Number.isNaN(Number(communityText))) {
            root.validationError(qsTr("Community rating must be a number."))
            communityField.focusInput()
            return
        }
        if (root.hasField("criticRating") && criticText.length > 0
                && Number.isNaN(Number(criticText))) {
            root.validationError(qsTr("Critic rating must be a number."))
            criticField.focusInput()
            return
        }
        if (root.hasField("indexNumber") && indexText.length > 0
                && (!Number.isInteger(Number(indexText)) || Number(indexText) < 0)) {
            root.validationError(qsTr("Number must be a non-negative whole number."))
            indexNumberField.focusInput()
            return
        }
        if (root.hasField("parentIndexNumber") && parentIndexText.length > 0
                && (!Number.isInteger(Number(parentIndexText)) || Number(parentIndexText) < 0)) {
            root.validationError(qsTr("Number must be a non-negative whole number."))
            parentIndexNumberField.focusInput()
            return
        }

        const changes = { title: title }
        if (root.hasField("originalTitle"))
            changes.originalTitle = originalTitleField.text.trim()
        if (root.hasField("sortName"))
            changes.sortName = sortNameField.text.trim()
        if (root.hasField("overview"))
            changes.overview = overviewField.text.trim()
        if (root.hasField("productionYear"))
            changes.productionYear = yearText.length > 0 ? Number(yearText) : null
        if (root.hasField("premiereDate"))
            changes.premiereDate = premiereField.text.trim()
        if (root.hasField("endDate"))
            changes.endDate = endDateField.text.trim()
        if (root.hasField("officialRating"))
            changes.officialRating = ratingField.text.trim()
        if (root.hasField("communityRating"))
            changes.communityRating = communityText.length > 0 ? Number(communityText) : null
        if (root.hasField("criticRating"))
            changes.criticRating = criticText.length > 0 ? Number(criticText) : null
        if (root.hasField("status"))
            changes.status = statusField.text.trim()
        if (root.hasField("airTime"))
            changes.airTime = airTimeField.text.trim()
        if (root.hasField("airDays"))
            changes.airDays = root.commaList(airDaysField.text)
        if (root.hasField("indexNumber"))
            changes.indexNumber = indexText.length > 0 ? Number(indexText) : null
        if (root.hasField("parentIndexNumber"))
            changes.parentIndexNumber = parentIndexText.length > 0 ? Number(parentIndexText) : null
        if (root.hasField("genres"))
            changes.genres = root.commaList(genresField.text)
        if (root.hasField("tags"))
            changes.tags = root.commaList(tagsField.text)

        const providerIds = {}
        for (let index = 0; index < externalIdModel.count; ++index) {
            const entry = externalIdModel.get(index)
            providerIds[entry.providerKey] = String(entry.identifierValue || "").trim()
        }
        changes.providerIds = providerIds
        if (root.viewModel && root.viewModel.save(changes))
            root.saving = true
    }

    function saveSucceeded() {
        root.saving = false
        root.forceDismiss()
    }

    function saveFailed() {
        root.saving = false
    }

    Connections {
        target: root.viewModel

        function onMetadataReady(metadata, viewGeneration) {
            root.applyMetadata(metadata, viewGeneration)
        }
        function onLoadFailed(itemId, message, nonModal) {
            const handled = root.loadFailed(itemId, message)
            root.actionFailure(message, nonModal, handled)
        }
        function onSaveCompleted(itemId, result) {
            if (String(itemId) === root.itemId)
                root.saveSucceeded()
        }
        function onSaveFailed(itemId, message, nonModal) {
            if (String(itemId) === root.itemId)
                root.saveFailed()
            root.actionFailure(message, nonModal, false)
        }
        function onReconciliationFailed(itemId, message, nonModal) {
            root.actionFailure(message, nonModal, false)
        }
    }

    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - height) / 2)
    width: Math.min(720, Math.max(420, parent.width - 64))
    height: Math.min(720, Math.max(540, parent.height - 64))
    padding: 0
    background: Rectangle {
        radius: 28
        color: "#F7181B23"
        border.width: 1
        border.color: "#4AFFFFFF"
    }

    contentItem: Item {
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 26
            spacing: 18

            RowLayout {
                Layout.fillWidth: true
                Column {
                    Layout.fillWidth: true
                    spacing: 3
                    Text {
                        text: qsTr("Edit metadata")
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                    }
                    Text {
                        text: LocaleText.itemTypeLabel(root.itemType)
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                }
                AppButton {
                    id: cancelButton
                    objectName: "metadataCancelButton"
                    kind: "ghost"
                    text: qsTr("Cancel")
                    enabled: !root.saving
                    onClicked: root.requestDismiss("cancel")
                }
                AppButton {
                    id: saveButton
                    objectName: "metadataSaveButton"
                    kind: "primary"
                    text: root.saving ? qsTr("Saving…") : qsTr("Save")
                    enabled: root.loaded && !root.saving
                    onClicked: root.save()
                }
            }

            SmoothFlickable {
                id: editorScroll
                visible: root.loaded
                opacity: root.loaded ? 1 : 0
                Layout.fillWidth: true
                Layout.fillHeight: true
                contentHeight: editorForm.implicitHeight + 16

                Behavior on opacity {
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }

                ColumnLayout {
                    id: editorForm
                    width: parent.width - 12
                    spacing: 12

                    AppTextField { id: titleField; Layout.fillWidth: true; label: qsTr("Title") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        AppTextField {
                            id: originalTitleField
                            visible: root.hasField("originalTitle")
                            Layout.fillWidth: true
                            label: qsTr("Original title")
                        }
                        AppTextField {
                            id: sortNameField
                            visible: root.hasField("sortName")
                            Layout.fillWidth: true
                            label: qsTr("Sort title")
                        }
                    }
                    RowLayout {
                        visible: root.hasField("productionYear") || root.hasField("premiereDate")
                            || root.hasField("endDate")
                        Layout.fillWidth: true
                        spacing: 12
                        AppTextField {
                            id: yearField
                            visible: root.hasField("productionYear")
                            Layout.fillWidth: true
                            label: qsTr("Production year")
                            inputMethodHints: Qt.ImhDigitsOnly
                        }
                        AppTextField {
                            id: premiereField
                            visible: root.hasField("premiereDate")
                            Layout.fillWidth: true
                            label: qsTr("Premiere date")
                        }
                        AppTextField {
                            id: endDateField
                            visible: root.hasField("endDate")
                            Layout.fillWidth: true
                            label: root.itemType === "Person" ? qsTr("Death date") : qsTr("End date")
                        }
                    }
                    RowLayout {
                        visible: root.hasField("officialRating") || root.hasField("communityRating")
                            || root.hasField("criticRating")
                        Layout.fillWidth: true
                        spacing: 12
                        AppTextField {
                            id: ratingField
                            visible: root.hasField("officialRating")
                            Layout.fillWidth: true
                            label: qsTr("Official rating")
                        }
                        AppTextField {
                            id: communityField
                            visible: root.hasField("communityRating")
                            Layout.fillWidth: true
                            label: qsTr("Community rating")
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                        }
                        AppTextField {
                            id: criticField
                            visible: root.hasField("criticRating")
                            Layout.fillWidth: true
                            label: qsTr("Critic rating")
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                        }
                    }
                    RowLayout {
                        visible: root.hasField("status") || root.hasField("airTime")
                        Layout.fillWidth: true
                        spacing: 12
                        AppTextField {
                            id: statusField
                            visible: root.hasField("status")
                            Layout.fillWidth: true
                            label: qsTr("Series status")
                            placeholderText: qsTr("Continuing or Ended")
                        }
                        AppTextField {
                            id: airTimeField
                            visible: root.hasField("airTime")
                            Layout.fillWidth: true
                            label: qsTr("Air time")
                            placeholderText: qsTr("For example, 20:30")
                        }
                    }
                    RowLayout {
                        visible: root.hasField("indexNumber") || root.hasField("parentIndexNumber")
                        Layout.fillWidth: true
                        spacing: 12
                        AppTextField {
                            id: parentIndexNumberField
                            visible: root.hasField("parentIndexNumber")
                            Layout.fillWidth: true
                            label: root.itemType === "Episode" ? qsTr("Season number") : qsTr("Disc number")
                            inputMethodHints: Qt.ImhDigitsOnly
                        }
                        AppTextField {
                            id: indexNumberField
                            visible: root.hasField("indexNumber")
                            Layout.fillWidth: true
                            label: root.itemType === "Episode" ? qsTr("Episode number")
                                : (root.itemType === "Season" ? qsTr("Season number") : qsTr("Track number"))
                            inputMethodHints: Qt.ImhDigitsOnly
                        }
                    }
                    AppTextField {
                        id: genresField
                        visible: root.hasField("genres")
                        Layout.fillWidth: true
                        label: qsTr("Genres")
                        placeholderText: qsTr("Separate multiple values with commas")
                    }
                    AppTextField {
                        id: airDaysField
                        visible: root.hasField("airDays")
                        Layout.fillWidth: true
                        label: qsTr("Air days")
                        placeholderText: qsTr("Separate multiple values with commas")
                    }
                    AppTextField {
                        id: tagsField
                        visible: root.hasField("tags")
                        Layout.fillWidth: true
                        label: qsTr("Tags")
                        placeholderText: qsTr("Separate multiple values with commas")
                    }

                    ColumnLayout {
                        visible: root.hasField("overview")
                        Layout.fillWidth: true
                        spacing: 7
                        Text {
                            text: qsTr("Overview")
                            color: overviewField.activeFocus ? Theme.text : Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 12
                            font.weight: Font.Medium
                        }
                        TextArea {
                            id: overviewField
                            Layout.fillWidth: true
                            Layout.preferredHeight: 150
                            leftPadding: 16
                            rightPadding: 16
                            topPadding: 14
                            bottomPadding: 14
                            color: Theme.text
                            placeholderTextColor: "#6F788A"
                            selectionColor: Theme.accent
                            selectedTextColor: "white"
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                            wrapMode: TextArea.Wrap
                            selectByMouse: true
                            Keys.priority: Keys.BeforeItem
                            Keys.onPressed: event => {
                                if (InputModality.modality
                                            !== InputModality.Controller
                                        && InputModality.modality
                                            !== InputModality.Remote)
                                    return
                                if (event.key === Qt.Key_Menu) {
                                    overviewField.clear()
                                    event.accepted = true
                                    return
                                }
                                if (event.key !== Qt.Key_Up
                                        && event.key !== Qt.Key_Down
                                        && event.key !== Qt.Key_Left
                                        && event.key !== Qt.Key_Right)
                                    return
                                const forward = event.key === Qt.Key_Down
                                    || event.key === Qt.Key_Right
                                const target = overviewField.nextItemInFocusChain(
                                    forward)
                                if (target && target !== overviewField) {
                                    target.forceActiveFocus(forward
                                        ? Qt.TabFocusReason
                                        : Qt.BacktabFocusReason)
                                    event.accepted = true
                                }
                            }
                            background: Rectangle {
                                radius: Theme.radiusSmall
                                color: overviewField.activeFocus ? "#E0161922" : Theme.field
                                border.width: overviewField.activeFocus ? 1.5 : 1
                                border.color: overviewField.activeFocus ? Theme.accent : Theme.outline
                            }
                        }
                        TapHandler {
                            parent: overviewField
                            acceptedButtons: Qt.AllButtons
                            onPressedChanged: {
                                if (pressed)
                                    PopupCoordinator.notePopupContentPress()
                            }
                        }
                    }

                    ColumnLayout {
                        visible: externalIdModel.count > 0
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        spacing: 4

                        Text {
                            text: qsTr("External identifiers")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 16
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Set exact provider IDs before refreshing metadata to improve matching accuracy.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: 8
                            columns: width >= 560 ? 2 : 1
                            columnSpacing: 12
                            rowSpacing: 12

                            Repeater {
                                model: externalIdModel

                                delegate: AppTextField {
                                    required property int index
                                    required property string providerKey
                                    required property string providerName
                                    required property string identifierValue

                                    Layout.fillWidth: true
                                    label: qsTr("%1 ID").arg(providerName)
                                    text: identifierValue
                                    placeholderText: providerKey
                                    onTextChanged: {
                                        if (identifierValue !== text) {
                                            externalIdModel.setProperty(index, "identifierValue", text)
                                            ++root.externalIdRevision
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Item {
                visible: !root.loaded
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    id: loadingSkeleton
                    visible: root.loading
                    anchors.fill: parent
                    anchors.rightMargin: 12
                    spacing: 12
                    opacity: 0.72

                    Repeater {
                        model: 6

                        delegate: Rectangle {
                            required property int index
                            Layout.fillWidth: true
                            Layout.preferredHeight: index === 5 ? 138 : 58
                            radius: Theme.radiusSmall
                            color: index % 2 === 0 ? "#10FFFFFF" : "#0CFFFFFF"
                        }
                    }

                    Item { Layout.fillHeight: true }

                    SequentialAnimation on opacity {
                        running: root.loading && root.opened
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.48; duration: 720; easing.type: Easing.InOutSine }
                        NumberAnimation { to: 0.78; duration: 720; easing.type: Easing.InOutSine }
                    }
                }

                ColumnLayout {
                    visible: !root.loading && root.loadingError.length > 0
                    anchors.centerIn: parent
                    width: Math.min(parent.width - 48, 420)
                    spacing: 16

                    Text {
                        Layout.fillWidth: true
                        text: root.loadingError
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 14
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.Wrap
                    }
                    AppButton {
                        id: retryButton
                        objectName: "metadataRetryButton"
                        Layout.alignment: Qt.AlignHCenter
                        kind: "secondary"
                        text: qsTr("Try again")
                        onClicked: root.retryLoading()
                    }
                }
            }
        }
    }

    AppConfirmDialog {
        id: discardConfirm
        onConfirmed: {
            root.pendingDismissReason = ""
            root.forceDismiss()
        }
        onRejected: root.pendingDismissReason = ""
    }
}
