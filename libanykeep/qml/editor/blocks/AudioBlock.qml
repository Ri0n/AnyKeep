import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../shared" as Shared

FocusScope {
    id: audioRoot
    required property var editorView
    objectName: "audioBlockEditor-" + block.index
    required property var block
    property var playback: audioRoot.editorView.editorBackend ? audioRoot.editorView.editorBackend.audioPlayback : null
    property var transcription: audioRoot.editorView.audioTranscriptionController
    readonly property bool individuallySelected:
        audioRoot.editorView.selectedAudioIndex === block.index
    readonly property bool selected: individuallySelected
                                     || block.structurallySelected
    property bool transcriptExpanded: false
    property bool renaming: false
    readonly property bool current: playback && playback.currentSourceUri === block.url
    readonly property bool playing: current && playback.playing
    readonly property bool loading: current && playback.loading
    readonly property bool transcribing: transcription
                                         && transcription.transcribingAudioRow === block.index
    readonly property bool transcriptionBlocked: transcription && transcription.busy && !transcribing
    readonly property real knownDuration: current && playback.duration > 0
                                          ? playback.duration : block.audioDuration
    width: block.width
    implicitHeight: audioColumn.implicitHeight
    activeFocusOnTab: true

    function selectAndFocus() {
        audioRoot.editorView.selectAudioBlock(block.index)
        audioRoot.forceActiveFocus()
    }

    function requestTranscription() {
        selectAndFocus()
        if (!transcription || !transcription.audioTranscriptionAvailable
                || transcriptionBlocked || transcribing)
            return false
        return transcription.transcribeAudio(block.index, block.url, block.audioDuration)
    }

    function beginRename() {
        renameField.text = block.alt.length > 0 ? block.alt : qsTr("Audio recording")
        renaming = true
        Qt.callLater(function() {
            renameField.forceActiveFocus()
            renameField.selectAll()
        })
    }

    function finishRename() {
        if (!renaming)
            return
        renaming = false
        editorView.renameAudioBlock(block.index, renameField.text)
        selectAndFocus()
    }

    function cancelRename() {
        if (!renaming)
            return
        renaming = false
        selectAndFocus()
    }

    function formatTime(milliseconds) {
        const seconds = Math.max(0, Math.floor(Number(milliseconds) / 1000))
        const remainder = seconds % 60
        return Math.floor(seconds / 60) + ":" + (remainder < 10 ? "0" : "") + remainder
    }

    Keys.onPressed: function(event) {
        if (event.matches(StandardKey.Copy)) {
            event.accepted = audioRoot.editorView.copyActiveSelection()
            return
        }
        if (event.modifiers)
            return
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            if (playback)
                playback.toggle(block.url)
            event.accepted = true
        } else if (event.key === Qt.Key_Left && playback) {
            playback.seek(block.url, Math.max(0, (current ? playback.position : 0) - 5000))
            event.accepted = true
        } else if (event.key === Qt.Key_Right && playback) {
            playback.seek(block.url, Math.min(knownDuration, (current ? playback.position : 0) + 5000))
            event.accepted = true
        } else if (event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) {
            audioRoot.editorView.removeAudioBlock(block.index, true)
            event.accepted = true
        } else if (event.key === Qt.Key_Escape) {
            audioRoot.editorView.clearAudioSelection()
            audioRoot.editorView.forceActiveFocus()
            event.accepted = true
        }
    }

    ColumnLayout {
        id: audioColumn
        width: parent.width
        spacing: 0

        Rectangle {
            id: audioCard
            objectName: "audioCard-" + audioRoot.block.index
            Layout.fillWidth: true
            implicitHeight: audioRoot.editorView.touchMode ? 68 : 58
            radius: 6
            color: audioRoot.selected
                   ? Qt.rgba(playButton.palette.highlight.r,
                             playButton.palette.highlight.g,
                             playButton.palette.highlight.b, 0.18)
                   : audioRoot.editorView.documentCardColor
            border.width: audioRoot.selected ? 2 : 1
            border.color: audioRoot.selected
                          ? playButton.palette.highlight
                          : audioRoot.editorView.documentCardBorderColor

            RowLayout {
                anchors.fill: parent
                anchors.margins: audioRoot.editorView.touchMode ? 8 : 6
                spacing: 6

                ToolButton {
                    id: playButton
                    Layout.preferredWidth: audioRoot.editorView.touchMode ? 44 : 36
                    Layout.preferredHeight: Layout.preferredWidth
                    enabled: audioRoot.playback && audioRoot.playback.available
                    text: audioRoot.loading ? "…" : (audioRoot.playing ? "Ⅱ" : "▶")
                    font.pixelSize: audioRoot.editorView.touchMode ? 19 : 16
                    Accessible.name: audioRoot.playing ? qsTr("Pause audio") : qsTr("Play audio")
                    ToolTip.visible: hovered
                    ToolTip.text: audioRoot.current && audioRoot.playback.errorString.length > 0
                                  ? audioRoot.playback.errorString : Accessible.name
                    onClicked: {
                        audioRoot.selectAndFocus()
                        audioRoot.playback.toggle(audioRoot.block.url)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: audioRoot.editorView.touchMode ? 4 : 3

                    Label {
                        id: audioTitle
                        objectName: "audioTitle-" + audioRoot.block.index
                        Layout.fillWidth: true
                        visible: !audioRoot.renaming
                        text: audioRoot.block.alt.length > 0
                              ? audioRoot.block.alt : qsTr("Audio recording")
                        elide: Text.ElideMiddle
                        font.bold: audioRoot.selected
                        color: audioRoot.editorView.documentTextColor
                    }

                    TextField {
                        id: renameField
                        objectName: "audioTitleField-" + audioRoot.block.index
                        Layout.fillWidth: true
                        visible: audioRoot.renaming
                        selectByMouse: true
                        font: audioRoot.editorView.editorFont
                        placeholderText: qsTr("Audio recording title")
                        color: audioRoot.editorView.documentTextColor
                        placeholderTextColor: audioRoot.editorView.documentSecondaryTextColor
                        onAccepted: audioRoot.finishRename()
                        onActiveFocusChanged: {
                            if (!activeFocus && audioRoot.renaming)
                                audioRoot.finishRename()
                        }
                        Keys.onEscapePressed: function(event) {
                            audioRoot.cancelRename()
                            event.accepted = true
                        }
                    }

                    Slider {
                        id: audioPosition
                        Layout.fillWidth: true
                        from: 0
                        to: Math.max(1, audioRoot.knownDuration)
                        value: audioRoot.current ? audioRoot.playback.position : 0
                        enabled: audioRoot.playback && audioRoot.knownDuration > 0
                        onMoved: audioRoot.playback.seek(audioRoot.block.url, value)
                        Accessible.name: qsTr("Audio position")
                    }
                }

                Label {
                    Layout.minimumWidth: audioRoot.editorView.touchMode ? 48 : 42
                    horizontalAlignment: Text.AlignRight
                    text: audioRoot.formatTime(audioRoot.current ? audioRoot.playback.position : 0)
                          + " / " + audioRoot.formatTime(audioRoot.knownDuration)
                    font.pixelSize: Math.max(10, audioRoot.editorView.editorFontMetricsHeight * 0.65)
                    color: audioRoot.editorView.documentSecondaryTextColor
                }

                ToolButton {
                    id: transcribeButton
                    property bool consumedLongPress: false
                    Layout.preferredWidth: audioRoot.editorView.touchMode ? 44 : 36
                    Layout.preferredHeight: Layout.preferredWidth
                    visible: audioRoot.transcription
                             && audioRoot.transcription.audioTranscriptionAvailable
                    enabled: !audioRoot.transcriptionBlocked && !audioRoot.transcribing
                    text: audioRoot.transcribing ? "…" : "STT"
                    font.pixelSize: Math.max(9, audioRoot.editorView.editorFontMetricsHeight * 0.55)
                    Accessible.name: audioRoot.block.audioTranscript.length > 0
                                     ? qsTr("Show transcript; press and hold to transcribe again")
                                     : qsTr("Transcribe audio")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    onPressAndHold: {
                        consumedLongPress = true
                        longPressReset.restart()
                        audioRoot.requestTranscription()
                    }
                    Timer {
                        id: longPressReset
                        interval: 2000
                        onTriggered: transcribeButton.consumedLongPress = false
                    }
                    onClicked: {
                        if (consumedLongPress) {
                            consumedLongPress = false
                            return
                        }
                        audioRoot.selectAndFocus()
                        if (audioRoot.block.audioTranscript.length > 0)
                            audioRoot.transcriptExpanded = !audioRoot.transcriptExpanded
                        else
                            audioRoot.requestTranscription()
                    }
                }

                ToolButton {
                    id: audioActionsButton
                    Layout.preferredWidth: audioRoot.editorView.touchMode ? 40 : 30
                    Layout.preferredHeight: Layout.preferredWidth
                    focusPolicy: Qt.NoFocus
                    display: AbstractButton.IconOnly
                    Accessible.name: qsTr("Audio recording actions")
                    ToolTip.visible: hovered
                    ToolTip.text: Accessible.name
                    contentItem: Shared.ThemedIconImpl {
                        themeName: "__bundled__"
                        fallbackName: "overflow-menu-symbolic.svg"
                        recolorFallback: true
                        pixelSize: audioRoot.editorView.touchMode ? 20 : 17
                    }
                    onClicked: {
                        audioRoot.selectAndFocus()
                        audioMenu.popup()
                    }
                }
            }

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onTapped: audioRoot.selectAndFocus()
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    audioRoot.selectAndFocus()
                    audioMenu.popup()
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: audioRoot.transcriptExpanded
                     && audioRoot.block.audioTranscript.length > 0
            implicitHeight: visible ? transcriptText.implicitHeight + 16 : 0
            radius: 5
            color: audioRoot.editorView.documentCardColor
            border.width: 1
            border.color: audioRoot.editorView.documentCardBorderColor

            TextArea {
                id: transcriptText
                anchors.fill: parent
                anchors.margins: 4
                readOnly: true
                text: audioRoot.block.audioTranscript
                wrapMode: TextEdit.Wrap
                selectByMouse: !audioRoot.editorView.touchMode
                background: null
                color: audioRoot.editorView.documentTextColor
                Accessible.name: qsTr("Audio transcript")
            }
        }
    }

    Connections {
        target: audioRoot.block
        function onAudioTranscriptChanged() {
            if (audioRoot.block.audioTranscript.length > 0)
                audioRoot.transcriptExpanded = true
        }
    }

    Menu {
        id: audioMenu
        MenuItem {
            text: audioRoot.playing ? qsTr("Pause") : qsTr("Play")
            enabled: audioRoot.playback && audioRoot.playback.available
            onTriggered: audioRoot.playback.toggle(audioRoot.block.url)
        }
        MenuItem {
            text: audioRoot.block.audioTranscript.length > 0
                  ? qsTr("Show transcript") : qsTr("Transcribe audio")
            visible: audioRoot.transcription
                     && audioRoot.transcription.audioTranscriptionAvailable
            height: visible ? implicitHeight : 0
            enabled: !audioRoot.transcriptionBlocked && !audioRoot.transcribing
            onTriggered: {
                if (audioRoot.block.audioTranscript.length > 0)
                    audioRoot.transcriptExpanded = true
                else
                    audioRoot.requestTranscription()
            }
        }
        MenuItem {
            text: qsTr("Transcribe again")
            visible: audioRoot.block.audioTranscript.length > 0
                     && audioRoot.transcription
                     && audioRoot.transcription.audioTranscriptionAvailable
            height: visible ? implicitHeight : 0
            enabled: !audioRoot.transcriptionBlocked && !audioRoot.transcribing
            onTriggered: audioRoot.requestTranscription()
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Rename")
            onTriggered: audioRoot.beginRename()
        }
        MenuSeparator { }
        MenuItem {
            text: qsTr("Remove Audio Recording")
            onTriggered: audioRoot.editorView.removeAudioBlock(audioRoot.block.index, true)
        }
    }

}
