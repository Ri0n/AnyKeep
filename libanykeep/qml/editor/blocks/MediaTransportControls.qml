import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../../shared" as Shared
import ".." as EditorComponents

// Transport controls are shared by the audio-only card and the video card.
// Neither the slider nor the buttons own media playback state or editor history.
Rectangle {
    id: transport
    required property var mediaRoot
    property bool embedded: false
    objectName: "mediaCard-" + mediaRoot.block.index
    color: mediaRoot.editorView.documentCardColor
    radius: embedded ? 0 : 5

        RowLayout {
            anchors.fill: parent
            anchors.margins: 4
            spacing: 6
            ToolButton {
                id: mediaPlaybackButton
                objectName: "mediaPlaybackButton-" + mediaRoot.block.index
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                enabled: mediaRoot.playback && mediaRoot.playback.available
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Item {
                    implicitWidth: 20
                    implicitHeight: 20
                    Shared.ThemedIconImpl {
                        objectName: "mediaPlaybackGlyph-" + mediaRoot.block.index
                        anchors.centerIn: parent
                        visible: !mediaRoot.loading
                        themeName: "__bundled__"
                        fallbackName: mediaRoot.playing ? "media-playback-pause-symbolic.svg"
                                                        : "media-playback-start-symbolic.svg"
                        recolorFallback: true
                        fallbackTintMode: String(mediaPlaybackButton.palette.buttonText)
                        pixelSize: 20
                    }
                    BusyIndicator {
                        objectName: "mediaPlaybackBusyIndicator-" + mediaRoot.block.index
                        anchors.centerIn: parent
                        width: 22
                        height: 22
                        visible: mediaRoot.loading
                        running: visible
                    }
                }
                Accessible.name: mediaRoot.loading ? qsTr("Loading media")
                                                  : (mediaRoot.playing ? qsTr("Pause media") : qsTr("Play media"))
                onClicked: {
                    mediaRoot.selectAndFocus()
                    mediaRoot.playback.toggle(mediaRoot.block.url)
                }
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    objectName: "mediaLabel-" + mediaRoot.block.index
                    Layout.fillWidth: true
                    visible: !mediaRoot.visual
                    text: mediaRoot.block.alt.length > 0 ? mediaRoot.block.alt : qsTr("Audio recording")
                    elide: Text.ElideMiddle
                    font.bold: mediaRoot.selected
                    color: mediaRoot.editorView.documentTextColor
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0
                    to: Math.max(1, mediaRoot.knownDuration)
                    value: mediaRoot.current ? mediaRoot.playback.position : 0
                    enabled: mediaRoot.playback && mediaRoot.knownDuration > 0
                    onMoved: mediaRoot.playback.seek(mediaRoot.block.url, value)
                }
            }
            Label {
                text: mediaRoot.formatTime(mediaRoot.current ? mediaRoot.playback.position : 0)
                      + " / " + mediaRoot.formatTime(mediaRoot.knownDuration)
                color: mediaRoot.editorView.documentSecondaryTextColor
            }
            EditorComponents.MediaSyncIndicator {
                objectName: "mediaAudioSync-" + mediaRoot.block.index
                visible: mediaRoot.audio && Boolean(mediaRoot.syncStatus.valid)
                Layout.preferredWidth: visible ? parent.height - 2 : 0
                Layout.preferredHeight: Layout.preferredWidth
                progress: mediaRoot.syncStatus.progress || 0
                syncState: mediaRoot.syncStatus.state || 0
                errorString: mediaRoot.syncStatus.error || ""
            }
            ToolButton {
                visible: mediaRoot.canTranscribe
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                enabled: !mediaRoot.transcriptionBlocked && !mediaRoot.transcribing
                text: mediaRoot.transcribing ? "…" : "STT"
                Accessible.name: mediaRoot.block.mediaTranscript.length > 0
                                 ? qsTr("Show transcript") : qsTr("Transcribe audio")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    if (mediaRoot.block.mediaTranscript.length > 0)
                        mediaRoot.transcriptExpanded = !mediaRoot.transcriptExpanded
                    else
                        mediaRoot.requestTranscription()
                }
            }
            ToolButton {
                id: fullScreenMediaButton
                objectName: "mediaFullscreenButton-" + mediaRoot.block.index
                visible: mediaRoot.video
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                enabled: mediaRoot.playback && mediaRoot.playback.available
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "mediaFullscreenGlyph-" + mediaRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "view-fullscreen-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(fullScreenMediaButton.palette.buttonText)
                    pixelSize: 20
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Full screen")
                Accessible.name: qsTr("Show video full screen")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    mediaRoot.openVideoFullScreen()
                }
            }
            ToolButton {
                id: mediaActionsButton
                objectName: "mediaActionsButton-" + mediaRoot.block.index
                Layout.preferredWidth: parent.height - 2
                Layout.preferredHeight: Layout.preferredWidth
                padding: 6
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    objectName: "mediaActionsGlyph-" + mediaRoot.block.index
                    themeName: "__bundled__"
                    fallbackName: "view-more-vertical-symbolic.svg"
                    recolorFallback: true
                    fallbackTintMode: String(mediaActionsButton.palette.buttonText)
                    pixelSize: 20
                }
                Accessible.name: qsTr("Media actions")
                onClicked: {
                    mediaRoot.selectAndFocus()
                    mediaRoot.openMediaContextMenu()
                }
            }
        }
}
