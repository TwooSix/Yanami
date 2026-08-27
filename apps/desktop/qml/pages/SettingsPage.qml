import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui

Item {
    id: root

    property bool pageActive: false
    property real embyConnectionProgress: app.session.connected ? 1 : 0
    property bool observedConnected: app.session.connected
    property bool loadingUpscalingSettings: false
    property bool upscalingEnabled: false
    property int upscalingPreset: 1
    property real upscalingAutoHeadroom: 20
    property int upscalingAnime4KContentMode: 0
    property int upscalingAnime4KModelSize: 2
    property int upscalingAnime4KRestorePasses: 1
    property bool upscalingAnime4KAutoDownscale: true
    property bool upscalingPerformanceProtection: true
    property string lastControllerAction: ""
    property string lastControllerPrompt: ""
    property bool lastControllerActionRepeated: false
    property bool controllerActionCleared: false
    readonly property var upscalingPresetIds: ["performance", "balanced", "quality", "custom"]
    readonly property var upscalingSelectedProviderCapability:
        nativeUpscalingProvider("anime4k")
    readonly property bool upscalingAnime4KSupported:
        upscalingSelectedProviderCapability.supported === true
    readonly property bool upscalingCanEnable:
        app.upscaling.capabilityReady && upscalingAnime4KSupported
    readonly property bool upscalingConfigurationAvailable:
        app.upscaling.capabilityReady
        && upscalingSelectedProviderCapability.supported === true
    readonly property string upscalingUnavailableReason:
        !app.upscaling.capabilityReady
        ? qsTr("Checking graphics capabilities…")
        : (upscalingSelectedProviderCapability.unavailableReason
           || qsTr("This upscaling method is unavailable on the active playback renderer."))
    readonly property string upscalingAssetPhase:
        app.upscaling.selectedAssets.phase || "unsupported"
    readonly property int upscalingDownloadState:
        upscalingAssetPhase === "ready" ? 3
      : upscalingAssetPhase === "checking" || upscalingAssetPhase === "verifying" ? 2
      : upscalingAssetPhase === "queued" || upscalingAssetPhase === "downloading" ? 1
      : upscalingAssetPhase === "failed" ? 4
      : upscalingAssetPhase === "unsupported" ? 5 : 0
    readonly property real upscalingDownloadProgress:
        Math.max(0, Math.min(100,
            Number(app.upscaling.selectedAssets.progress || 0) * 100))
    readonly property bool upscalingRequiresComponents:
        app.upscaling.selectedAssets.requiresDownload !== false

    function nativeUpscalingProvider(id) {
        const providers = app.upscaling.providers || []
        for (let index = 0; index < providers.length; ++index) {
            if (providers[index].id === id)
                return providers[index]
        }
        return {
            "id": id,
            "supported": false,
            "unavailableReason": qsTr("Checking graphics capabilities…"),
            "requiredBackend": ""
        }
    }

    function upscalingPresetDisplayName(id) {
        return id === "performance" ? qsTr("Performance")
             : id === "balanced" ? qsTr("Balanced")
             : id === "quality" ? qsTr("Quality")
             : id === "custom" ? qsTr("Custom")
             : ""
    }

    readonly property var upscalingPresets: [
        {
            "name": qsTr("Performance"),
            "description": qsTr("Lowest GPU load · best for integrated graphics")
        },
        {
            "name": qsTr("Balanced"),
            "description": qsTr("Clearer lines with comfortable playback headroom")
        },
        {
            "name": qsTr("Quality"),
            "description": qsTr("Highest real-time detail · for faster discrete GPUs")
        },
        {
            "name": qsTr("Custom"),
            "description": qsTr("Tune Anime4K model and source handling")
        }
    ]
    readonly property string upscalingEffectiveProviderName: "Anime4K"
    readonly property string upscalingEffectivePresetName:
        app.upscaling.resolvedPresetId !== ""
        ? upscalingPresetDisplayName(app.upscaling.resolvedPresetId)
        : upscalingPresets[upscalingPreset].name
    readonly property real trailingControlWidth: 300
    readonly property real sectionNavWidth: Math.min(176, Math.max(140, width * 0.19))
    readonly property real sectionNavGap: 18
    readonly property real sectionScrollTopMargin: 18
    readonly property real scrollBarGutter: 18
    readonly property int activeSection: {
        const probeY = settingsFlickable.contentY + sectionScrollTopMargin + 20
        if (settingsFlickable.contentY > 0.5 && settingsFlickable.atYEnd)
            return 5
        if (probeY >= form.y + developerSection.y)
            return 5
        if (probeY >= form.y + controllerSection.y)
            return 4
        if (probeY >= form.y + upscalingSection.y)
            return 3
        if (probeY >= form.y + playbackPanel.y)
            return 2
        if (probeY >= form.y + languagePanel.y)
            return 1
        return 0
    }

    function controllerFamilyLabel(family) {
        const normalized = String(family || "").toLowerCase()
        if (normalized === "xbox")
            return qsTr("Xbox")
        if (normalized === "playstation")
            return qsTr("PlayStation · experimental")
        if (normalized === "nintendo")
            return qsTr("Nintendo Switch · experimental")
        if (normalized === "remote")
            return qsTr("TV remote · experimental")
        if (normalized === "generic")
            return qsTr("Generic controller")
        return qsTr("None")
    }

    function controllerDeviceName(device) {
        const name = String(device && device.name || "")
        const id = String(device && device.id || "")
        if (id === "remote:qt-key")
            return qsTr("TV remote / media keys")
        if (id.startsWith("xinput:")) {
            const slot = Number(id.slice("xinput:".length)) + 1
            return qsTr("Xbox controller %1").arg(slot)
        }
        if (name === "HID TV Remote")
            return qsTr("HID TV remote")
        if (name === "Gamepad")
            return qsTr("Gamepad")
        return name.length > 0 ? name : qsTr("Unknown device")
    }

    function controllerActionLabel(action) {
        if (action === InputModality.NavigateUp)
            return qsTr("Navigate up")
        if (action === InputModality.NavigateDown)
            return qsTr("Navigate down")
        if (action === InputModality.NavigateLeft)
            return qsTr("Navigate left")
        if (action === InputModality.NavigateRight)
            return qsTr("Navigate right")
        if (action === InputModality.Activate)
            return qsTr("Activate")
        if (action === InputModality.Back)
            return qsTr("Back")
        if (action === InputModality.Context)
            return qsTr("Context menu")
        if (action === InputModality.Menu)
            return qsTr("Application menu")
        if (action === InputModality.Search)
            return qsTr("Search")
        if (action === InputModality.PagePrevious)
            return qsTr("Previous page")
        if (action === InputModality.PageNext)
            return qsTr("Next page")
        if (action === InputModality.PageUp)
            return qsTr("Page up")
        if (action === InputModality.PageDown)
            return qsTr("Page down")
        if (action === InputModality.ScrollUp)
            return qsTr("Scroll up")
        if (action === InputModality.ScrollDown)
            return qsTr("Scroll down")
        if (action === InputModality.ScrollLeft)
            return qsTr("Scroll left")
        if (action === InputModality.ScrollRight)
            return qsTr("Scroll right")
        if (action === InputModality.PlayPause)
            return qsTr("Play or pause")
        if (action === InputModality.SeekBackward)
            return qsTr("Seek backward")
        if (action === InputModality.SeekForward)
            return qsTr("Seek forward")
        if (action === InputModality.VolumeUp)
            return qsTr("Volume up")
        if (action === InputModality.VolumeDown)
            return qsTr("Volume down")
        if (action === InputModality.PreviousItem)
            return qsTr("Previous item")
        if (action === InputModality.NextItem)
            return qsTr("Next item")
        return qsTr("Unknown action")
    }

    function controllerActionNameLabel(actionName) {
        const actions = ({
            "navigateUp": InputModality.NavigateUp,
            "navigateDown": InputModality.NavigateDown,
            "navigateLeft": InputModality.NavigateLeft,
            "navigateRight": InputModality.NavigateRight,
            "activate": InputModality.Activate,
            "back": InputModality.Back,
            "context": InputModality.Context,
            "menu": InputModality.Menu,
            "search": InputModality.Search,
            "pagePrevious": InputModality.PagePrevious,
            "pageNext": InputModality.PageNext,
            "pageUp": InputModality.PageUp,
            "pageDown": InputModality.PageDown,
            "scrollUp": InputModality.ScrollUp,
            "scrollDown": InputModality.ScrollDown,
            "scrollLeft": InputModality.ScrollLeft,
            "scrollRight": InputModality.ScrollRight,
            "playPause": InputModality.PlayPause,
            "seekBackward": InputModality.SeekBackward,
            "seekForward": InputModality.SeekForward,
            "volumeUp": InputModality.VolumeUp,
            "volumeDown": InputModality.VolumeDown,
            "previousItem": InputModality.PreviousItem,
            "nextItem": InputModality.NextItem
        })
        const key = String(actionName || "")
        return actions[key] !== undefined
            ? root.controllerActionLabel(actions[key])
            : qsTr("Unknown action")
    }

    function controllerDefaultFocusItem() {
        return sectionRepeater.itemAt(0)
    }

    function indexOfValue(values, value, fallback) {
        const index = values.indexOf(value)
        return index >= 0 ? index : fallback
    }

    function loadUpscalingSettings() {
        const settings = app.upscaling.settings || {}
        loadingUpscalingSettings = true
        upscalingEnabled = settings.enabled === true
        upscalingPreset = indexOfValue(
            upscalingPresetIds, settings.presetId, 1)
        upscalingAutoHeadroom = Number(settings.autoHeadroom || 20)
        upscalingAnime4KContentMode = indexOfValue(
            ["a", "b", "c"], settings.anime4kMode, 0)
        upscalingAnime4KModelSize = indexOfValue(
            ["s", "m", "l", "vl", "ul"], settings.anime4kModelSize, 3)
        upscalingAnime4KRestorePasses = Math.max(
            0, Math.min(1, Number(settings.anime4kRestorePasses || 1) - 1))
        upscalingAnime4KAutoDownscale = settings.anime4kAutoDownscale !== false
        upscalingPerformanceProtection = settings.performanceProtection !== false
        loadingUpscalingSettings = false
    }

    function saveUpscalingSettings() {
        if (loadingUpscalingSettings)
            return
        app.upscaling.saveSettings({
            "enabled": upscalingEnabled,
            "providerId": "anime4k",
            "presetId": upscalingPresetIds[upscalingPreset],
            "autoHeadroom": Math.round(upscalingAutoHeadroom),
            "anime4kMode": ["a", "b", "c"][upscalingAnime4KContentMode],
            "anime4kModelSize": ["s", "m", "l", "vl", "ul"][upscalingAnime4KModelSize],
            "anime4kRestorePasses": upscalingAnime4KRestorePasses + 1,
            "anime4kAutoDownscale": upscalingAnime4KAutoDownscale,
            "performanceProtection": upscalingPerformanceProtection
        })
    }

    function selectUpscalingPreset(index) {
        if (index < 0 || index >= upscalingPresets.length
                || !upscalingConfigurationAvailable)
            return
        if (upscalingPreset === index)
            return
        upscalingPreset = index
        saveUpscalingSettings()
    }

    function resetUpscalingCustomSettings() {
        upscalingAnime4KContentMode = 0
        upscalingAnime4KModelSize = 3
        upscalingAnime4KRestorePasses = 0
        upscalingAnime4KAutoDownscale = true
        saveUpscalingSettings()
    }

    component CompactSwitch: Button {
        id: compactSwitch

        property string accessibleLabel: ""

        checkable: true
        implicitWidth: 60
        implicitHeight: 44
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.CheckBox
        Accessible.name: accessibleLabel
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
                return
            compactSwitch.click()
            event.accepted = true
        }

        contentItem: Item {}
        background: Item {
            Rectangle {
                width: 52
                height: 30
                anchors.centerIn: parent
                radius: height / 2
                color: compactSwitch.checked ? Theme.accent : "#24FFFFFF"
                border.width: compactSwitch.visualFocus ? 2 : 1
                border.color: compactSwitch.visualFocus
                              ? "white" : compactSwitch.checked
                                ? Theme.accentHover : Theme.outlineStrong

                Rectangle {
                    width: 22
                    height: 22
                    radius: 11
                    x: compactSwitch.checked ? parent.width - width - 4 : 4
                    anchors.verticalCenter: parent.verticalCenter
                    color: compactSwitch.checked ? "white" : "#AAB2C1"

                    Behavior on x {
                        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                    }
                }

                Behavior on color { ColorAnimation { duration: 140 } }
                Behavior on border.color { ColorAnimation { duration: 140 } }
            }
        }
    }

    component UpscalingInfoButton: Button {
        id: infoButton

        property string fieldLabel: ""
        property string helpText: ""
        property bool tooltipPinned: false
        property bool tooltipSuppressed: false
        property bool tooltipHoverGrace: false
        readonly property bool interactionActive: hovered || visualFocus
            || tooltipPinned || tooltipHoverGrace || infoToolTip.hovered
        readonly property bool showToolTip: root.visible
            && helpText.length > 0
            && !tooltipSuppressed
            && (hovered || visualFocus || tooltipPinned
                || tooltipHoverGrace || infoToolTip.hovered)

        implicitWidth: 20
        implicitHeight: 20
        Layout.minimumWidth: 20
        Layout.preferredWidth: 20
        Layout.maximumWidth: 20
        Layout.minimumHeight: 20
        Layout.preferredHeight: 20
        Layout.maximumHeight: 20
        Layout.alignment: Qt.AlignVCenter
        leftPadding: 0
        rightPadding: 0
        topPadding: 0
        bottomPadding: 0
        visible: helpText.length > 0
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("Information about %1").arg(fieldLabel)
        Accessible.description: helpText
        Keys.onPressed: event => {
            if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter)
                return
            infoButton.click()
            event.accepted = true
        }

        contentItem: Item {
            AppIcon {
                anchors.centerIn: parent
                width: 13
                height: 13
                name: "info"
                color: infoButton.interactionActive
                       ? Theme.text : Theme.textMuted

                Behavior on color { ColorAnimation { duration: 120 } }
            }
        }

        background: Rectangle {
            radius: 6
            color: infoButton.down
                   ? "#20FFFFFF"
                   : infoButton.interactionActive
                     ? "#12FFFFFF" : "transparent"
            border.width: infoButton.visualFocus ? 1 : 0
            border.color: Theme.accent

            Behavior on color { ColorAnimation { duration: 120 } }
        }

        onClicked: {
            tooltipSuppressed = false
            if (!activeFocus)
                forceActiveFocus(Qt.MouseFocusReason)
            // Pointer users keep the tooltip open through hover. Pinning is
            // only needed when a keyboard or accessibility activation occurs
            // without a pointer over the button.
            tooltipPinned = !hovered && !tooltipPinned
        }
        onPressedChanged: {
            if (pressed)
                PopupCoordinator.notePopupContentPress()
        }
        onActiveFocusChanged: {
            if (!activeFocus) {
                tooltipPinned = false
                tooltipSuppressed = false
            }
        }
        onHoveredChanged: {
            if (hovered) {
                tooltipCloseGrace.stop()
                tooltipHoverGrace = false
                tooltipSuppressed = false
            } else if (infoToolTip.visible) {
                tooltipHoverGrace = true
                tooltipCloseGrace.restart()
            }
        }
        Keys.onEscapePressed: event => {
            tooltipPinned = false
            tooltipSuppressed = true
            event.accepted = true
        }

        Timer {
            id: tooltipCloseGrace

            interval: 300
            repeat: false
            onTriggered: infoButton.tooltipHoverGrace = false
        }

        Connections {
            target: root

            function onVisibleChanged() {
                if (root.visible)
                    return
                tooltipCloseGrace.stop()
                infoButton.tooltipPinned = false
                infoButton.tooltipHoverGrace = false
                infoButton.tooltipSuppressed = false
            }
        }

        AppToolTip {
            id: infoToolTip

            parent: infoButton
            visible: infoButton.showToolTip
            text: infoButton.helpText
            delay: infoButton.visualFocus || infoButton.tooltipPinned ? 0 : 450
            timeout: -1
            verticalGap: 4
            keepOpenOnHover: true
            selectableText: true
            coordinatePopupLifecycle: true
            closePolicy: Popup.NoAutoClose
            onHoveredChanged: {
                if (hovered) {
                    tooltipCloseGrace.stop()
                    infoButton.tooltipHoverGrace = false
                    // Once the pointer enters the tooltip, pointer presence is
                    // the only lifetime owner. Text selection focus must not
                    // leave the tooltip stuck open after the pointer exits.
                    infoButton.tooltipPinned = false
                } else {
                    tooltipCloseGrace.stop()
                    infoButton.tooltipHoverGrace = false
                    infoButton.tooltipPinned = false
                }
            }
            onEscapeRequested: {
                infoButton.tooltipPinned = false
                infoButton.tooltipSuppressed = true
                infoButton.forceActiveFocus(Qt.OtherFocusReason)
            }
        }
    }

    component UpscalingChoiceRow: ColumnLayout {
        id: choiceRow

        property string label: ""
        property string helpText: ""
        property var options: []
        property int selectedIndex: 0
        signal optionSelected(int index)

        Layout.fillWidth: true
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 2

            Text {
                id: choiceLabel

                Layout.maximumWidth: Math.max(
                    0, choiceRow.width - choiceInfo.implicitWidth - 12)
                text: choiceRow.label
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
                elide: Text.ElideRight
            }

            UpscalingInfoButton {
                id: choiceInfo

                fieldLabel: choiceRow.label
                helpText: choiceRow.helpText
            }

            Item { Layout.fillWidth: true }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: choiceRow.options.length >= 5 && choiceRow.width < 560
                     ? 3 : Math.max(1, choiceRow.options.length)
            columnSpacing: 8
            rowSpacing: 8
            uniformCellWidths: true
            uniformCellHeights: true

            Repeater {
                model: choiceRow.options

                delegate: AppButton {
                    required property int index
                    required property string modelData

                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    controlSize: 36
                    checkable: true
                    checked: choiceRow.selectedIndex === index
                    kind: checked ? "primary" : "ghost"
                    text: modelData
                    onClicked: choiceRow.optionSelected(index)
                }
            }
        }
    }

    Behavior on embyConnectionProgress {
        NumberAnimation {
            duration: 280
            easing.type: Easing.OutCubic
        }
    }

    Timer {
        id: upscalingSaveTimer

        interval: 180
        repeat: false
        onTriggered: root.saveUpscalingSettings()
    }

    function connectEmby() {
        if (app.session.connected || app.session.busy
                || serverName.text.trim().length === 0
                || serverUrl.text.trim().length === 0
                || username.text.trim().length === 0)
            return
        if (serverUrl.text.trim().toLowerCase().startsWith("http://")) {
            insecureHttpConfirm.show(
                qsTr("Unencrypted HTTP connection"),
                qsTr("HTTP sends your Emby password and session token without encryption. Continue only if you trust the network path to this server; HTTPS is strongly recommended."),
                qsTr("Connect with HTTP"))
            return
        }
        root.submitEmbyConnection(false)
    }

    function submitEmbyConnection(allowInsecureHttp) {
        app.session.login(serverName.text, serverUrl.text,
                          username.text, password.text,
                          allowInsecureHttp)
        password.text = ""
    }

    function restoreEmbyForm() {
        serverName.text = app.session.displayName.trim().length > 0
                ? app.session.displayName : qsTr("Home")
        serverUrl.text = app.session.serverUrl
        username.text = ""
        password.text = ""
    }

    function sectionItem(sectionIndex) {
        if (sectionIndex === 1)
            return languagePanel
        if (sectionIndex === 2)
            return playbackPanel
        if (sectionIndex === 3)
            return upscalingSection
        if (sectionIndex === 4)
            return controllerSection
        if (sectionIndex === 5)
            return developerSection
        return embyPanel
    }

    function scrollToSection(sectionIndex) {
        const section = root.sectionItem(sectionIndex)
        const sectionPosition = section.mapToItem(
            settingsFlickable.contentItem, 0, 0)
        const maximum = Math.max(0,
            settingsFlickable.contentHeight - settingsFlickable.height)
        const destination = Math.max(0, Math.min(maximum,
            sectionPosition.y - sectionScrollTopMargin))

        settingsFlickable.scrollToContentY(destination)
    }

    Component.onCompleted: {
        restoreEmbyForm()
        loadUpscalingSettings()
    }

    Connections {
        target: app.session

        function onStateChanged() {
            if (root.observedConnected === app.session.connected)
                return
            root.observedConnected = app.session.connected
            username.text = ""
            password.text = ""
            if (!app.session.connected)
                root.restoreEmbyForm()
        }
    }

    Connections {
        target: app.preferences

        function onUpscalingSettingsChanged() {
            root.loadUpscalingSettings()
        }
    }

    ControllerInputTestScope {
        id: controllerInputTestScope
        available: root.pageActive && root.activeSection === 4
    }

    Connections {
        target: InputModality

        function onControllerInputTestAction(action, repeated) {
            if (!controllerInputTestScope.acquired)
                return
            if (controllerInputTestScope.handleAction(action, repeated))
                return
            root.controllerActionCleared = false
            root.lastControllerAction = root.controllerActionLabel(action)
            root.lastControllerPrompt = InputModality.promptForAction(action)
            root.lastControllerActionRepeated = repeated
        }
    }

    AppConfirmDialog {
        id: insecureHttpConfirm
        onConfirmed: root.submitEmbyConnection(true)
        onRejected: password.text = ""
    }

    Item {
        id: settingsLayout
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(root.width,
                        root.sectionNavWidth + root.sectionNavGap + 920)

        GlassPanel {
            id: sectionSidebar

            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.topMargin: 18
            anchors.bottomMargin: 18
            width: root.sectionNavWidth
            radius: Theme.radiusLarge
            color: Theme.surface

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Text {
                    Layout.fillWidth: true
                    Layout.leftMargin: 10
                    Layout.rightMargin: 10
                    Layout.topMargin: 8
                    Layout.bottomMargin: 5
                    text: qsTr("PREFERENCES")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    font.letterSpacing: 1.4
                    elide: Text.ElideRight
                }

                Repeater {
                    id: sectionRepeater
                    model: [
                        qsTr("Emby server"),
                        qsTr("Language"),
                        qsTr("Playback"),
                        qsTr("Anime upscaling"),
                        qsTr("Controller"),
                        qsTr("Developer options")
                    ]

                    delegate: Button {
                        id: sectionButton

                        required property int index
                        required property string modelData
                        readonly property bool selected: index === root.activeSection

                        Layout.fillWidth: true
                        Layout.preferredHeight: 46
                        leftPadding: 14
                        rightPadding: 12
                        topPadding: 0
                        bottomPadding: 0
                        hoverEnabled: true
                        focusPolicy: Qt.StrongFocus
                        text: modelData
                        Accessible.name: text
                        onClicked: root.scrollToSection(index)
                        Keys.onPressed: event => {
                            if (event.key !== Qt.Key_Return
                                    && event.key !== Qt.Key_Enter)
                                return
                            sectionButton.click()
                            event.accepted = true
                        }

                        contentItem: Text {
                            text: sectionButton.text
                            color: sectionButton.selected
                                   ? Theme.accent
                                   : sectionButton.hovered ? Theme.text : Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 13
                            font.weight: sectionButton.selected ? Font.DemiBold : Font.Medium
                            horizontalAlignment: Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }

                        background: Rectangle {
                            radius: 13
                            color: sectionButton.selected
                                   ? Theme.accentSoft
                                   : sectionButton.hovered ? "#18FFFFFF" : "transparent"
                            border.width: sectionButton.visualFocus
                                ? 2 : (sectionButton.selected ? 1 : 0)
                            border.color: sectionButton.visualFocus
                                ? Theme.accent : "#52FF6687"
                        }
                    }
                }

                Item { Layout.fillHeight: true }
            }
        }

        SmoothFlickable {
            id: settingsFlickable

            anchors.left: sectionSidebar.right
            anchors.leftMargin: root.sectionNavGap
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            contentWidth: width
            contentHeight: Math.max(
                form.implicitHeight + 88,
                form.y + developerSection.y + height
                    - root.sectionScrollTopMargin)

            ColumnLayout {
                id: form
                x: Math.max(0, (settingsFlickable.width
                               - root.scrollBarGutter - width) / 2)
                y: 18
                width: Math.min(settingsFlickable.width
                                - root.scrollBarGutter, 920)
                spacing: 22

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                Text {
                    text: qsTr("PREFERENCES")
                    color: Theme.accent
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: 1.8
                }
                Text {
                    text: qsTr("Settings")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 38
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Manage your media server and playback defaults.")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 14
                }
            }

            GlassPanel {
                id: embyPanel

                Layout.fillWidth: true
                Layout.preferredHeight: embyLayout.implicitHeight + 56
                radius: Theme.radiusLarge

                ColumnLayout {
                    id: embyLayout
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 20

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14

                        Rectangle {
                            Layout.preferredWidth: 46
                            Layout.preferredHeight: 46
                            radius: 15
                            color: Theme.accentSoft
                            border.width: 1
                            border.color: "#52FF6687"
                            Text {
                                anchors.centerIn: parent
                                text: "E"
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 17
                                font.weight: Font.Bold
                            }
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Text {
                                text: qsTr("Emby server")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 19
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Connect any HTTP or HTTPS address, including reverse-proxy path prefixes.")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                        }
                    }

                    Item {
                        id: connectionBody

                        Layout.fillWidth: true
                        Layout.preferredHeight: root.embyConnectionProgress
                                                * connectedSummary.implicitHeight
                                                + (1 - root.embyConnectionProgress)
                                                * embyFormBody.implicitHeight
                        clip: true

                        Item {
                            id: connectedSection

                            width: parent.width
                            height: connectedSummary.implicitHeight
                            y: -8 * (1 - root.embyConnectionProgress)
                            visible: root.embyConnectionProgress > 0
                            enabled: app.session.connected
                            opacity: root.embyConnectionProgress

                            RowLayout {
                                id: connectedSummary
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                spacing: 18

                                Rectangle {
                                    Layout.preferredWidth: connectedStatus.implicitWidth + 28
                                    Layout.preferredHeight: 34
                                    radius: 17
                                    color: "#2074DBA4"
                                    border.width: 1
                                    border.color: "#5274DBA4"

                                    Row {
                                        id: connectedStatus
                                        anchors.centerIn: parent
                                        spacing: 8

                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 7
                                            height: 7
                                            radius: 4
                                            color: Theme.success
                                        }
                                        Text {
                                            text: qsTr("Connected")
                                            color: Theme.success
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }
                                }

                                Text {
                                    text: app.session.userName
                                    color: Theme.text
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                    Accessible.name: qsTr("Username: %1").arg(text)
                                }

                                Rectangle {
                                    Layout.preferredWidth: 1
                                    Layout.preferredHeight: 18
                                    color: Theme.outline
                                }

                                Text {
                                    Layout.fillWidth: true
                                    text: app.session.serverDomain
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 14
                                    elide: Text.ElideRight
                                    Accessible.name: qsTr("Server address: %1").arg(text)
                                }

                                AppButton {
                                    id: disconnectButton
                                    kind: "danger"
                                    text: qsTr("Disconnect")
                                    enabled: !app.session.busy
                                    onClicked: app.session.logout()
                                }
                            }
                        }

                        Item {
                            id: disconnectedSection

                            width: parent.width
                            height: embyFormBody.implicitHeight
                            y: 8 * root.embyConnectionProgress
                            visible: root.embyConnectionProgress < 1
                            enabled: !app.session.connected
                            opacity: 1 - root.embyConnectionProgress

                            ColumnLayout {
                                id: embyFormBody
                                anchors.left: parent.left
                                anchors.right: parent.right
                                anchors.top: parent.top
                                spacing: 20

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14
                                AppTextField {
                                    id: serverName
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 260
                                    label: qsTr("Display name")
                                    placeholderText: qsTr("Home")
                                    enabled: !app.session.busy
                                    onAccepted: root.connectEmby()
                                }
                                AppTextField {
                                    id: serverUrl
                                    Layout.fillWidth: true
                                    Layout.preferredWidth: 540
                                    label: qsTr("Server address")
                                    placeholderText: "https://media.example.com/emby"
                                    inputMethodHints: Qt.ImhUrlCharactersOnly
                                    enabled: !app.session.busy
                                    onAccepted: root.connectEmby()
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: serverUrl.text.trim().length > 0
                                text: serverUrl.text.trim().toLowerCase().startsWith("https://")
                                      ? qsTr("HTTPS connection: credentials are encrypted in transit.")
                                      : qsTr("HTTP connection: credentials are not encrypted. Trusted local networks only.")
                                color: serverUrl.text.trim().toLowerCase().startsWith("https://")
                                       ? Theme.success : Theme.danger
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14
                                AppTextField {
                                    id: username
                                    Layout.fillWidth: true
                                    label: qsTr("Username")
                                    placeholderText: qsTr("Your Emby username")
                                    enabled: !app.session.busy
                                    onAccepted: root.connectEmby()
                                }
                                AppTextField {
                                    id: password
                                    Layout.fillWidth: true
                                    label: qsTr("Password")
                                    placeholderText: qsTr("Your Emby password")
                                    echoMode: TextInput.Password
                                    enabled: !app.session.busy
                                    onAccepted: root.connectEmby()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true

                                Rectangle {
                                    Layout.preferredWidth: disconnectedStatus.implicitWidth + 28
                                    Layout.preferredHeight: 34
                                    radius: 17
                                    color: "#12FFFFFF"
                                    border.width: 1
                                    border.color: Theme.outline

                                    Row {
                                        id: disconnectedStatus
                                        anchors.centerIn: parent
                                        spacing: 8

                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 7
                                            height: 7
                                            radius: 4
                                            color: "#626B7A"
                                        }
                                        Text {
                                            text: qsTr("Not connected")
                                            color: Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                AppButton {
                                    kind: "primary"
                                    text: app.session.busy ? qsTr("Connecting…") : qsTr("Connect to Emby")
                                    enabled: !app.session.busy
                                             && serverName.text.trim().length > 0
                                             && serverUrl.text.trim().length > 0
                                             && username.text.trim().length > 0
                                    onClicked: root.connectEmby()
                                }
                            }
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: app.status.message.length > 0
                text: app.status.message
                color: app.status.error ? Theme.danger : Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }

            GlassPanel {
                id: languagePanel

                Layout.fillWidth: true
                Layout.preferredHeight: 118
                radius: Theme.radiusLarge

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 26
                    spacing: 20
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Text {
                            text: qsTr("Language")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: qsTr("Changes apply immediately and are remembered on this device.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 13
                        }
                    }
                    Item {
                        Layout.preferredWidth: root.trailingControlWidth
                        Layout.preferredHeight: languageActions.implicitHeight
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

                        RowLayout {
                            id: languageActions
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 10

                            AppButton {
                                kind: i18n.language === "en" ? "primary" : "secondary"
                                text: qsTr("English")
                                onClicked: {
                                    app.status.clear()
                                    i18n.setLanguage("en")
                                }
                            }
                            AppButton {
                                kind: i18n.language === "zh_CN" ? "primary" : "secondary"
                                text: qsTr("Simplified Chinese")
                                onClicked: {
                                    app.status.clear()
                                    i18n.setLanguage("zh_CN")
                                }
                            }
                        }
                    }
                }
            }

            GlassPanel {
                id: playbackPanel

                Layout.fillWidth: true
                Layout.preferredHeight: 118
                radius: Theme.radiusLarge

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 26
                    spacing: 20
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 5
                        Text {
                            text: qsTr("Playback")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 18
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: qsTr("Hardware decoding and original quality are preferred automatically.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 13
                        }
                    }
                    Item {
                        Layout.preferredWidth: root.trailingControlWidth
                        Layout.preferredHeight: 36
                        Layout.alignment: Qt.AlignRight | Qt.AlignVCenter

                        Rectangle {
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            width: playbackPill.implicitWidth + 28
                            height: 36
                            radius: 18
                            color: "#16FFFFFF"
                            border.width: 1
                            border.color: Theme.outline

                            Text {
                                id: playbackPill
                                anchors.centerIn: parent
                                text: qsTr("AUTO  ·  ORIGINAL")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.letterSpacing: 0.8
                            }
                        }
                    }
                }
            }

            GlassPanel {
                id: upscalingSection

                Layout.fillWidth: true
                Layout.preferredHeight: upscalingLayout.implicitHeight + 56
                radius: Theme.radiusLarge

                AppConfirmDialog {
                    id: upscalingDownloadConfirm

                    confirmKind: "primary"
                    onConfirmed: app.upscaling.downloadSelected()
                }

                ColumnLayout {
                    id: upscalingLayout

                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 22

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Rectangle {
                            Layout.preferredWidth: 48
                            Layout.preferredHeight: 48
                            radius: 16
                            color: Theme.accentSoft
                            border.width: 1
                            border.color: "#52FF6687"

                            Text {
                                anchors.centerIn: parent
                                text: "2×"
                                color: Theme.accent
                                font.family: Theme.fontFamily
                                font.pixelSize: 16
                                font.weight: Font.Bold
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Real-time anime upscaling")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 20
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Reconstruct cleaner lines and details while video is playing.")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 13
                                wrapMode: Text.WordWrap
                            }
                        }

                        ColumnLayout {
                            spacing: 5

                            CompactSwitch {
                                id: upscalingMasterSwitch

                                Layout.alignment: Qt.AlignRight
                                enabled: root.upscalingCanEnable
                                checked: root.upscalingEnabled
                                         && root.upscalingCanEnable
                                accessibleLabel: qsTr("Enable real-time anime upscaling")
                                onToggled: {
                                    root.upscalingEnabled = checked
                                    root.saveUpscalingSettings()
                                }
                            }
                            Text {
                                Layout.alignment: Qt.AlignHCenter
                                text: !app.upscaling.capabilityReady
                                      ? qsTr("Checking")
                                      : !root.upscalingAnime4KSupported
                                        ? qsTr("Unavailable")
                                        : root.upscalingEnabled ? qsTr("On") : qsTr("Off")
                                color: root.upscalingEnabled
                                       && root.upscalingCanEnable
                                       ? Theme.accent : Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        enabled: root.upscalingCanEnable
                        opacity: enabled ? 1 : 0.5
                        spacing: 14

                        Behavior on opacity { NumberAnimation { duration: 150 } }

                        Rectangle {
                            visible: !root.upscalingConfigurationAvailable
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible
                                                    ? unavailableLayout.implicitHeight + 28 : 0
                            radius: 16
                            color: "#0DFFFFFF"
                            border.width: 1
                            border.color: Theme.outline

                            ColumnLayout {
                                id: unavailableLayout
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 4

                                Text {
                                    Layout.fillWidth: true
                                    text: app.upscaling.capabilityReady
                                          ? qsTr("Anime4K unavailable")
                                          : qsTr("Checking playback renderer")
                                    color: Theme.text
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.upscalingUnavailableReason
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        RowLayout {
                            visible: root.upscalingConfigurationAvailable
                            Layout.fillWidth: true
                            Layout.topMargin: 4

                            Text {
                                text: qsTr("Preset")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                text: root.upscalingPresets[root.upscalingPreset].description
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        RowLayout {
                            visible: root.upscalingConfigurationAvailable
                            Layout.fillWidth: true
                            spacing: 10

                            Repeater {
                                model: root.upscalingPresets

                                delegate: AppButton {
                                    required property int index
                                    required property var modelData

                                    Layout.fillWidth: true
                                    checkable: true
                                    checked: root.upscalingPreset === index
                                    kind: checked ? "primary" : "secondary"
                                    text: modelData.name
                                    Accessible.description: modelData.description
                                    onClicked: root.selectUpscalingPreset(index)
                                }
                            }
                        }

                        Rectangle {
                            visible: root.upscalingConfigurationAvailable
                                     && root.upscalingPreset === 3
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible
                                                    ? advancedLayout.implicitHeight + 40 : 0
                            radius: 18
                            color: "#0CFFFFFF"
                            border.width: 1
                            border.color: Theme.outline

                            ColumnLayout {
                                id: advancedLayout

                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 17

                                RowLayout {
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3
                                        Text {
                                            text: qsTr("Advanced configuration")
                                            color: Theme.text
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 14
                                            font.weight: Font.DemiBold
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("Options shown here are specific to %1.")
                                                .arg("Anime4K")
                                            color: Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 11
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                    AppButton {
                                        kind: "ghost"
                                        controlSize: 36
                                        text: qsTr("Reset")
                                        onClicked: root.resetUpscalingCustomSettings()
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 12

                                    UpscalingChoiceRow {
                                        label: qsTr("Content mode")
                                        helpText: qsTr("Modes A and B use different restoration networks. Mode C combines upscaling with denoising; start with A and compare on difficult sources.")
                                        options: ["A", "B", "C"]
                                        selectedIndex: root.upscalingAnime4KContentMode
                                        onOptionSelected: index => {
                                            root.upscalingAnime4KContentMode = index
                                            root.saveUpscalingSettings()
                                        }
                                    }

                                    UpscalingChoiceRow {
                                        label: qsTr("CNN size")
                                        helpText: qsTr("Controls Anime4K model capacity from S to UL. Larger models can recover more detail but require more GPU time.")
                                        options: ["S", "M", "L", "VL", "UL"]
                                        selectedIndex: root.upscalingAnime4KModelSize
                                        onOptionSelected: index => {
                                            root.upscalingAnime4KModelSize = index
                                            root.saveUpscalingSettings()
                                        }
                                    }

                                    UpscalingChoiceRow {
                                        label: qsTr("Restore passes")
                                        helpText: qsTr("Two passes add a second neural processing stage. This can help difficult sources but increases GPU workload.")
                                        options: [qsTr("1 pass"), qsTr("2 passes")]
                                        selectedIndex: root.upscalingAnime4KRestorePasses
                                        onOptionSelected: index => {
                                            root.upscalingAnime4KRestorePasses = index
                                            root.saveUpscalingSettings()
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 14

                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 3

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 2

                                                Text {
                                                    text: qsTr("Automatic downscale")
                                                    color: Theme.text
                                                    font.family: Theme.fontForText(text)
                                                    font.pixelSize: 12
                                                    font.weight: Font.DemiBold
                                                }
                                                UpscalingInfoButton {
                                                    fieldLabel: qsTr("Automatic downscale")
                                                    helpText: qsTr("Adapts the Anime4K shader chain when the source is already close to the output size, reducing unnecessary processing.")
                                                }
                                                Item { Layout.fillWidth: true }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: qsTr("Avoid unnecessary shader passes when the source is already close to output size.")
                                                color: Theme.textMuted
                                                font.family: Theme.fontForText(text)
                                                font.pixelSize: 11
                                                wrapMode: Text.WordWrap
                                            }
                                        }
                                        CompactSwitch {
                                            checked: root.upscalingAnime4KAutoDownscale
                                            accessibleLabel: qsTr("Automatic downscale")
                                            onToggled: {
                                                root.upscalingAnime4KAutoDownscale = checked
                                                root.saveUpscalingSettings()
                                            }
                                        }
                                    }
                                }

                            }
                        }

                        Rectangle {
                            visible: root.upscalingConfigurationAvailable
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible
                                                    ? protectionLayout.implicitHeight + 40 : 0
                            radius: 18
                            color: "#0CFFFFFF"
                            border.width: 1
                            border.color: Theme.outline

                            ColumnLayout {
                                id: protectionLayout
                                anchors.fill: parent
                                anchors.margins: 20
                                spacing: 14

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 14

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 3
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 2

                                            Text {
                                                text: qsTr("Playback performance protection")
                                                color: Theme.text
                                                font.family: Theme.fontForText(text)
                                                font.pixelSize: 13
                                                font.weight: Font.DemiBold
                                            }
                                            UpscalingInfoButton {
                                                fieldLabel: qsTr("Playback performance protection")
                                                helpText: qsTr("Tracks playback smoothness. If Anime4K causes sustained frame drops, Yanami lowers its preset and can turn it off for that playback.")
                                            }
                                            Item { Layout.fillWidth: true }
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: qsTr("If playback starts dropping frames, Yanami lowers Anime4K first and temporarily turns it off only when needed.")
                                            color: Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 11
                                            wrapMode: Text.WordWrap
                                        }
                                    }
                                    CompactSwitch {
                                        checked: root.upscalingPerformanceProtection
                                        accessibleLabel: qsTr("Playback performance protection")
                                        Accessible.description: qsTr("Automatically lowers Anime4K or turns it off when playback cannot stay smooth.")
                                        onToggled: {
                                            root.upscalingPerformanceProtection = checked
                                            root.saveUpscalingSettings()
                                        }
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    opacity: root.upscalingPerformanceProtection ? 1 : 0.5

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Text {
                                            text: qsTr("Anime4K downgrade threshold")
                                            color: Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                        UpscalingInfoButton {
                                            fieldLabel: qsTr("Anime4K downgrade threshold")
                                            helpText: qsTr("Yanami continuously checks rendering time, output frame drops, and A/V sync. After three consecutive overloaded checks, it lowers Anime4K by one tier. If no lower tier remains, upscaling is disabled only for the current playback. This percentage sets the rendering-time threshold: 80% means a check is overloaded when rendering takes more than 80% of one frame (about 13.3 ms at 60 FPS).")
                                        }
                                        Item { Layout.fillWidth: true }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 12

                                        AppSlider {
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 160
                                            enabled: root.upscalingPerformanceProtection
                                            from: 60
                                            to: 90
                                            stepSize: 5
                                            snapMode: Slider.SnapAlways
                                            value: 100 - root.upscalingAutoHeadroom
                                            Accessible.name: qsTr("Anime4K downgrade threshold")
                                            Accessible.description: qsTr("Rendering is marked overloaded when it uses more than this percentage of one frame.")
                                            onMoved: {
                                                root.upscalingAutoHeadroom = 100 - value
                                                upscalingSaveTimer.restart()
                                            }
                                        }
                                        Text {
                                            Layout.preferredWidth: 42
                                            text: Math.round(
                                                100 - root.upscalingAutoHeadroom) + "%"
                                            color: Theme.text
                                            font.family: Theme.fontFamily
                                            font.pixelSize: 12
                                            horizontalAlignment: Text.AlignRight
                                        }
                                    }

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Layout.rightMargin: 54
                                        spacing: 8

                                        Text {
                                            text: qsTr("60% · Stricter")
                                            color: Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 10
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            text: qsTr("90% · More tolerant")
                                            color: Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 10
                                        }
                                    }
                                }
                            }
                        }

                        Rectangle {
                            visible: root.upscalingConfigurationAvailable
                                     && root.upscalingRequiresComponents
                            Layout.fillWidth: true
                            Layout.preferredHeight: visible ? 108 : 0
                            Layout.topMargin: 4
                            radius: 18
                            color: "#0DFFFFFF"
                            border.width: 1
                            border.color: root.upscalingDownloadState === 3
                                          ? "#4674DBA4" : Theme.outline

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 18
                                anchors.rightMargin: 18
                                spacing: 14

                                Rectangle {
                                    Layout.preferredWidth: 42
                                    Layout.preferredHeight: 42
                                    radius: 14
                                    color: root.upscalingDownloadState === 3
                                           ? "#2074DBA4" : "#14FFFFFF"
                                    border.width: 1
                                    border.color: root.upscalingDownloadState === 3
                                                  ? "#5274DBA4" : Theme.outline

                                    AppIcon {
                                        anchors.centerIn: parent
                                        width: 19
                                        height: 19
                                        name: root.upscalingDownloadState === 3
                                              ? "check" : "download"
                                        color: root.upscalingDownloadState === 3
                                               ? Theme.success : Theme.textMuted
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 4

                                    Text {
                                        Layout.fillWidth: true
                                        text: qsTr("Model components · %1 · %2")
                                              .arg(root.upscalingEffectiveProviderName)
                                              .arg(root.upscalingEffectivePresetName)
                                        color: Theme.text
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.upscalingDownloadState === 0
                                              ? qsTr("Download is required before this configuration can be used. Playback continues in original quality until it is ready.")
                                              : root.upscalingDownloadState === 1
                                                ? qsTr("Downloading · %1% complete")
                                                  .arg(Math.round(root.upscalingDownloadProgress))
                                                : root.upscalingDownloadState === 2
                                                  ? root.upscalingAssetPhase === "checking"
                                                    ? qsTr("Checking installed components…")
                                                    : qsTr("Verifying model components…")
                                                  : root.upscalingDownloadState === 4
                                                    ? (app.upscaling.selectedAssets.errorMessage
                                                       || qsTr("The component download failed. Try again."))
                                                    : qsTr("Ready · components verified")
                                        color: root.upscalingDownloadState === 3
                                               ? Theme.success : Theme.textMuted
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 11
                                        wrapMode: Text.WordWrap
                                    }
                                }

                                AppButton {
                                    visible: root.upscalingDownloadState === 0
                                             || root.upscalingDownloadState === 4
                                    kind: "primary"
                                    iconName: "download"
                                    text: root.upscalingDownloadState === 4
                                          ? qsTr("Try again")
                                          : qsTr("Download components")
                                    onClicked: upscalingDownloadConfirm.show(
                                        qsTr("Download upscaling components?"),
                                        qsTr("This configuration needs its model components before it can be used. Until the download and verification finish, videos will continue playing in original quality."),
                                        qsTr("Download"))
                                }

                                RowLayout {
                                    visible: root.upscalingDownloadState === 1
                                    spacing: 10

                                    Button {
                                        id: downloadProgressButton

                                        Layout.preferredWidth: 48
                                        Layout.preferredHeight: 48
                                        enabled: false
                                        hoverEnabled: false
                                        focusPolicy: Qt.NoFocus
                                        Accessible.role: Accessible.ProgressBar
                                        Accessible.name: qsTr("Model download · %1 percent")
                                            .arg(Math.round(root.upscalingDownloadProgress))

                                        contentItem: Item {
                                            Canvas {
                                                anchors.fill: parent
                                                property real progress: root.upscalingDownloadProgress
                                                property color trackColor: "#26FFFFFF"
                                                property color progressColor: Theme.accent
                                                onProgressChanged: requestPaint()
                                                onTrackColorChanged: requestPaint()
                                                onProgressColorChanged: requestPaint()
                                                Component.onCompleted: requestPaint()
                                                onPaint: {
                                                    const context = getContext("2d")
                                                    context.reset()
                                                    const center = width / 2
                                                    const radius = Math.max(0, center - 3)
                                                    context.lineWidth = 3
                                                    context.lineCap = "round"
                                                    context.strokeStyle = trackColor
                                                    context.beginPath()
                                                    context.arc(center, center, radius,
                                                                0, Math.PI * 2)
                                                    context.stroke()
                                                    context.strokeStyle = progressColor
                                                    context.beginPath()
                                                    context.arc(center, center, radius,
                                                                -Math.PI / 2,
                                                                -Math.PI / 2
                                                                + Math.PI * 2
                                                                * progress / 100)
                                                    context.stroke()
                                                }
                                            }
                                            TextMetrics {
                                                id: downloadProgressMetrics
                                                font: downloadProgressLabel.font
                                                renderType: downloadProgressLabel.renderType
                                                text: downloadProgressLabel.text
                                            }
                                            Text {
                                                id: downloadProgressLabel
                                                x: (parent.width - downloadProgressMetrics.tightBoundingRect.width) / 2
                                                   - downloadProgressMetrics.tightBoundingRect.x
                                                y: (parent.height - downloadProgressMetrics.tightBoundingRect.height) / 2
                                                   - baselineOffset
                                                   - downloadProgressMetrics.tightBoundingRect.y
                                                text: Math.round(root.upscalingDownloadProgress) + "%"
                                                color: Theme.text
                                                font.family: Theme.fontFamily
                                                font.pixelSize: 9
                                                font.weight: Font.Bold
                                            }
                                        }

                                        background: Rectangle {
                                            radius: width / 2
                                            color: "transparent"
                                        }
                                    }

                                    AppButton {
                                        kind: "ghost"
                                        controlSize: 36
                                        text: qsTr("Cancel")
                                        onClicked: app.upscaling.cancelSelected()
                                    }
                                }

                                Item {
                                    visible: root.upscalingDownloadState === 2
                                    Layout.preferredWidth: 48
                                    Layout.preferredHeight: 48

                                    LoadingIndicator {
                                        anchors.centerIn: parent
                                        width: 30
                                        height: 30
                                    }
                                }

                                Rectangle {
                                    visible: root.upscalingDownloadState === 3
                                    Layout.preferredWidth: readyLabel.implicitWidth + 24
                                    Layout.preferredHeight: 34
                                    radius: 17
                                    color: "#2074DBA4"
                                    border.width: 1
                                    border.color: "#5274DBA4"

                                    Text {
                                        id: readyLabel
                                        anchors.centerIn: parent
                                        text: qsTr("Installed")
                                        color: Theme.success
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                    }
                                }
                            }
                        }

                        Text {
                            visible: root.upscalingConfigurationAvailable
                            Layout.fillWidth: true
                            text: qsTr("Changes will apply to the next playback. A missing or unsupported component always falls back to original quality.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            GlassPanel {
                id: controllerSection
                objectName: "controllerDiagnosticsSection"

                readonly property var devices:
                    InputModality.connectedDevices || []
                Layout.fillWidth: true
                Layout.preferredHeight: controllerLayout.implicitHeight + 56
                radius: Theme.radiusLarge

                ColumnLayout {
                    id: controllerLayout
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 18

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Rectangle {
                            Layout.preferredWidth: 48
                            Layout.preferredHeight: 48
                            radius: 16
                            color: InputModality.controllerConnected
                                ? Theme.accentSoft : "#14FFFFFF"
                            border.width: 1
                            border.color: Theme.outline

                            AppIcon {
                                anchors.centerIn: parent
                                width: 22
                                height: 22
                                name: "gear"
                                color: InputModality.controllerConnected
                                    ? Theme.accent : Theme.textMuted
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Controller")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 19
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Connected devices switch automatically; the last meaningful input becomes active.")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                                wrapMode: Text.WordWrap
                            }
                        }

                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 104
                        radius: 18
                        color: "#0DFFFFFF"
                        border.width: 1
                        border.color: Theme.outline

                        GridLayout {
                            anchors.fill: parent
                            anchors.margins: 18
                            columns: 2
                            columnSpacing: 28
                            rowSpacing: 9

                            Text {
                                text: qsTr("Active device")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.fillWidth: true
                                text: InputModality.activeDeviceName || qsTr("None")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideMiddle
                            }
                            Text {
                                text: qsTr("Profile")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.controllerFamilyLabel(
                                    InputModality.activeDeviceFamily)
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignRight
                                elide: Text.ElideRight
                            }
                            Text {
                                text: qsTr("Input engine")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.fillWidth: true
                                text: String(InputModality.controllerBackend
                                             || "none").toUpperCase()
                                color: Theme.textMuted
                                font.family: Theme.fontFamily
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: controllerSection.devices.length > 0
                        spacing: 8

                        Text {
                            text: qsTr("Connected devices")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }

                        Repeater {
                            model: controllerSection.devices

                            delegate: Rectangle {
                                id: controllerDeviceRow
                                required property var modelData
                                required property int index

                                Layout.fillWidth: true
                                Layout.preferredHeight: 48
                                radius: 14
                                color: String(modelData.name || "")
                                    === String(InputModality.activeDeviceName || "")
                                    ? "#16FFFFFF" : "#09FFFFFF"
                                border.width: 1
                                border.color: Theme.outline

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 14
                                    anchors.rightMargin: 14
                                    spacing: 12

                                    Rectangle {
                                        Layout.preferredWidth: 8
                                        Layout.preferredHeight: 8
                                        radius: 4
                                        color: controllerDeviceRow.modelData.connected === false
                                            ? Theme.textMuted
                                            : String(controllerDeviceRow.modelData.supportTier || "")
                                                === "verified"
                                                ? Theme.success : Theme.info
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.controllerDeviceName(
                                            controllerDeviceRow.modelData)
                                        color: Theme.text
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                    }
                                    Text {
                                        text: root.controllerFamilyLabel(
                                            controllerDeviceRow.modelData.family)
                                        color: Theme.textMuted
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 10
                                    }
                                    Text {
                                        text: String(controllerDeviceRow.modelData[
                                            "back" + "end"]
                                                     || "none").toUpperCase()
                                        color: Theme.textMuted
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 108
                        radius: 18
                        color: "#0DFFFFFF"
                        border.width: 1
                        border.color: root.lastControllerAction.length > 0
                            ? "#52FF8FA7" : Theme.outline

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 18
                            spacing: 14

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 5

                                Text {
                                    text: controllerInputTestScope.running
                                        ? qsTr("Input test active · press any controller button")
                                        : qsTr("Input test · isolated controller diagnostics")
                                    color: Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 11
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: root.lastControllerAction.length > 0
                                        ? root.lastControllerAction
                                            + (root.lastControllerActionRepeated
                                               ? qsTr(" · repeating") : "")
                                        : (!root.controllerActionCleared
                                           && InputModality.lastActionName.length > 0
                                           ? root.controllerActionNameLabel(
                                                 InputModality.lastActionName)
                                           : qsTr("Waiting for input"))
                                    color: root.lastControllerAction.length > 0
                                        ? Theme.text : Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 14
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                Text {
                                    Layout.fillWidth: true
                                    text: controllerInputTestScope.running
                                        ? qsTr("Controller input is isolated · press B / Back to exit")
                                        : qsTr("Select Start test (A / OK) to prevent navigation while testing")
                                    color: controllerInputTestScope.running
                                        ? Theme.accent : Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                }
                            }

                            Rectangle {
                                visible: root.lastControllerPrompt.length > 0
                                Layout.preferredWidth: Math.max(
                                    36, controllerPrompt.implicitWidth + 18)
                                Layout.preferredHeight: 36
                                radius: 12
                                color: Theme.accentSoft
                                border.width: 1
                                border.color: "#52FF8FA7"

                                Text {
                                    id: controllerPrompt
                                    anchors.centerIn: parent
                                    text: root.lastControllerPrompt
                                    color: Theme.accent
                                    font.family: Theme.fontFamily
                                    font.pixelSize: 12
                                    font.weight: Font.Bold
                                }
                            }

                            AppButton {
                                kind: controllerInputTestScope.running
                                    ? "secondary" : "primary"
                                controlSize: 36
                                text: controllerInputTestScope.running
                                    ? qsTr("Stop test") : qsTr("Start test")
                                enabled: controllerInputTestScope.available
                                onClicked: {
                                    if (controllerInputTestScope.running) {
                                        controllerInputTestScope.stop()
                                    } else {
                                        root.controllerActionCleared = true
                                        root.lastControllerAction = ""
                                        root.lastControllerPrompt = ""
                                        root.lastControllerActionRepeated = false
                                        controllerInputTestScope.start()
                                    }
                                }
                            }

                            AppButton {
                                kind: "ghost"
                                controlSize: 36
                                text: qsTr("Clear")
                                enabled: root.lastControllerAction.length > 0
                                    || root.lastControllerPrompt.length > 0
                                    || (!root.controllerActionCleared
                                        && InputModality.lastActionName.length > 0)
                                onClicked: {
                                    root.controllerActionCleared = true
                                    root.lastControllerAction = ""
                                    root.lastControllerPrompt = ""
                                    root.lastControllerActionRepeated = false
                                }
                            }
                        }
                    }
                }
            }

            Item {
                id: developerSection

                property real expansionProgress: developerToggle.checked ? 1 : 0
                readonly property real expandedBodyHeight: credentialsLayout.implicitHeight + 56

                Layout.fillWidth: true
                implicitHeight: developerToggle.height
                                + expansionProgress * (14 + expandedBodyHeight)
                clip: true

                Behavior on expansionProgress {
                    NumberAnimation {
                        duration: 260
                        easing.type: Easing.OutCubic
                    }
                }

                Button {
                    id: developerToggle

                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 84
                    leftPadding: 20
                    rightPadding: 20
                    topPadding: 0
                    bottomPadding: 0
                    checkable: true
                    checked: false
                    hoverEnabled: true
                    Accessible.name: qsTr("Developer options")
                    Accessible.description: qsTr("Advanced settings for local development and service integrations.")
                    Keys.onPressed: event => {
                        if (event.key !== Qt.Key_Return
                                && event.key !== Qt.Key_Enter)
                            return
                        developerToggle.click()
                        event.accepted = true
                    }
                    onToggled: {
                        if (!checked)
                            forceActiveFocus()
                    }

                    contentItem: RowLayout {
                        spacing: 14

                        Rectangle {
                            Layout.preferredWidth: 42
                            Layout.preferredHeight: 42
                            radius: 13
                            color: developerToggle.checked ? Theme.accentSoft : "#16FFFFFF"
                            border.width: 1
                            border.color: developerToggle.checked ? "#52FF6687" : Theme.outline

                            AppIcon {
                                anchors.centerIn: parent
                                width: 19
                                height: 19
                                name: "settings"
                                color: developerToggle.checked ? Theme.accent : Theme.textMuted
                            }

                            Behavior on color { ColorAnimation { duration: 180 } }
                            Behavior on border.color { ColorAnimation { duration: 180 } }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4

                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Developer options")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 18
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: qsTr("Advanced settings for local development and service integrations.")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                        }

                        Rectangle {
                            Layout.preferredWidth: 34
                            Layout.preferredHeight: 34
                            radius: 17
                            color: developerToggle.hovered ? "#18FFFFFF" : "#0CFFFFFF"
                            border.width: 1
                            border.color: developerToggle.checked ? "#48FF6687" : Theme.outline

                            AppIcon {
                                anchors.centerIn: parent
                                width: 16
                                height: 16
                                name: "open"
                                color: developerToggle.checked ? Theme.accent : Theme.textMuted
                                rotation: developerToggle.checked ? 90 : 0

                                Behavior on rotation {
                                    NumberAnimation {
                                        duration: 220
                                        easing.type: Easing.OutCubic
                                    }
                                }
                            }

                            Behavior on color { ColorAnimation { duration: 160 } }
                            Behavior on border.color { ColorAnimation { duration: 180 } }
                        }
                    }

                    background: GlassPanel {
                        radius: Theme.radiusLarge
                        color: developerToggle.hovered || developerToggle.down
                               ? "#16FFFFFF" : Theme.surface
                        border.color: developerToggle.checked
                                      ? Theme.accent : Theme.outline

                        Behavior on border.color { ColorAnimation { duration: 180 } }
                    }
                }

                Item {
                    id: developerBody

                    x: 0
                    y: developerToggle.height + 14 * developerSection.expansionProgress
                    width: developerSection.width
                    height: developerSection.expandedBodyHeight * developerSection.expansionProgress
                    visible: developerSection.expansionProgress > 0
                    enabled: developerToggle.checked
                    opacity: developerSection.expansionProgress
                    clip: true

                    GlassPanel {
                        id: credentialsPanel

                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: developerSection.expandedBodyHeight
                        radius: Theme.radiusLarge

                        ColumnLayout {
                            id: credentialsLayout
                            anchors.fill: parent
                            anchors.margins: 28
                            spacing: 20

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14

                                Rectangle {
                                    Layout.preferredWidth: 46
                                    Layout.preferredHeight: 46
                                    radius: 15
                                    color: "#242B7FFF"
                                    border.width: 1
                                    border.color: "#4A7DA3FF"
                                    Text {
                                        anchors.centerIn: parent
                                        text: "D"
                                        color: "#91B1FF"
                                        font.family: Theme.fontFamily
                                        font.pixelSize: 17
                                        font.weight: Font.Bold
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 3
                                    Text {
                                        text: qsTr("DanDanPlay credentials")
                                        color: Theme.text
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 19
                                        font.weight: Font.DemiBold
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: app.danmaku.credentialSource === 1
                                              ? qsTr("This release includes project credentials. Developer credentials entered below take precedence.")
                                              : qsTr("Use developer credentials for local development. AppSecret is stored only in the operating-system credential vault.")
                                        color: Theme.textMuted
                                        font.family: Theme.fontForText(text)
                                        font.pixelSize: 13
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14
                                AppTextField {
                                    id: appId
                                    Layout.fillWidth: true
                                    label: "AppId"
                                    placeholderText: qsTr("Developer application ID")
                                }
                                AppTextField {
                                    id: appSecret
                                    Layout.fillWidth: true
                                    label: "AppSecret"
                                    placeholderText: qsTr("Developer application secret")
                                    echoMode: TextInput.Password
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Rectangle {
                                    Layout.preferredWidth: danmakuStatus.implicitWidth + 28
                                    Layout.preferredHeight: 34
                                    radius: 17
                                    color: app.danmaku.configured ? "#2074DBA4" : "#12FFFFFF"
                                    border.width: 1
                                    border.color: app.danmaku.configured ? "#5274DBA4" : Theme.outline
                                    Row {
                                        id: danmakuStatus
                                        anchors.centerIn: parent
                                        spacing: 8
                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 7
                                            height: 7
                                            radius: 4
                                            color: app.danmaku.configured ? Theme.success : "#626B7A"
                                        }
                                        Text {
                                            text: app.danmaku.credentialSource === 2
                                                  ? qsTr("Developer override")
                                                  : app.danmaku.credentialSource === 1
                                                    ? qsTr("Bundled release credentials")
                                                    : qsTr("Not configured")
                                            color: app.danmaku.configured ? Theme.success : Theme.textMuted
                                            font.family: Theme.fontForText(text)
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }
                                }
                                Item { Layout.fillWidth: true }
                                AppButton {
                                    visible: app.danmaku.credentialSource === 2
                                    kind: "danger"
                                    text: qsTr("Remove override")
                                    enabled: !app.danmaku.configurationBusy
                                    onClicked: app.danmaku.clearConfiguration()
                                }
                                AppButton {
                                    kind: "primary"
                                    text: app.danmaku.configurationBusy ? qsTr("Validating…") : qsTr("Validate and save")
                                    enabled: !app.danmaku.configurationBusy && appId.text.length > 0 && appSecret.text.length > 0
                                    onClicked: {
                                        app.danmaku.configure(appId.text, appSecret.text)
                                        appSecret.text = ""
                                    }
                                }
                            }
                        }
                    }
                }
            }
            }
        }
    }
}
