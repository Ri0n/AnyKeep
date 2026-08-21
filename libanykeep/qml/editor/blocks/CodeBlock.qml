import QtQuick
import QtQuick.Controls
import "../components" as Editor
import "../../shared" as Shared

Rectangle {
    id: codeRoot
    objectName: "codeBlock"
    required property var editorView
    required property var linkEditorPopup
    required property var block
    property alias blockIndex: codeCell.blockIndex
    property bool lineWrapEnabled: false
    width: block.width
    implicitHeight: Math.max(codeCell.implicitHeight + 12, codeRoot.editorView.editorFontMetricsHeight * 2)
    radius: 4
    clip: true
    readonly property real baseLuminance: 0.2126 * activePalette.base.r
                                          + 0.7152 * activePalette.base.g
                                          + 0.0722 * activePalette.base.b
    readonly property real textLuminance: 0.2126 * activePalette.text.r
                                          + 0.7152 * activePalette.text.g
                                          + 0.0722 * activePalette.text.b
    readonly property real surfaceMix: baseLuminance < textLuminance ? 0.07 : 0.045
    color: Qt.rgba(
        activePalette.base.r * (1.0 - surfaceMix) + activePalette.text.r * surfaceMix,
        activePalette.base.g * (1.0 - surfaceMix) + activePalette.text.g * surfaceMix,
        activePalette.base.b * (1.0 - surfaceMix) + activePalette.text.b * surfaceMix,
        1.0)
    border.width: 1
    readonly property color codeTextColor: activePalette.text
    readonly property color codeBorderColor: Qt.rgba(codeTextColor.r, codeTextColor.g,
                                                     codeTextColor.b, 0.28)
    // Keep the overlay controls on the document surface rather than the code
    // surface, so their boundary remains clear even on subtle themes.
    readonly property color controlSurface: activePalette.base
    readonly property color controlHoverSurface: Qt.rgba(
        activePalette.base.r * 0.86 + activePalette.text.r * 0.14,
        activePalette.base.g * 0.86 + activePalette.text.g * 0.14,
        activePalette.base.b * 0.86 + activePalette.text.b * 0.14,
        1.0)
    border.color: codeBorderColor

    // Do not inherit the Windows inactive control palette for document UI.
    SystemPalette {
        id: activePalette
        colorGroup: SystemPalette.Active
    }

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
        wrapMode: codeRoot.lineWrapEnabled ? TextEdit.Wrap : TextEdit.NoWrap
        font.family: Qt.platform.os === "windows" ? "Consolas" : "monospace"
        font.pointSize: codeRoot.editorView.editorPointSize
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
                id: wrapCodeButton
                objectName: "wrapCodeButton"
                width: languageSelector.height
                height: languageSelector.height
                padding: 3
                checkable: true
                checked: codeRoot.lineWrapEnabled
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    // Do not let a desktop theme substitute a low-resolution
                    // bitmap for the bundled symbolic SVG.
                    themeName: "__bundled__"
                    fallbackName: "text-wrap-symbolic.svg"
                    recolorFallback: true
                    pixelSize: Math.max(14, Math.round(wrapCodeButton.height * 0.58))
                }
                Accessible.name: checked ? qsTr("Disable code line wrapping")
                                         : qsTr("Enable code line wrapping")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onToggled: codeRoot.lineWrapEnabled = checked

                background: Rectangle {
                    radius: codeRoot.radius
                    color: wrapCodeButton.checked || wrapCodeButton.pressed
                           ? codeRoot.controlHoverSurface : codeRoot.controlSurface
                    border.width: 1
                    border.color: codeRoot.codeBorderColor
                }
            }

            ToolButton {
                id: copyCodeButton
                width: languageSelector.height
                height: languageSelector.height
                padding: 3
                display: AbstractButton.IconOnly
                contentItem: Shared.ThemedIconImpl {
                    themeName: "__bundled__"
                    fallbackName: "edit-copy-symbolic.svg"
                    recolorFallback: true
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
                    color: copyCodeButton.pressed ? codeRoot.controlHoverSurface
                                                  : codeRoot.controlSurface
                    border.width: 1
                    border.color: codeRoot.codeBorderColor
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
            backgroundColor: codeRoot.controlSurface
            popupBackgroundColor: codeRoot.controlSurface
            hoverColor: codeRoot.controlHoverSurface
            borderColor: codeRoot.codeBorderColor

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
