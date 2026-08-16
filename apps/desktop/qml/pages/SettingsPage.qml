import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui

Item {
    id: root

    property real embyConnectionProgress: app.session.connected ? 1 : 0
    property bool observedConnected: app.session.connected
    readonly property real trailingControlWidth: 300
    readonly property real sectionNavWidth: Math.min(176, Math.max(140, width * 0.19))
    readonly property real sectionNavGap: 18
    readonly property real sectionScrollTopMargin: 18
    readonly property real scrollBarGutter: 18
    readonly property int activeSection: {
        const probeY = settingsFlickable.contentY + sectionScrollTopMargin + 20
        if (settingsFlickable.contentY > 0.5 && settingsFlickable.atYEnd)
            return 3
        if (probeY >= form.y + developerSection.y)
            return 3
        if (probeY >= form.y + playbackPanel.y)
            return 2
        if (probeY >= form.y + languagePanel.y)
            return 1
        return 0
    }

    Behavior on embyConnectionProgress {
        NumberAnimation {
            duration: 280
            easing.type: Easing.OutCubic
        }
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

    Component.onCompleted: restoreEmbyForm()

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
                    model: [
                        qsTr("Emby server"),
                        qsTr("Language"),
                        qsTr("Playback"),
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
                            border.width: sectionButton.selected || sectionButton.visualFocus ? 1 : 0
                            border.color: sectionButton.selected
                                          ? "#52FF6687" : Theme.outlineStrong
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
