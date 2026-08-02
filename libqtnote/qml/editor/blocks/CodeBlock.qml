import QtQuick
import QtQuick.Controls
import "../components" as Editor
import "../../shared" as Shared

Rectangle {
    id: codeRoot
    required property var editorView
    required property var linkEditorPopup
    required property var block
    property alias blockIndex: codeCell.blockIndex
    width: block.width
    implicitHeight: Math.max(codeCell.implicitHeight + 12, codeRoot.editorView.editorFontMetricsHeight * 3)
    radius: 4
    clip: true
    color: Qt.rgba(codeCell.palette.base.r, codeCell.palette.base.g,
                   codeCell.palette.base.b, 0.82)
    border.width: 1
    border.color: codeCell.palette.midlight

    function forceActiveFocus() { codeCell.forceActiveFocus() }

    Editor.NoteBlockTextArea {
        editorView: codeRoot.editorView
        linkPopup: codeRoot.linkEditorPopup
        id: codeCell
        property var block: codeRoot.block
        anchors.fill: parent
        anchors.margins: 6
        blockIndex: block.index
        editorField: "code"
        codeDocument: true
        syntaxLanguage: block.codeLanguage
        sourceText: block.blockText
        textFormat: TextEdit.PlainText
        wrapMode: TextEdit.NoWrap
        font.family: Qt.platform.os === "windows" ? "Consolas" : "monospace"
        commitText: function() { codeRoot.editorView.blockModel.setBlockText(block.index, text) }
        onTextChanged: commitChangedText(activeFocus)
    }

    Item {
        id: codeOverlay
        anchors.top: parent.top
        anchors.right: parent.right
        width: languageSelector.width + codeActions.width
        height: languageSelector.height
        z: 2

        Editor.HoverActionStrip {
            id: codeActions
            anchors.right: languageSelector.left
            anchors.verticalCenter: languageSelector.verticalCenter
            triggerHovered: languageSelector.hovered
            fadeDuration: 480
            gapAfter: 6

            ToolButton {
                id: copyCodeButton
                width: languageSelector.height
                height: languageSelector.height
                padding: 3
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    themeName: "edit-copy"
                    fallbackName: "copy22.png"
                    pixelSize: Math.max(14, Math.round(copyCodeButton.height * 0.58))
                }
                Accessible.name: qsTr("Copy code")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: {
                    if (codeRoot.editorView.editorBackend)
                        codeRoot.editorView.editorBackend.copyToClipboard(codeCell.text)
                }

                background: Rectangle {
                    radius: codeRoot.radius
                    color: copyCodeButton.pressed ? copyCodeButton.palette.mid
                                                  : copyCodeButton.palette.alternateBase
                    border.width: 1
                    border.color: codeCell.palette.midlight
                }
            }
        }

        Editor.CompactPopupSelector {
            id: languageSelector
            anchors.top: parent.top
            anchors.right: parent.right
            model: codeRoot.editorView.platformBackend ? codeRoot.editorView.platformBackend.codeLanguages : []
            textRole: "name"
            valueRole: "id"
            implicitHeight: Math.max(24, codeRoot.editorView.editorFontMetricsHeight + 8)
            minimumControlWidth: 48
            minimumPopupWidth: 180
            backgroundColor: palette.alternateBase
            hoverColor: palette.alternateBase
            borderColor: codeCell.palette.midlight

            function synchronizeLanguage() {
                const language = codeRoot.editorView.platformBackend
                        ? codeRoot.editorView.platformBackend.canonicalCodeLanguage(codeRoot.block.codeLanguage)
                        : codeRoot.block.codeLanguage
                currentIndex = Math.max(0, indexOfValue(language))
            }

            Component.onCompleted: synchronizeLanguage()
            onModelChanged: synchronizeLanguage()
            onActivated: function(index, value) {
                codeRoot.editorView.blockModel.setCodeLanguage(codeRoot.block.index, value || "")
            }

            Connections {
                target: codeRoot.block
                ignoreUnknownSignals: true
                function onCodeLanguageChanged() { languageSelector.synchronizeLanguage() }
            }
        }
    }
}
