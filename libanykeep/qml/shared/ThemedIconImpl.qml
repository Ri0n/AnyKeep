pragma ComponentBehavior: Bound

import QtQuick

Item {
    id: root

    required property string themeName
    required property string fallbackName
    property bool recolorFallback: false
    property string fallbackTintMode: "auto"
    property int pixelSize: 20
    readonly property real effectiveDevicePixelRatio: Math.max(1, Screen.devicePixelRatio)
    readonly property int rasterSize: Math.max(pixelSize,
                                                Math.ceil(pixelSize * effectiveDevicePixelRatio))
    readonly property string fallbackMode:
        recolorFallback
        ? (fallbackTintMode.length > 0 ? fallbackTintMode : "auto")
        : "original"
    // QML Image caches image-provider results by URL. Include the live
    // application palette in the URL so symbolic theme icons and auto-tinted
    // fallbacks are requested again after a desktop theme switch. The provider
    // intentionally ignores this final cache-key path segment.
    SystemPalette {
        id: systemPalette
    }
    readonly property string paletteCacheKey:
        String(systemPalette.window) + "-"
        + String(systemPalette.windowText) + "-"
        + String(systemPalette.button) + "-"
        + String(systemPalette.buttonText)
    readonly property url iconSource:
        "image://anykeepicons/" + encodeURIComponent(themeName)
        + "/" + encodeURIComponent(fallbackName)
        + "/" + encodeURIComponent(fallbackMode)
        + "/" + encodeURIComponent(paletteCacheKey + "-" + rasterSize)

    implicitWidth: pixelSize
    implicitHeight: pixelSize

    Image {
        anchors.centerIn: parent
        width: root.pixelSize
        height: root.pixelSize
        source: root.iconSource
        // The image provider returns a rasterized QIcon. Request enough
        // physical pixels for the screen while retaining pixelSize as the
        // logical layout size, otherwise fractional/high-DPI scaling blurs
        // even SVG-backed icons.
        sourceSize.width: root.rasterSize
        sourceSize.height: root.rasterSize
        fillMode: Image.PreserveAspectFit
        smooth: true
        opacity: root.enabled ? 1.0 : 0.38
    }
}
