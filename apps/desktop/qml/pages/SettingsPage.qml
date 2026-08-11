import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami

Item {
    id: root

    Flickable {
        anchors.fill: parent
        contentWidth: width
        contentHeight: form.implicitHeight + 88
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        ColumnLayout {
            id: form
            x: Math.max(0, (root.width - width) / 2)
            y: 18
            width: Math.min(root.width, 920)
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
                    text: qsTr("Manage your media server, danmaku service and playback defaults.")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 14
                }
            }

            GlassPanel {
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

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        AppTextField {
                            id: serverName
                            Layout.fillWidth: true
                            Layout.preferredWidth: 260
                            label: qsTr("Display name")
                            placeholderText: qsTr("Home")
                            text: "Home"
                        }
                        AppTextField {
                            id: serverUrl
                            Layout.fillWidth: true
                            Layout.preferredWidth: 540
                            label: qsTr("Server address")
                            placeholderText: "https://media.example.com/emby"
                            inputMethodHints: Qt.ImhUrlCharactersOnly
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 14
                        AppTextField {
                            id: username
                            Layout.fillWidth: true
                            label: qsTr("Username")
                            placeholderText: qsTr("Your Emby username")
                        }
                        AppTextField {
                            id: password
                            Layout.fillWidth: true
                            label: qsTr("Password")
                            placeholderText: qsTr("Your Emby password")
                            echoMode: TextInput.Password
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true

                        Rectangle {
                            Layout.preferredWidth: connectionStatus.implicitWidth + 28
                            Layout.preferredHeight: 34
                            radius: 17
                            color: backend.embyConnected ? "#2074DBA4" : "#12FFFFFF"
                            border.width: 1
                            border.color: backend.embyConnected ? "#5274DBA4" : Theme.outline
                            Row {
                                id: connectionStatus
                                anchors.centerIn: parent
                                spacing: 8
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 7
                                    height: 7
                                    radius: 4
                                    color: backend.embyConnected ? Theme.success : "#626B7A"
                                }
                                Text {
                                    text: backend.embyConnected ? qsTr("Connected") : qsTr("Not connected")
                                    color: backend.embyConnected ? Theme.success : Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            visible: backend.embyConnected
                            kind: "danger"
                            text: qsTr("Disconnect")
                            enabled: !backend.busy
                            onClicked: backend.logoutEmby()
                        }
                        AppButton {
                            visible: !backend.embyConnected
                            kind: "primary"
                            text: backend.busy ? qsTr("Connecting…") : qsTr("Connect to Emby")
                            enabled: !backend.busy && serverUrl.text.length > 0 && username.text.length > 0
                            onClicked: {
                                backend.loginEmby(serverName.text, serverUrl.text, username.text, password.text)
                                password.text = ""
                            }
                        }
                    }
                }
            }

            GlassPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: credentialsLayout.implicitHeight + 56
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
                                text: qsTr("弹弹play credentials")
                                color: Theme.text
                                font.family: Theme.fontForText(text)
                                font.pixelSize: 19
                                font.weight: Font.DemiBold
                            }
                            Text {
                                Layout.fillWidth: true
                                text: backend.danmakuCredentialSource === 1
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
                            color: backend.danmakuConfigured ? "#2074DBA4" : "#12FFFFFF"
                            border.width: 1
                            border.color: backend.danmakuConfigured ? "#5274DBA4" : Theme.outline
                            Row {
                                id: danmakuStatus
                                anchors.centerIn: parent
                                spacing: 8
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 7
                                    height: 7
                                    radius: 4
                                    color: backend.danmakuConfigured ? Theme.success : "#626B7A"
                                }
                                Text {
                                    text: backend.danmakuCredentialSource === 2
                                          ? qsTr("Developer override")
                                          : backend.danmakuCredentialSource === 1
                                            ? qsTr("Bundled release credentials")
                                            : qsTr("Not configured")
                                    color: backend.danmakuConfigured ? Theme.success : Theme.textMuted
                                    font.family: Theme.fontForText(text)
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                        AppButton {
                            visible: backend.danmakuCredentialSource === 2
                            kind: "danger"
                            text: qsTr("Remove override")
                            enabled: !backend.busy
                            onClicked: backend.clearDandanplay()
                        }
                        AppButton {
                            kind: "primary"
                            text: backend.busy ? qsTr("Validating…") : qsTr("Validate and save")
                            enabled: !backend.busy && appId.text.length > 0 && appSecret.text.length > 0
                            onClicked: {
                                backend.configureDandanplay(appId.text, appSecret.text)
                                appSecret.text = ""
                            }
                        }
                    }
                }
            }

            Text {
                Layout.fillWidth: true
                visible: backend.statusMessage.length > 0
                text: backend.statusMessage
                color: backend.statusIsError ? Theme.danger : Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                wrapMode: Text.WordWrap
            }

            GlassPanel {
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
                    RowLayout {
                        spacing: 10
                        AppButton {
                            kind: i18n.language === "en" ? "primary" : "secondary"
                            text: qsTr("English")
                            onClicked: {
                                backend.clearStatus()
                                i18n.setLanguage("en")
                            }
                        }
                        AppButton {
                            kind: i18n.language === "zh_CN" ? "primary" : "secondary"
                            text: qsTr("Simplified Chinese")
                            onClicked: {
                                backend.clearStatus()
                                i18n.setLanguage("zh_CN")
                            }
                        }
                    }
                }
            }

            GlassPanel {
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
                    Rectangle {
                        Layout.preferredWidth: playbackPill.implicitWidth + 28
                        Layout.preferredHeight: 36
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
    }
}
