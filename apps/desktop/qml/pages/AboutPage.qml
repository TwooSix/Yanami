import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami.Ui

Item {
    id: root

    readonly property string repositoryUrl: "https://github.com/TwooSix/Yanami"
    readonly property string sponsorUrl: "https://afdian.com/a/twooosix"
    signal feedbackRequested(string message, string tone)

    function openExternalUrl(url, failureMessage) {
        if (!Qt.openUrlExternally(url))
            root.feedbackRequested(failureMessage, "error")
    }

    function updateStatusText() {
        if (app.updates.checking)
            return qsTr("Checking GitHub Releases…")
        if (!app.updates.hasChecked)
            return qsTr("Check whether a newer stable release is available.")
        if (app.updates.errorMessage.length > 0)
            return app.updates.errorMessage
        if (!app.updates.releaseFound)
            return qsTr("No published release was found yet.")
        if (app.updates.updateAvailable)
            return qsTr("Version %1 is available.").arg(app.updates.latestVersion)
        return qsTr("You are using the latest published version.")
    }

    SmoothFlickable {
        id: aboutFlickable
        anchors.fill: parent
        contentWidth: width
        contentHeight: aboutColumn.implicitHeight + 72

        ColumnLayout {
            id: aboutColumn
            x: Math.max(32, (aboutFlickable.width - width) / 2)
            y: 30
            width: Math.min(880, aboutFlickable.width - 64)
            spacing: 20

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                Text {
                    text: qsTr("ABOUT")
                    color: Theme.accent
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 11
                    font.weight: Font.Bold
                    font.letterSpacing: 1.8
                }
                Text {
                    text: qsTr("About Yanami")
                    color: Theme.text
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 38
                    font.weight: Font.DemiBold
                }
                Text {
                    text: qsTr("Software information, diagnostics, support, and updates.")
                    color: Theme.textMuted
                    font.family: Theme.fontForText(text)
                    font.pixelSize: 14
                }
            }

            GlassPanel {
                Layout.fillWidth: true
                Layout.preferredHeight: 214
                radius: Theme.radiusLarge

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 24

                    BrandMark {
                        Layout.preferredWidth: 104
                        Layout.preferredHeight: 104
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 7

                        Text {
                            text: "Yanami"
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 31
                            font.weight: Font.Bold
                        }
                        Text {
                            text: qsTr("Version %1").arg(app.updates.currentVersion)
                            color: Theme.accent
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("A modern desktop client for browsing and playing media from Emby.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 20

                GlassPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 202
                    radius: Theme.radiusLarge

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 10

                        Text {
                            text: qsTr("Project")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 19
                            font.weight: Font.DemiBold
                        }
                        Text {
                            text: qsTr("Author  ·  TwooSix")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                        }
                        Text {
                            text: qsTr("License  ·  GPL-3.0-or-later")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                        }
                        Item { Layout.fillHeight: true }
                        AppButton {
                            text: qsTr("Open project on GitHub")
                            iconName: "open"
                            onClicked: root.openExternalUrl(
                                root.repositoryUrl,
                                qsTr("Could not open the project page."))
                        }
                    }
                }

                GlassPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 202
                    radius: Theme.radiusLarge
                    border.color: app.updates.updateAvailable
                        ? "#70FF6687" : Theme.outline

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 10

                        Text {
                            text: qsTr("Software updates")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 19
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.updateStatusText()
                            color: app.updates.errorMessage.length > 0
                                ? Theme.danger
                                : (app.updates.updateAvailable
                                    ? Theme.accent : Theme.textMuted)
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            spacing: 10

                            AppButton {
                                text: app.updates.checking
                                    ? qsTr("Checking…") : qsTr("Check for updates")
                                iconName: "refresh"
                                iconSpinning: app.updates.checking
                                enabled: !app.updates.checking
                                onClicked: app.updates.check()
                            }
                            AppButton {
                                visible: app.updates.updateAvailable
                                text: qsTr("View release")
                                iconName: "open"
                                onClicked: root.openExternalUrl(
                                    app.updates.releaseUrl,
                                    qsTr("Could not open the release page."))
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 20

                GlassPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 250
                    radius: Theme.radiusLarge

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 9

                        Text {
                            text: qsTr("Diagnostics and feedback")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 19
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Export recent runtime logs as one file to help investigate a problem.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 13
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("Known credentials, URL details, and your home-folder path are hidden. Logs may still contain media titles or device details; review them before sharing.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }
                        Text {
                            Layout.fillWidth: true
                            visible: app.diagnostics.lastExportPath.length > 0
                                || app.diagnostics.errorMessage.length > 0
                            text: app.diagnostics.errorMessage.length > 0
                                ? app.diagnostics.errorMessage
                                : qsTr("Saved to %1").arg(
                                    app.diagnostics.lastExportPath)
                            color: app.diagnostics.errorMessage.length > 0
                                ? Theme.danger : Theme.success
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 11
                            wrapMode: Text.WrapAnywhere
                        }
                        Item { Layout.fillHeight: true }
                        RowLayout {
                            spacing: 10

                            AppButton {
                                text: app.diagnostics.exporting
                                    ? qsTr("Exporting…") : qsTr("Export logs")
                                iconName: app.diagnostics.exporting
                                    ? "refresh" : "download"
                                iconSpinning: app.diagnostics.exporting
                                enabled: !app.diagnostics.exporting
                                onClicked: app.diagnostics.exportLogs()
                            }
                            AppButton {
                                visible: app.diagnostics.lastExportPath.length > 0
                                text: qsTr("Open folder")
                                iconName: "open"
                                onClicked: app.diagnostics.openExportFolder()
                            }
                        }
                    }
                }

                GlassPanel {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 250
                    radius: Theme.radiusLarge

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 24
                        spacing: 10

                        Text {
                            text: qsTr("Support the project")
                            color: Theme.text
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 19
                            font.weight: Font.DemiBold
                        }
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("If Yanami is useful to you, you can sponsor the author and support its continued development.")
                            color: Theme.textMuted
                            font.family: Theme.fontForText(text)
                            font.pixelSize: 14
                            wrapMode: Text.WordWrap
                        }
                        Item { Layout.fillHeight: true }
                        AppButton {
                            text: qsTr("Sponsor the author")
                            kind: "primary"
                            iconName: "heart"
                            onClicked: root.openExternalUrl(
                                root.sponsorUrl,
                                qsTr("Could not open the sponsorship page."))
                        }
                    }
                }
            }

        }
    }

    Connections {
        target: app.diagnostics

        function onExportSucceeded(path) {
            root.feedbackRequested(
                qsTr("Diagnostics exported successfully."), "success")
        }

        function onExportFailed(message) {
            root.feedbackRequested(message, "error")
        }
    }
}
