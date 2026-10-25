import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window
import QtMultimedia
import "../../shared" as Shared

Item {
    id: root
    objectName: "mediaVideoSurface"
    property var playback: null
    property var platformBackend: null
    property var attachedPlayback: null
    readonly property bool fullScreen: fullScreenWindow.visible
    readonly property bool waitingForVideo: playback && (playback.loading || playback.seeking)
    readonly property bool canControlPlayback: playback && playback.available
                                                && playback.currentSourceUri.length > 0

    function togglePlayback() {
        if (canControlPlayback)
            playback.toggle(playback.currentSourceUri)
    }

    function seekTo(position) {
        if (canControlPlayback && playback.duration > 0)
            playback.seek(playback.currentSourceUri,
                          Math.round(Math.max(0, Math.min(playback.duration, position))))
    }

    function seekBy(offset) {
        seekTo(playback ? playback.position + offset : 0)
    }

    function formatTime(milliseconds) {
        const seconds = Math.max(0, Math.floor(Number(milliseconds || 0) / 1000))
        const remainder = seconds % 60
        return Math.floor(seconds / 60) + ":" + (remainder < 10 ? "0" : "") + remainder
    }

    component FullScreenButton: ToolButton {
        id: control
        required property string glyphName
        Layout.preferredWidth: 44
        Layout.preferredHeight: 44
        padding: 10
        display: AbstractButton.IconOnly
        contentItem: Shared.ThemedIconImpl {
            themeName: "__bundled__"
            fallbackName: control.glyphName
            recolorFallback: true
            fallbackTintMode: "white"
            pixelSize: 24
        }
        background: Rectangle {
            radius: 4
            color: control.down ? "#707070" : (control.hovered || control.visualFocus ? "#505050" : "transparent")
        }
        ToolTip.visible: hovered
        ToolTip.text: Accessible.name
    }

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
        if (platformBackend) {
            platformBackend.prepareFullScreenWindow(fullScreenWindow, root)
        } else {
            const sourceWindow = root.Window.window
            if (sourceWindow && sourceWindow.screen) {
                fullScreenWindow.transientParent = sourceWindow
                fullScreenWindow.screen = sourceWindow.screen
                fullScreenWindow.x = sourceWindow.screen.virtualX
                fullScreenWindow.y = sourceWindow.screen.virtualY
                fullScreenWindow.width = sourceWindow.screen.width
                fullScreenWindow.height = sourceWindow.screen.height
            }
        }
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
        objectName: "inlineVideoOutput"
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
        objectName: "fullScreenVideoWindow"
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
            objectName: "fullScreenVideoContent"
            anchors.fill: parent
            focus: true
            Keys.onEscapePressed: root.closeFullScreen()
            Keys.onPressed: function(event) {
                fullScreenControlsVisibility.reveal()
                if (event.modifiers)
                    return
                if (event.key === Qt.Key_Space) {
                    root.togglePlayback()
                } else if (event.key === Qt.Key_Left) {
                    root.seekBy(-5000)
                } else if (event.key === Qt.Key_Right) {
                    root.seekBy(5000)
                } else if (event.key === Qt.Key_F11) {
                    root.closeFullScreen()
                } else {
                    return
                }
                event.accepted = true
            }

            VideoOutput {
                id: fullScreenVideo
                objectName: "fullScreenVideoOutput"
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

            Rectangle {
                objectName: "fullScreenVideoControls"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 12
                height: controls.implicitHeight + 16
                radius: 8
                color: "#d0202020"
                z: 2
                opacity: fullScreenControlsVisibility.controlsVisible ? 1 : 0
                enabled: fullScreenControlsVisibility.controlsVisible || opacity > 0
                onEnabledChanged: {
                    if (!enabled && root.fullScreen)
                        fullScreenContent.forceActiveFocus()
                }
                Behavior on opacity {
                    NumberAnimation { duration: 180; easing.type: Easing.InOutQuad }
                }

                RowLayout {
                    id: controls
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 12

                    FullScreenButton {
                        id: fullScreenPlayPause
                        objectName: "fullScreenVideoPlayPause"
                        enabled: root.canControlPlayback
                        glyphName: root.playback && root.playback.playing
                                   ? "media-playback-pause-symbolic.svg" : "media-playback-start-symbolic.svg"
                        Accessible.name: root.playback && root.playback.playing ? qsTr("Pause media") : qsTr("Play media")
                        onClicked: root.togglePlayback()
                    }

                    Slider {
                        id: fullScreenSeek
                        objectName: "fullScreenVideoSeek"
                        Layout.fillWidth: true
                        Layout.preferredHeight: 44
                        from: 0
                        to: root.playback ? Math.max(1, root.playback.duration || 0) : 1
                        value: root.playback ? root.playback.position || 0 : 0
                        enabled: root.canControlPlayback && root.playback.duration > 0
                        palette.highlight: "white"
                        palette.mid: "#909090"
                        Accessible.name: qsTr("Playback position")
                        onMoved: root.seekTo(value)
                        Keys.onLeftPressed: root.seekBy(-5000)
                        Keys.onRightPressed: root.seekBy(5000)
                    }

                    Label {
                        objectName: "fullScreenVideoTime"
                        text: root.formatTime(root.playback ? root.playback.position : 0)
                              + " / " + root.formatTime(root.playback ? root.playback.duration : 0)
                        color: "white"
                    }

                    FullScreenButton {
                        id: fullScreenExit
                        objectName: "fullScreenVideoExit"
                        glyphName: "view-restore-symbolic.svg"
                        Accessible.name: qsTr("Exit full screen video")
                        onClicked: root.closeFullScreen()
                    }
                }
            }

            VideoControlsVisibility {
                id: fullScreenControlsVisibility
                objectName: "fullScreenVideoControlsVisibility"
                anchors.fill: parent
                z: 3
                enabled: root.fullScreen
                playing: Boolean(root.playback && root.playback.playing) && !root.waitingForVideo
                interacting: fullScreenSeek.pressed || fullScreenSeek.visualFocus
                             || fullScreenPlayPause.down || fullScreenPlayPause.visualFocus
                             || fullScreenExit.down || fullScreenExit.visualFocus
            }
        }
    }
}
