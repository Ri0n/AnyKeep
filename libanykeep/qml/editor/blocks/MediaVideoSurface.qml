import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import "../../shared" as Shared

Item {
    id: root
    property var playback: null
    property var mediaBlock: null
    property var attachedPlayback: null
    property bool useHostWindow: Qt.platform.os === "android"
    property Window fullScreenHost: null
    property int previousVisibility: Window.Windowed
    readonly property bool fullScreen: fullScreenHost !== null || fullScreenWindow.visible
    readonly property bool waitingForVideo: playback && (playback.loading || playback.seeking)

    function attach() {
        if (attachedPlayback === playback)
            return
        if (attachedPlayback) {
            attachedPlayback.detachVideoOutput(inlineVideo)
            attachedPlayback.detachVideoOutput(fullScreenVideo)
        }
        attachedPlayback = playback
        if (attachedPlayback)
            attachedPlayback.attachVideoOutput(fullScreen ? fullScreenVideo : inlineVideo)
    }

    function openFullScreen() {
        if (!attachedPlayback || fullScreen)
            return
        const sourceWindow = root.Window.window
        if (useHostWindow && sourceWindow) {
            // Android delivers touch to the activity's Qt window. Present
            // fullscreen in that scene, above the editor and its hit areas.
            previousVisibility = sourceWindow.visibility
            fullScreenHost = sourceWindow
            sourceWindow.showFullScreen()
        } else {
            if (sourceWindow && sourceWindow.screen)
                fullScreenWindow.screen = sourceWindow.screen
            fullScreenWindow.showFullScreen()
            fullScreenWindow.requestActivate()
        }
        attachedPlayback.attachVideoOutput(fullScreenVideo)
        fullScreenContent.forceActiveFocus()
    }

    function closeFullScreen() {
        if (!fullScreen)
            return
        if (attachedPlayback) {
            attachedPlayback.attachVideoOutput(inlineVideo)
            attachedPlayback.detachVideoOutput(fullScreenVideo)
        }
        if (fullScreenHost) {
            fullScreenHost.visibility = previousVisibility
            fullScreenHost = null
        } else {
            fullScreenWindow.hide()
        }
        const sourceWindow = root.Window.window
        if (sourceWindow)
            sourceWindow.requestActivate()
        root.forceActiveFocus()
    }

    onPlaybackChanged: attach()
    Component.onCompleted: attach()
    Component.onDestruction: {
        if (fullScreenHost)
            fullScreenHost.visibility = previousVisibility
        if (attachedPlayback) {
            attachedPlayback.detachVideoOutput(inlineVideo)
            attachedPlayback.detachVideoOutput(fullScreenVideo)
        }
    }

    VideoOutput {
        id: inlineVideo
        anchors.fill: parent
        fillMode: VideoOutput.PreserveAspectFit
    }

    Rectangle {
        anchors.centerIn: parent
        width: 80
        height: 80
        radius: 40
        color: "#90000000"
        visible: root.waitingForVideo && !root.fullScreen
        BusyIndicator {
            objectName: "inlineVideoBusyIndicator"
            anchors.centerIn: parent
            width: 64
            height: 64
            running: parent.visible
            Accessible.name: root.playback && root.playback.seeking ? qsTr("Seeking video") : qsTr("Loading video")
        }
    }

    Window {
        id: fullScreenWindow
        objectName: "mediaFullScreenWindow"
        visible: false
        color: "black"
        flags: Qt.Window | Qt.FramelessWindowHint
        title: qsTr("Video")
        onActiveChanged: if (active) fullScreenContent.forceActiveFocus()

        onClosing: function(close) {
            close.accepted = false
            root.closeFullScreen()
        }

        FocusScope {
            id: fullScreenContent
            parent: root.fullScreenHost ? root.Window.contentItem : fullScreenWindow.contentItem
            anchors.fill: parent
            visible: root.fullScreen
            z: 100
            Rectangle {
                anchors.fill: parent
                color: "black"
                z: -1
            }
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
            }
            focus: true
            Keys.onEscapePressed: root.closeFullScreen()
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Back || (event.key === Qt.Key_F && !event.modifiers)) {
                    if (!event.isAutoRepeat)
                        root.closeFullScreen()
                    event.accepted = true
                }
            }

            HoverHandler {
                id: videoHover
                enabled: !root.useHostWindow
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                property point lastPosition: Qt.point(-1, -1)
                onPointChanged: {
                    const position = point.scenePosition
                    if (!hovered || (lastPosition.x === position.x && lastPosition.y === position.y))
                        return
                    lastPosition = position
                    controlsTimeout.restart()
                }
            }
            Timer {
                id: controlsTimeout
                interval: 2500
            }

            // Anchor the shortcut to this scene's item. A Shortcut declared
            // directly under the nested Window can retain the source scene's
            // QObject parent on older Qt and resolve the wrong window.
            Shortcut {
                sequences: [StandardKey.Cancel]
                context: Qt.WindowShortcut
                enabled: fullScreenWindow.visible
                onActivated: root.closeFullScreen()
            }
            // Android Back can arrive as a shortcut instead of a key event.
            Shortcut {
                sequences: ["Back"]
                context: Qt.WindowShortcut
                enabled: fullScreenWindow.visible
                onActivated: root.closeFullScreen()
            }

            VideoOutput {
                id: fullScreenVideo
                objectName: "fullScreenVideoOutput"
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
            }

            Rectangle {
                anchors.centerIn: parent
                z: 1
                width: 80
                height: 80
                radius: 40
                color: "#90000000"
                visible: root.waitingForVideo
                BusyIndicator {
                    objectName: "fullScreenVideoBusyIndicator"
                    anchors.centerIn: parent
                    width: 64
                    height: 64
                    running: parent.visible && root.fullScreen
                    Accessible.name: root.playback && root.playback.seeking ? qsTr("Seeking video") : qsTr("Loading video")
                }
            }

            Item {
                objectName: "fullScreenVideoTapArea"
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: fullScreenTransport.visible ? fullScreenTransport.top : parent.bottom
                z: 1
                VideoTapHandler {
                    enabled: root.fullScreen && root.mediaBlock !== null
                             && root.playback && root.playback.available
                    gesturePolicy: TapHandler.DragThreshold
                    onClicked: root.playback.toggle(root.mediaBlock.block.url)
                    onDoubleClicked: root.closeFullScreen()
                }
            }

            Loader {
                id: fullScreenTransport
                objectName: "fullScreenTransportLoader"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: item ? item.implicitHeight : 0
                active: root.fullScreen && root.mediaBlock !== null
                visible: root.mediaBlock !== null
                         && (!root.mediaBlock.playing || root.waitingForVideo || controlsTimeout.running
                             || transportHover.hovered || (item && item.seeking))
                z: 2
                HoverHandler {
                    id: transportHover
                    enabled: videoHover.enabled
                    acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                }
                sourceComponent: MediaTransportControls {
                    mediaBlock: root.mediaBlock
                    embedded: true
                    fullScreenPresentation: true
                    color: {
                        const background = mediaBlock.editorView.documentCardColor
                        return Qt.rgba(background.r, background.g, background.b, 0.94)
                    }
                    onExitFullScreenRequested: root.closeFullScreen()
                }
            }

            // Standalone surface users without a transport still have a
            // visible, font-independent way to leave fullscreen.
            ToolButton {
                objectName: "fullScreenExitButton"
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 16
                width: 48
                height: 48
                visible: !root.mediaBlock
                z: 2
                contentItem: Shared.ThemedIconImpl {
                    themeName: "__bundled__"
                    fallbackName: "view-restore-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: "white"
                    pixelSize: 24
                }
                background: Rectangle { radius: 5; color: "#a0000000" }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Exit full screen")
                Accessible.name: qsTr("Exit full screen video")
                onClicked: root.closeFullScreen()
            }
        }
    }
}
