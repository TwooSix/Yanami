pragma Singleton

import QtQuick

QtObject {
    readonly property color background: "#090B10"
    readonly property color surface: "#B8151820"
    readonly property color surfaceStrong: "#EB1B1E27"
    readonly property color surfaceHover: "#F0242833"
    readonly property color field: "#A80E1118"
    readonly property color outline: "#24FFFFFF"
    readonly property color outlineStrong: "#42FFFFFF"
    readonly property color text: "#F7F8FC"
    readonly property color textMuted: "#A2AABC"
    readonly property color accent: "#FF6687"
    readonly property color accentHover: "#FF7895"
    readonly property color accentSoft: "#2EFF6687"
    readonly property color success: "#74DBA4"
    readonly property color danger: "#FF879E"
    readonly property real radiusSmall: 12
    readonly property real radius: 20
    readonly property real radiusLarge: 28
    readonly property real spacing: 16
    readonly property string fontFamily: Qt.platform.os === "osx"
        ? ".AppleSystemUIFont"
        : (Qt.platform.os === "windows" ? "Segoe UI Variable Display" : "Inter")
    readonly property string cjkFontFamily: Qt.platform.os === "osx"
        ? "PingFang SC"
        : (Qt.platform.os === "windows" ? "Microsoft YaHei UI" : "Noto Sans CJK SC")

    function fontForText(value) {
        return /[\u2E80-\u9FFF\uF900-\uFAFF]/.test(value || "")
            ? cjkFontFamily
            : fontFamily
    }
}
