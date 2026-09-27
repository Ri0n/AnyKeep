import QtQuick
import QtQuick.Controls
import QtQuick.Window
import QtMultimedia

Item {
    id: root
    property var playback: null
    property var attachedPlayback: null
    readonly property bool fullScreen: fullScreenWindow.visible

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
        fullScreenWindow.showFullScreen()
        fullScreenContent.forceActiveFocus()
    }

    function closeFullScreen() {
        if (!fullScreen)
            return
        fullScreenWindow.hide()
        if (attachedPlayback)
            attachedPlayback.attachVideoOutput(inlineVideo)
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

    Window {
        id: fullScreenWindow
        visible: false
        color: "black"
        flags: Qt.Window | Qt.FramelessWindowHint
        title: qsTr("Video")

        onClosing: function(close) {
            close.accepted = false
            root.closeFullScreen()
        }

        Item {
            id: fullScreenContent
            anchors.fill: parent
            focus: true
            Keys.onEscapePressed: root.closeFullScreen()

            VideoOutput {
                id: fullScreenVideo
                anchors.fill: parent
                fillMode: VideoOutput.PreserveAspectFit
            }

            ToolButton {
                anchors.top: parent.top
                anchors.right: parent.right
                anchors.margins: 16
                text: "×"
                z: 2
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Exit full screen")
                Accessible.name: qsTr("Exit full screen video")
                onClicked: root.closeFullScreen()
            }
        }
    }
}
