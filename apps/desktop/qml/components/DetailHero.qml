import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import Yanami

GlassPanel {
    id: root

    property string eyebrow
    property string title
    property string subtitle
    property string overview
    property string continueLabel
    property url posterUrl
    property url backdropUrl
    signal playRequested()

    implicitHeight: 292
    radius: Theme.radiusLarge
    color: "#EE11141B"
    border.color: "#32FFFFFF"

    RoundedImage {
        anchors.fill: parent
        source: root.backdropUrl
        radius: root.radius
        opacity: status === Image.Ready ? 0.46 : 0

        Behavior on opacity { NumberAnimation { duration: 260 } }
    }

    Rectangle {
        anchors.fill: parent
        radius: root.radius
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "#F310131A" }
            GradientStop { position: 0.58; color: "#CF10131A" }
            GradientStop { position: 1; color: "#7A10131A" }
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 26

        Rectangle {
            Layout.preferredWidth: 158
            Layout.fillHeight: true
            radius: 20
            color: "#303846"
            border.width: 1
            border.color: "#32FFFFFF"

            RoundedImage {
                id: poster
                anchors.fill: parent
                source: root.posterUrl
                radius: parent.radius
                fillMode: Image.PreserveAspectCrop
            }

        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.topMargin: 5
            Layout.bottomMargin: 5
            spacing: 8

            Text {
                text: root.eyebrow.toUpperCase()
                color: Theme.accent
                font.family: Theme.fontForText(text)
                font.pixelSize: 11
                font.weight: Font.Bold
                font.letterSpacing: 1.3
            }

            Text {
                Layout.fillWidth: true
                text: root.title
                color: Theme.text
                font.family: Theme.fontForText(text)
                font.pixelSize: 31
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                text: root.subtitle
                color: "#C7CEDA"
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                font.weight: Font.Medium
                elide: Text.ElideRight
            }

            Text {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.maximumHeight: 76
                visible: root.overview.trim().length > 0
                text: root.overview
                color: Theme.textMuted
                font.family: Theme.fontForText(text)
                font.pixelSize: 13
                lineHeight: 1.35
                wrapMode: Text.Wrap
                maximumLineCount: 4
                elide: Text.ElideRight
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                AppButton {
                    kind: "primary"
                    iconName: "play"
                    text: root.continueLabel.length > 0 ? qsTr("Resume") : qsTr("Play")
                    onClicked: root.playRequested()
                }

                Column {
                    Layout.fillWidth: true
                    spacing: 2

                    Text {
                        visible: root.continueLabel.length > 0
                        text: qsTr("UP NEXT")
                        color: Theme.textMuted
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 10
                        font.weight: Font.Medium
                    }
                    Text {
                        width: parent.width
                        visible: root.continueLabel.length > 0
                        text: root.continueLabel
                        color: Theme.text
                        font.family: Theme.fontForText(text)
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
