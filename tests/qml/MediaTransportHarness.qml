import QtQuick
import QtQuick.Controls
import "qrc:/qml/editor/blocks" as Media

Item {
    id: harness
    objectName: "mediaHarness"
    width: 390
    height: 200
    property bool audioMode: true
    property bool detachedAvailable: true
    property alias touchMode: mockView.touchMode
    property alias canTranscribe: mockMedia.canTranscribe
    property alias syncStatus: mockMedia.syncStatus
    property alias knownDuration: mockMedia.knownDuration

    QtObject {
        id: mockBlock
        property int index: 23
        property string url: "anykeep-media:/missing/long_audio_filename_that_overflows_the_title_field.ogg"
        property string alt: ""
        property string mediaTranscript: ""
    }
    QtObject {
        id: mockPlayback
        property bool available: true
        property real position: 0
        property real duration: 21000
        property var openWindow: harness.detachedAvailable ? function(source) {} : null
        function toggle(source) {}
        function seek(source, offset) {}
    }
    QtObject {
        id: mockBackend
        function mediaFileName(source) { return "long_audio_filename_that_overflows_the_title_field.ogg" }
    }
    QtObject {
        id: mockView
        property bool touchMode: true
        property var editorBackend: mockBackend
        property color documentCardColor: "#252525"
        property color documentTextColor: "white"
        property color documentSecondaryTextColor: "#aaaaaa"
        property bool mediaTitleEditorFocused: false
        function beginEditTransaction(name) {}
        function endEditTransaction() {}
    }
    QtObject {
        id: mockMedia
        property var editorView: mockView
        property var block: mockBlock
        property var playback: mockPlayback
        property bool audio: harness.audioMode
        property bool video: !harness.audioMode
        property bool playing: false
        property bool current: true
        property bool loading: false
        property bool selected: false
        property real knownDuration: 21000
        property bool canEditTitle: true
        property bool titleEditing: false
        property bool canTranscribe: true
        property bool transcriptionBlocked: false
        property bool transcribing: false
        property bool transcriptExpanded: false
        property var syncStatus: ({valid: true, progress: 1, state: 2, error: ""})
        function formatTime(milliseconds) {
            const seconds = Math.floor(milliseconds / 1000)
            return Math.floor(seconds / 60) + ":" + (seconds % 60 < 10 ? "0" : "") + seconds % 60
        }
        function selectAndFocus() {}
        function handleNavigationKey(event) { return false }
        function setTitle(value) { mockBlock.alt = value }
        function requestTranscription() {}
        function openMediaContextMenu() {}
        function openVideoFullScreen() {}
        function openVideoWindow() {}
    }
    Media.MediaTransportControls {
        objectName: "mediaLayoutUnderTest"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        mediaBlock: mockMedia
        height: implicitHeight
    }
}
