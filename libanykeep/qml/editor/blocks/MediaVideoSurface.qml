import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia
import "../../shared" as Shared

Item {
    id: root
    property var playback: null
    property var mediaBlock: null
    property bool controlsVisible: true
    property var attachedPlayback: null
    readonly property bool fullScreen: fullScreenWindow.visible
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
        if (sourceWindow && sourceWindow.screen)
            fullScreenWindow.screen = sourceWindow.screen
        attachedPlayback.attachVideoOutput(fullScreenVideo)
        controlsVisible = true
        fullScreenWindow.showFullScreen()
        fullScreenWindow.requestActivate()
        fullScreenContent.forceActiveFocus()
    }

    function closeFullScreen() {
        if (!fullScreen)
            return
        fullScreenWindow.hide()
        controlsVisible = true
        if (attachedPlayback)
            attachedPlayback.attachVideoOutput(inlineVideo)
        const sourceWindow = root.Window.window
        if (sourceWindow)
            sourceWindow.requestActivate()
        root.forceActiveFocus()
    }

    // Autohide is a playback affordance, not a delayed layout correction.
    // Keep the controls on screen when paused or when the user opens fullscreen.
    Timer {
        interval: 4000
        running: root.fullScreen && root.controlsVisible && root.mediaBlock
                 && root.mediaBlock.playing
                 && (!fullScreenTransport.item || !fullScreenTransport.item.seeking)
        onTriggered: root.controlsVisible = false
    }

    onPlaybackChanged: attach()
    Component.onCompleted: attach()
    Component.onDestruction: {
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

        Item {
            id: fullScreenContent
            anchors.fill: parent
            focus: true
            Keys.onEscapePressed: root.closeFullScreen()
            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Back) {
                    root.closeFullScreen()
                    event.accepted = true
                }
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
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
            }

            Rectangle {
                anchors.centerIn: parent
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

            // Capture taps above VideoOutput but below the transport, so taps
            // on playback buttons and the seek slider do not hide controls.
            Item {
                objectName: "fullScreenVideoTapArea"
                anchors.fill: parent
                z: 1
                TapHandler {
                    acceptedButtons: Qt.LeftButton
                    gesturePolicy: TapHandler.DragThreshold
                    onTapped: root.controlsVisible = !root.controlsVisible
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
                visible: root.controlsVisible
                z: 2
                sourceComponent: MediaTransportControls {
                    mediaBlock: root.mediaBlock
                    embedded: true
                    fullScreenPresentation: true
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
                visible: root.controlsVisible && !root.mediaBlock
                z: 2
                contentItem: Shared.ThemedIconImpl {
                    themeName: "__bundled__"
                    fallbackName: "window-close-symbolic.svg"
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
