import QtQuick
import QtQuick.Effects

Item {
    id: root

    property alias source: sourceImage.source
    property alias fillMode: sourceImage.fillMode
    property alias sourceSize: sourceImage.sourceSize
    property alias cache: sourceImage.cache
    property alias asynchronous: sourceImage.asynchronous
    property real radius: 0
    readonly property int status: sourceImage.status

    Rectangle {
        id: mask
        anchors.fill: parent
        radius: root.radius
        color: "white"
        visible: false
        layer.enabled: true
    }

    Image {
        id: sourceImage
        anchors.fill: parent
        asynchronous: true
        cache: true
        fillMode: Image.PreserveAspectCrop
        layer.enabled: root.radius > 0
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
    }
}
