import QtQuick
import Yanami.Ui

Item {
    id: root

    implicitWidth: 46
    implicitHeight: 46

    Image {
        anchors.fill: parent
        source: Qt.resolvedUrl("../assets/yanami-logo.png")
        fillMode: Image.PreserveAspectFit
        smooth: true
        mipmap: true
    }
}
