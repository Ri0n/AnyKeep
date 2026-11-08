import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "../../shared" as Shared

Window {
    id: playerWindow
    required property var playback
    required property string sourceUri
    required property string mediaTitle
    required property url previewUrl
    objectName: "detachedMediaWindow"
    title: mediaTitle.length > 0 ? mediaTitle : qsTr("Video")
    width: 800
    height: 500
    minimumWidth: 400
    minimumHeight: 260
    color: colors.window
    SystemPalette { id: colors }

    Shortcut {
        sequences: [StandardKey.Cancel]
        context: Qt.WindowShortcut
        enabled: !surface.fullScreen
        onActivated: playerWindow.close()
    }
    QtObject {
        id: adapter
        readonly property var playback: playerWindow.playback
        readonly property bool video: true
        readonly property bool audio: false
        readonly property bool visual: true
        readonly property bool current: true
        readonly property bool playing: playback.playing
        readonly property bool loading: playback.loading
        readonly property real knownDuration: playback.duration
        readonly property bool selected: false
        readonly property bool canTranscribe: false
        readonly property bool canEditTitle: false
        readonly property bool transcribing: false
        readonly property bool transcriptionBlocked: false
        readonly property var syncStatus: ({ valid: false, progress: 0, state: 0 })
        readonly property var block: QtObject {
            readonly property int index: -1
            readonly property string url: playerWindow.sourceUri
            readonly property string alt: playerWindow.mediaTitle
            readonly property string mediaTranscript: ""
        }
        readonly property var editorView: QtObject {
            readonly property color documentCardColor: colors.alternateBase
            readonly property color documentTextColor: colors.text
            readonly property color documentSecondaryTextColor: colors.placeholderText
        }
        function selectAndFocus() { content.forceActiveFocus() }
        function handleNavigationKey(event) { return false }
        function formatTime(ms) {
            const seconds = Math.max(0, Math.floor(ms / 1000))
            return Math.floor(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + seconds % 60
        }
        function openVideoFullScreen() { surface.openFullScreen() }
        function openMediaContextMenu() { windowMenu.popup() }
    }
    FocusScope {
        id: content
        anchors.fill: parent
        focus: true
        Keys.onSpacePressed: playback.toggle(sourceUri)
        Keys.onLeftPressed: playback.seek(sourceUri, Math.max(0, playback.position - 5000))
        Keys.onRightPressed: playback.seek(sourceUri, playback.position + 5000)
        Item {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: transport.top
            Image {
                id: poster
                anchors.fill: parent
                source: String(playerWindow.previewUrl).startsWith("image://anykeep-media/")
                        || String(playerWindow.previewUrl).startsWith("data:image/jpeg;base64,")
                        ? playerWindow.previewUrl : ""
                fillMode: Image.PreserveAspectFit
                visible: !playback.hasVideoFrame
            }
            Shared.ThemedIconImpl {
                anchors.centerIn: parent
                visible: !playback.hasVideoFrame && poster.status !== Image.Ready
                themeName: "__bundled__"
                fallbackName: "video-x-generic-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: String(colors.placeholderText)
                pixelSize: 64
            }
            MediaVideoSurface {
                id: surface
                anchors.fill: parent
                playback: playerWindow.playback
            }
            Label {
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.margins: 8
                padding: 6
                text: playerWindow.mediaTitle
                visible: !playback.playing && text.length > 0
                color: "white"
                background: Rectangle { radius: 4; color: "#a0000000" }
            }
            TapHandler { onTapped: content.forceActiveFocus() }
            Label {
                anchors.centerIn: parent
                width: Math.min(implicitWidth, Math.max(0, parent.width - 32))
                padding: 8
                text: playback.errorString
                visible: text.length > 0
                wrapMode: Text.Wrap
                color: colors.text
                background: Rectangle { color: colors.base; radius: 4 }
            }
        }
        MediaTransportControls {
            id: transport
            mediaBlock: adapter
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 48
        }
    }
    Menu {
        id: windowMenu
        MenuItem {
            text: playback.playing ? qsTr("Pause") : qsTr("Play")
            onTriggered: playback.toggle(sourceUri)
        }
        MenuItem {
            text: qsTr("Close player")
            onTriggered: playerWindow.close()
        }
    }
}
