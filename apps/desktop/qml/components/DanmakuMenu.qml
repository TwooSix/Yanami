import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui

AppTransientPopup {
    id: root

    property string animeSuggestion: ""
    property string episodeSuggestion: ""
    property string loadedTitle: ""
    property int commentCount: 0
    property string loadStatus: "idle"
    property var matches: []
    property var animeMatches: []
    property var selectedAnime: ({})
    property bool working: false
    readonly property real fontSize: preferences.fontSize
    readonly property real opacityValue: preferences.opacity
    readonly property real scrollDuration: preferences.scrollDuration
    readonly property real displayArea: preferences.displayArea
    readonly property int density: Math.round(preferences.density)
    readonly property real timeOffset: preferences.timeOffset
    readonly property string blockedTerms: preferences.blockedTerms
    readonly property bool showScroll: preferences.showScroll
    readonly property bool showTop: preferences.showTop
    readonly property bool showBottom: preferences.showBottom
    readonly property real danmakuTopMargin: preferences.topMargin
    readonly property bool pointerInside: popupHover.hovered

    signal searchRequested(string anime)
    signal matchRequested(var match, var style)
    signal styleRequested(var style)

    initialFocusTarget: styleRepeater.count > 0
        ? styleRepeater.itemAt(0).focusTarget : animeInput

    function stylePayload() {
        return {
            "fontSize": preferences.fontSize,
            "opacity": preferences.opacity,
            "scrollDuration": preferences.scrollDuration,
            "displayArea": preferences.displayArea,
            "density": Math.round(preferences.density),
            "timeOffset": preferences.timeOffset,
            "blockedTerms": preferences.blockedTerms,
            "showScroll": preferences.showScroll,
            "showTop": preferences.showTop,
            "showBottom": preferences.showBottom,
            "topMargin": preferences.topMargin
        }
    }

    function persistStyle() {
        app.preferences.saveDanmakuStyle(stylePayload())
    }

    function formattedStyleValue(key, value) {
        if (key === "fontSize")
            return Math.round(value) + " px"
        if (key === "opacity" || key === "displayArea")
            return Math.round(value * 100) + "%"
        if (key === "scrollDuration")
            return Number(value).toFixed(1) + " s"
        if (key === "topMargin")
            return Math.round(value) + " px"
        return String(Math.round(value))
    }

    function previewDevelopmentStyle(value) {
        preferences.fontSize = Math.max(18, Math.min(72, Number(value)))
        preferences.opacity = 0.55
        root.styleRequested(root.stylePayload())
    }

    function setSuggestions(anime, episode) {
        animeSuggestion = anime || ""
        episodeSuggestion = episode || ""
        if (!animeInput.activeFocus)
            animeInput.text = animeSuggestion
    }

    function showAnimeResults(animes) {
        animeMatches = animes || []
        selectedAnime = ({})
        Qt.callLater(root.revealManualResults)
    }

    function chooseAnime(anime) {
        selectedAnime = anime || ({})
        Qt.callLater(root.revealManualResults)
    }

    function revealManualResults() {
        manualRevealTimer.restart()
    }

    function revealFocusItem(item) {
        if (!item)
            return
        const position = item.mapToItem(content, 0, 0)
        const top = position.y - 12
        const bottom = position.y + item.height + 12
        if (top < scroller.contentY)
            scroller.revealContentY(top)
        else if (bottom > scroller.contentY + scroller.height)
            scroller.revealContentY(bottom - scroller.height)
    }

    function activateMatch(match) {
        if (root.selectedAnime.animeTitle === undefined) {
            root.chooseAnime(match)
        } else {
            root.matchRequested({
                "episodeId": match.episodeId,
                "animeTitle": root.selectedAnime.animeTitle || "",
                "episodeTitle": match.episodeTitle || ""
            }, root.stylePayload())
        }
    }

    width: 412
    height: 570
    padding: 12

    QtObject {
        id: preferences
        property real fontSize: 42
        property real opacity: 0.88
        property real scrollDuration: 9
        property real displayArea: 0.70
        property real density: 14
        property real timeOffset: 0
        property string blockedTerms: ""
        property bool showScroll: true
        property bool showTop: true
        property bool showBottom: true
        property real topMargin: 0
    }

    Component.onCompleted: {
        const saved = app.preferences.danmakuStyle
        preferences.fontSize = Number(saved.fontSize || 42)
        preferences.opacity = Number(saved.opacity || 0.88)
        preferences.scrollDuration = Number(saved.scrollDuration || 9)
        preferences.displayArea = Number(saved.displayArea || 0.70)
        preferences.density = Number(saved.density || 14)
        preferences.timeOffset = Number(saved.timeOffset || 0)
        preferences.blockedTerms = String(saved.blockedTerms || "")
        preferences.showScroll = saved.showScroll === undefined ? true : Boolean(saved.showScroll)
        preferences.showTop = saved.showTop === undefined ? true : Boolean(saved.showTop)
        preferences.showBottom = saved.showBottom === undefined ? true : Boolean(saved.showBottom)
        preferences.topMargin = Number(saved.topMargin || 0)
    }

    background: Rectangle {
        radius: 22
        color: "#F51A1D26"
        border.width: 1
        border.color: "#48FFFFFF"
    }

    // Own controller/remote navigation and semantic scrolling in exactly one
    // place. A second InputModality connection here would apply every right-
    // stick/page action twice.
    PopupControllerNavigator { popup: root }

    HoverHandler {
        id: popupHover
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }

    contentItem: SmoothFlickable {
        id: scroller
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        flickDeceleration: 1300
        maximumFlickVelocity: 6500

        Timer {
            id: manualRevealTimer
            interval: 100
            onTriggered: {
                const maximum = Math.max(0, scroller.contentHeight - scroller.height)
                scroller.scrollToContentY(maximum)
            }
        }

        ColumnLayout {
            id: content
            x: Theme.scrollContentInset
            width: Math.max(0, scroller.width - 2 * Theme.scrollContentInset)
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                Text {
                    Layout.fillWidth: true
                    text: qsTr("Danmaku")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                BusyIndicator {
                    visible: root.working
                    running: visible
                    implicitWidth: 22
                    implicitHeight: 22
                    palette.highlight: Theme.accent
                }
            }

            Text {
                Layout.fillWidth: true
                text: root.loadedTitle.length > 0
                    ? qsTr("%1 · %2 comments").arg(root.loadedTitle).arg(root.commentCount)
                    : root.loadStatus === "choice-required"
                        ? qsTr("Confirm the correct episode before loading")
                        : root.loadStatus === "no-match"
                            ? qsTr("No confident automatic match. Try a manual search.")
                            : qsTr("Matching continues in the background while video plays.")
                color: root.loadedTitle.length > 0 ? Theme.success : Theme.textMuted
                wrapMode: Text.WordWrap
                font.family: Theme.fontForText(text)
                font.pixelSize: 12
            }

            Text {
                text: qsTr("Appearance")
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Repeater {
                id: styleRepeater
                model: [
                    { "label": qsTr("Size"), "from": 18, "to": 72, "step": 1, "key": "fontSize" },
                    { "label": qsTr("Opacity"), "from": 0.2, "to": 1, "step": 0.01, "key": "opacity" },
                    { "label": qsTr("Scroll duration"), "from": 20, "to": 3, "step": 0.1, "key": "scrollDuration" },
                    { "label": qsTr("Display area"), "from": 0.25, "to": 1, "step": 0.01, "key": "displayArea" },
                    { "label": qsTr("Density"), "from": 4, "to": 30, "step": 1, "key": "density" },
                    { "label": qsTr("Top margin"), "from": 0, "to": 240, "step": 2, "key": "topMargin" }
                ]
                delegate: RowLayout {
                    required property var modelData
                    property alias focusTarget: styleSlider
                    Layout.fillWidth: true
                    spacing: 10
                    Text {
                        Layout.preferredWidth: 82
                        text: modelData.label
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                    }
                    AppSlider {
                        id: styleSlider
                        property bool controllerConsumesHorizontalNavigation: true
                        Layout.fillWidth: true
                        from: modelData.from
                        to: modelData.to
                        stepSize: modelData.step
                        snapMode: Slider.SnapAlways
                        value: preferences[modelData.key]
                        Accessible.name: modelData.label
                        onActiveFocusChanged: {
                            if (activeFocus)
                                root.revealFocusItem(styleSlider)
                        }
                        onMoved: {
                            preferences[modelData.key] = value
                            root.styleRequested(root.stylePayload())
                            stylePersistTimer.restart()
                        }
                        onPressedChanged: {
                            if (!pressed) {
                                stylePersistTimer.stop()
                                root.persistStyle()
                            }
                        }
                    }
                    Text {
                        Layout.preferredWidth: 54
                        horizontalAlignment: Text.AlignRight
                        text: root.formattedStyleValue(
                            modelData.key, preferences[modelData.key])
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 12
                        font.features: { "tnum": 1 }
                    }
                }
            }

            Timer {
                id: stylePersistTimer
                interval: 260
                onTriggered: root.persistStyle()
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text {
                    Layout.preferredWidth: 82
                    text: qsTr("Types")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
                Repeater {
                    model: [
                        { "key": "showScroll", "icon": "danmaku-scroll", "tip": qsTr("Scrolling danmaku") },
                        { "key": "showTop", "icon": "danmaku-top", "tip": qsTr("Top danmaku") },
                        { "key": "showBottom", "icon": "danmaku-bottom", "tip": qsTr("Bottom danmaku") }
                    ]
                    delegate: AppButton {
                        required property var modelData
                        kind: "secondary"
                        iconOnly: true
                        iconName: modelData.icon
                        iconSize: 19
                        controlSize: 38
                        checkable: true
                        checked: Boolean(preferences[modelData.key])
                        Accessible.name: modelData.tip
                        toolTipText: modelData.tip
                        toolTipDelay: 450
                        onToggled: {
                            preferences[modelData.key] = checked
                            root.styleRequested(root.stylePayload())
                            root.persistStyle()
                        }
                    }
                }
                Item { Layout.fillWidth: true }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text {
                    Layout.preferredWidth: 82
                    text: qsTr("Offset")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 12
                }
                SpinBox {
                    id: offsetInput
                    Layout.fillWidth: true
                    implicitHeight: 42
                    from: -600
                    to: 600
                    stepSize: 1
                    value: Math.round(preferences.timeOffset)
                    editable: true
                    onValueModified: {
                        preferences.timeOffset = value
                        root.persistStyle()
                        root.styleRequested(root.stylePayload())
                    }
                    contentItem: TextInput {
                        property bool controllerConsumesHorizontalNavigation: true
                        z: 2
                        text: offsetInput.textFromValue(offsetInput.value, offsetInput.locale)
                        color: Theme.text
                        selectionColor: Theme.accent
                        selectedTextColor: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        readOnly: !offsetInput.editable
                        validator: offsetInput.validator
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                        font.family: Theme.fontFamily
                        font.pixelSize: 13
                        Keys.onPressed: event => {
                            if (InputModality.modality !== InputModality.Controller
                                    && InputModality.modality !== InputModality.Remote)
                                return
                            if (event.key === Qt.Key_Left) {
                                offsetInput.decrease()
                                event.accepted = true
                            } else if (event.key === Qt.Key_Right) {
                                offsetInput.increase()
                                event.accepted = true
                            }
                        }
                    }
                    background: Rectangle {
                        radius: 12
                        color: "#14FFFFFF"
                        border.width: 1
                        border.color: offsetInput.activeFocus ? Theme.accent : Theme.outline
                    }
                    down.indicator: Rectangle {
                        x: 1; y: 1
                        width: 42; height: parent.height - 2
                        radius: 11
                        color: downMouse.containsMouse ? "#20FFFFFF" : "transparent"
                        Text { anchors.centerIn: parent; text: "−"; color: Theme.text; font.pixelSize: 20 }
                        MouseArea {
                            id: downMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: offsetInput.decrease()
                        }
                    }
                    up.indicator: Rectangle {
                        x: parent.width - width - 1; y: 1
                        width: 42; height: parent.height - 2
                        radius: 11
                        color: upMouse.containsMouse ? "#20FFFFFF" : "transparent"
                        Text { anchors.centerIn: parent; text: "+"; color: Theme.text; font.pixelSize: 18 }
                        MouseArea {
                            id: upMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: offsetInput.increase()
                        }
                    }
                }
                Text {
                    text: qsTr("sec")
                    color: Theme.textMuted
                    font.pixelSize: 12
                }
            }

            TextField {
                id: blockedTermsInput
                Layout.fillWidth: true
                placeholderText: qsTr("Blocked words, separated by commas")
                text: preferences.blockedTerms
                color: Theme.text
                placeholderTextColor: Theme.textMuted
                selectByMouse: true
                onEditingFinished: {
                    preferences.blockedTerms = text
                    root.persistStyle()
                    root.styleRequested(root.stylePayload())
                }
                Keys.priority: Keys.BeforeItem
                Keys.onPressed: event => {
                    if (InputModality.modality !== InputModality.Controller
                            && InputModality.modality !== InputModality.Remote)
                        return
                    if (event.key === Qt.Key_Menu) {
                        blockedTermsInput.clear()
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
                    const target = blockedTermsInput.nextItemInFocusChain(forward)
                    if (target && target !== blockedTermsInput) {
                        target.forceActiveFocus(forward
                            ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                        event.accepted = true
                    }
                }
                background: Rectangle {
                    radius: 12
                    color: "#14FFFFFF"
                    border.width: 1
                    border.color: parent.activeFocus ? Theme.accent : Theme.outline
                }
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#20FFFFFF" }

            Text {
                text: qsTr("Manual match")
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: animeInput
                    Layout.fillWidth: true
                    text: root.animeSuggestion
                    placeholderText: qsTr("Anime title")
                    color: Theme.text
                    placeholderTextColor: Theme.textMuted
                    selectByMouse: true
                    onAccepted: root.searchRequested(text)
                    Keys.priority: Keys.BeforeItem
                    Keys.onPressed: event => {
                        if (InputModality.modality !== InputModality.Controller
                                && InputModality.modality !== InputModality.Remote)
                            return
                        if (event.key === Qt.Key_Menu) {
                            animeInput.clear()
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
                        const target = animeInput.nextItemInFocusChain(forward)
                        if (target && target !== animeInput) {
                            target.forceActiveFocus(forward
                                ? Qt.TabFocusReason : Qt.BacktabFocusReason)
                            event.accepted = true
                        }
                    }
                    background: Rectangle {
                        radius: 12; color: "#14FFFFFF"; border.width: 1
                        border.color: parent.activeFocus ? Theme.accent : Theme.outline
                    }
                }
                AppButton {
                    id: searchButton
                    kind: "secondary"
                    text: qsTr("Search")
                    controlSize: 40
                    enabled: !root.working && animeInput.text.trim().length > 0
                    onClicked: root.searchRequested(animeInput.text)
                }
            }

            RowLayout {
                visible: root.selectedAnime.animeTitle !== undefined
                Layout.fillWidth: true
                spacing: 8
                AppButton {
                    id: animeBackButton
                    kind: "ghost"
                    iconOnly: true
                    iconName: "back"
                    controlSize: 34
                    onClicked: root.selectedAnime = ({})
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Text {
                        Layout.fillWidth: true
                        text: String(root.selectedAnime.animeTitle || qsTr("Unknown anime"))
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: String(root.selectedAnime.typeDescription || "").length > 0
                        text: String(root.selectedAnime.typeDescription || "")
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
            }

            Column {
                id: matchList

                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                visible: matchRepeater.count > 0
                spacing: 4

                Repeater {
                    id: matchRepeater
                    model: root.selectedAnime.animeTitle !== undefined
                        ? (root.selectedAnime.episodes || [])
                        : root.animeMatches

                    delegate: Rectangle {
                        id: matchRow
                        required property var modelData
                        width: matchList.width
                        height: 54
                        activeFocusOnTab: true
                        radius: 12
                        color: matchRow.activeFocus || matchMouse.containsMouse
                            ? "#20FFFFFF" : "#0DFFFFFF"
                        border.width: matchRow.activeFocus ? 2 : 0
                        border.color: Theme.accent
                        Accessible.role: Accessible.ListItem
                        Accessible.name: root.selectedAnime.animeTitle !== undefined
                            ? String(modelData.episodeTitle || qsTr("Unknown episode"))
                            : String(modelData.animeTitle || qsTr("Unknown anime"))
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter
                                    || event.key === Qt.Key_Space) {
                                root.activateMatch(matchRow.modelData)
                                event.accepted = true
                            }
                        }
                        onActiveFocusChanged: {
                            if (activeFocus)
                                root.revealFocusItem(matchRow)
                        }
                        Column {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 12
                            spacing: 2
                            Text {
                                width: parent.width
                                text: root.selectedAnime.animeTitle !== undefined
                                    ? (modelData.episodeTitle || qsTr("Unknown episode"))
                                    : (modelData.animeTitle || qsTr("Unknown anime"))
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 12
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Text {
                                width: parent.width
                                text: root.selectedAnime.animeTitle !== undefined
                                    ? qsTr("Click to load this episode")
                                    : qsTr("%1 episodes%2")
                                        .arg((modelData.episodes || []).length)
                                        .arg(modelData.typeDescription
                                             ? " 路 " + modelData.typeDescription : "")
                                color: Theme.textMuted
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }
                        MouseArea {
                            id: matchMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.activateMatch(matchRow.modelData)
                        }
                    }
                }
            }

            Column {
                id: directMatchList

                Layout.fillWidth: true
                Layout.preferredHeight: implicitHeight
                visible: directMatchRepeater.count > 0
                    && root.animeMatches.length === 0
                spacing: 4

                Repeater {
                    id: directMatchRepeater
                    model: root.matches

                    delegate: Rectangle {
                        id: directMatchRow
                        required property var modelData
                        width: directMatchList.width
                        height: 52
                        activeFocusOnTab: true
                        radius: 12
                        color: directMatchRow.activeFocus
                                || directMatchMouse.containsMouse
                            ? "#20FFFFFF" : "#0DFFFFFF"
                        border.width: directMatchRow.activeFocus ? 2 : 0
                        border.color: Theme.accent
                        Accessible.role: Accessible.ListItem
                        Accessible.name: (modelData.animeTitle || qsTr("Unknown anime"))
                            + " · " + (modelData.episodeTitle
                                || qsTr("Unknown episode"))
                        Keys.onPressed: event => {
                            if (event.key === Qt.Key_Return
                                    || event.key === Qt.Key_Enter
                                    || event.key === Qt.Key_Space) {
                                root.matchRequested(directMatchRow.modelData,
                                                    root.stylePayload())
                                event.accepted = true
                            }
                        }
                        onActiveFocusChanged: {
                            if (activeFocus)
                                root.revealFocusItem(directMatchRow)
                        }
                        Text {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.margins: 12
                            text: (modelData.animeTitle || qsTr("Unknown anime"))
                                + " 路 " + (modelData.episodeTitle || qsTr("Unknown episode"))
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        MouseArea {
                            id: directMatchMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.matchRequested(
                                directMatchRow.modelData, root.stylePayload())
                        }
                    }
                }
            }
        }

    }
}
