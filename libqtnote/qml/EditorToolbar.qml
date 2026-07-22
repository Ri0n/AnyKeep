import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    required property var editorBackend
    required property var blockEditor
    property var platformBackend: null
    property bool compact: width < 720
    property bool showBackButton: false
    property bool showDeleteButton: false
    property bool showMobileActions: false
    property bool microphoneVisible: false
    property bool shortcutVisible: false
    signal backRequested()
    signal deleteRequested()
    signal findRequested()
    signal shareRequested()
    signal exportRequested()
    signal microphoneRequested()
    signal addToHomeScreenRequested()

    function runMarkdownCommand(kind, command) {
        if (!root.editorBackend || !root.blockEditor)
            return false
        root.blockEditor.flushPendingEditorChanges()
        const beforeView = root.blockEditor.captureEditorState()
        root.editorBackend.beginHistoryTransaction(kind, beforeView)
        try {
            if (!root.editorBackend.markdown)
                root.editorBackend.markdown = true
            return command()
        } finally {
            root.editorBackend.endHistoryTransaction(root.blockEditor.captureEditorState())
        }
    }

    function insertList(type) {
        const wasMarkdown = root.editorBackend && root.editorBackend.markdown
        const row = root.blockEditor ? root.blockEditor.insertionBlockIndex() : 0
        return runMarkdownCommand("insert-or-convert-list", function() {
            if (wasMarkdown)
                return root.blockEditor.insertListBlock(type)
            root.blockEditor.blockModel.insertList(row, type)
            root.blockEditor.focusBlock(row)
            return true
        })
    }

    function insertTable() {
        const row = root.blockEditor ? root.blockEditor.insertionBlockIndex() : 0
        return runMarkdownCommand("insert-table", function() {
            root.blockEditor.blockModel.insertTable(row)
            root.blockEditor.focusBlock(row)
            return true
        })
    }

    function insertImage() {
        if (!root.platformBackend || !root.editorBackend || !root.blockEditor)
            return false
        root.blockEditor.flushPendingEditorChanges()
        if (!root.editorBackend.markdown)
            root.editorBackend.markdown = true
        Qt.callLater(function() {
            root.platformBackend.insertImage(root.blockEditor.insertionBlockIndex())
        })
        return true
    }

    function copyDocument() {
        if (!root.editorBackend || !root.blockEditor)
            return false
        root.blockEditor.flushPendingEditorChanges()
        root.editorBackend.copyDocumentToClipboard()
        return true
    }

    function setMarkdownMode(markdown) {
        if (!root.editorBackend || !root.blockEditor
                || root.editorBackend.markdown === markdown)
            return
        root.blockEditor.flushPendingEditorChanges()
        root.editorBackend.markdown = markdown
    }

    function activeHeadingLevel() {
        if (!root.blockEditor || !root.blockEditor.activeEditor)
            return 0
        const editor = root.blockEditor.activeEditor
        if (!editor.block || editor.block.headingLevel === undefined)
            return 0
        const level = Number(editor.block.headingLevel)
        return level >= 1 && level <= 6 ? level : 0
    }

    function activeBlockStyleLabel() {
        const level = activeHeadingLevel()
        return level > 0 ? qsTr("H%1").arg(level) : qsTr("P")
    }

    readonly property int controlSize: 34
    readonly property int iconSize: 20
    implicitHeight: controlSize + 8

    function themedIconSource(themeName, fallbackName) {
        return "image://qtnoteicons/" + themeName + "/" + fallbackName
    }

    // QQuickImageProvider URLs must be loaded by an Image item. Passing an
    // image:// URL through AbstractButton.icon.source is resolved as a local
    // file by the Qt Quick Controls icon path in the supported Qt builds.
    component ThemedIconContent: Item {
        id: iconContent
        required property string themeName
        required property string fallbackName
        property int pixelSize: root.iconSize
        implicitWidth: pixelSize
        implicitHeight: pixelSize

        Image {
            anchors.centerIn: parent
            width: iconContent.pixelSize
            height: iconContent.pixelSize
            source: root.themedIconSource(iconContent.themeName, iconContent.fallbackName)
            sourceSize.width: iconContent.pixelSize
            sourceSize.height: iconContent.pixelSize
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: iconContent.enabled ? 1.0 : 0.38
        }
    }

    RowLayout {
        id: contentRow
        anchors.fill: parent
        anchors.margins: 4
        spacing: 1

        ToolButton {
            visible: root.showBackButton
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("‹")
            font.pixelSize: 26
            padding: 0
            Accessible.name: qsTr("Back")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.backRequested()
        }

        ToolButton {
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "edit-undo-symbolic"
                fallbackName: "edit-undo-symbolic.svg"
            }
            enabled: root.editorBackend && root.editorBackend.canUndo
            Accessible.name: root.editorBackend && root.editorBackend.undoText.length > 0
                             ? qsTr("Undo %1").arg(root.editorBackend.undoText) : qsTr("Undo")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.editorBackend.undo()
        }
        ToolButton {
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "edit-redo-symbolic"
                fallbackName: "edit-redo-symbolic.svg"
            }
            enabled: root.editorBackend && root.editorBackend.canRedo
            Accessible.name: root.editorBackend && root.editorBackend.redoText.length > 0
                             ? qsTr("Redo %1").arg(root.editorBackend.redoText) : qsTr("Redo")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.editorBackend.redo()
        }

        ToolSeparator {
            Layout.preferredHeight: root.controlSize - 8
            Layout.alignment: Qt.AlignVCenter
        }

        ToolButton {
            id: modeButton
            Layout.preferredWidth: 42
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            rightPadding: 10
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "__bundled__"
                fallbackName: root.editorBackend && root.editorBackend.markdown
                              ? "markdown.svg" : "txt.svg"
                pixelSize: 22
            }
            Accessible.name: root.editorBackend && root.editorBackend.markdown
                             ? qsTr("Document mode: Markdown")
                             : qsTr("Document mode: Plain text")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: modeMenu.open()

            Label {
                anchors.right: parent.right
                anchors.rightMargin: 3
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 2
                text: qsTr("▾")
                font.pixelSize: 9
                color: modeButton.palette.buttonText
                opacity: modeButton.enabled ? 0.8 : 0.4
            }

            Menu {
                id: modeMenu

                MenuItem {
                    text: qsTr("Plain text")
                    checkable: true
                    checked: root.editorBackend && !root.editorBackend.markdown
                    onTriggered: root.setMarkdownMode(false)
                }
                MenuItem {
                    text: qsTr("Markdown")
                    checkable: true
                    checked: root.editorBackend && root.editorBackend.markdown
                    onTriggered: root.setMarkdownMode(true)
                }
            }
        }

        ToolSeparator {
            visible: !root.compact
            Layout.preferredHeight: root.controlSize - 8
            Layout.alignment: Qt.AlignVCenter
        }

        ToolButton {
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("B")
            font.pixelSize: 18
            font.bold: true
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Bold")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.applyActiveInlineStyle("bold")
        }
        ToolButton {
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("I")
            font.pixelSize: 18
            font.italic: true
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Italic")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.applyActiveInlineStyle("italic")
        }
        ToolButton {
            visible: !root.compact
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("S")
            font.pixelSize: 18
            font.strikeout: true
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Strikethrough")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.applyActiveInlineStyle("strike")
        }
        ToolButton {
            visible: !root.compact
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("</>")
            font.pixelSize: 13
            font.family: "monospace"
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Inline code")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.applyActiveInlineStyle("code")
        }
        ToolButton {
            visible: !root.compact
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("🔗")
            font.pixelSize: 16
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Link")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.editActiveLink()
        }

        ToolSeparator {
            visible: !root.compact
            Layout.preferredHeight: root.controlSize - 8
            Layout.alignment: Qt.AlignVCenter
        }

        ToolButton {
            id: headingButton
            visible: !root.compact
            Layout.preferredWidth: 47
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: root.activeBlockStyleLabel() + qsTr(" ▾")
            font.pixelSize: 14
            font.bold: root.activeHeadingLevel() > 0
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Paragraph style: %1").arg(root.activeBlockStyleLabel())
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: headingMenu.open()

            Menu {
                id: headingMenu
                MenuItem {
                    text: qsTr("Normal paragraph")
                    checkable: true
                    checked: root.activeHeadingLevel() === 0
                    onTriggered: root.blockEditor.convertActiveToHeading(0)
                }
                MenuItem {
                    text: qsTr("Heading 1")
                    checkable: true
                    checked: root.activeHeadingLevel() === 1
                    onTriggered: root.blockEditor.convertActiveToHeading(1)
                }
                MenuItem {
                    text: qsTr("Heading 2")
                    checkable: true
                    checked: root.activeHeadingLevel() === 2
                    onTriggered: root.blockEditor.convertActiveToHeading(2)
                }
                MenuItem {
                    text: qsTr("Heading 3")
                    checkable: true
                    checked: root.activeHeadingLevel() === 3
                    onTriggered: root.blockEditor.convertActiveToHeading(3)
                }
                MenuItem {
                    text: qsTr("Heading 4")
                    checkable: true
                    checked: root.activeHeadingLevel() === 4
                    onTriggered: root.blockEditor.convertActiveToHeading(4)
                }
            }
        }

        ToolButton {
            id: listButton
            visible: !root.compact
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("☷")
            font.pixelSize: 20
            padding: 0
            enabled: root.editorBackend !== null
            Accessible.name: qsTr("Insert list")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: listMenu.open()

            Menu {
                id: listMenu
                MenuItem { text: qsTr("Task list"); onTriggered: root.insertList(5) }
                MenuItem { text: qsTr("Numbered list"); onTriggered: root.insertList(1) }
                MenuItem { text: qsTr("Bullet list"); onTriggered: root.insertList(2) }
            }
        }

        ToolButton {
            visible: !root.compact
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "table-symbolic"
                fallbackName: "table-symbolic.svg"
            }
            enabled: root.editorBackend !== null
            Accessible.name: qsTr("Insert table")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.insertTable()
        }
        ToolButton {
            visible: !root.compact && root.platformBackend !== null
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "insert-image-symbolic"
                fallbackName: "insert-image-symbolic.svg"
            }
            enabled: root.platformBackend && root.editorBackend && root.editorBackend.supportsMedia
            Accessible.name: qsTr("Insert image")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.insertImage()
        }
        ToolButton {
            visible: !root.compact
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "edit-find-symbolic"
                fallbackName: "edit-find-symbolic.svg"
            }
            Accessible.name: qsTr("Find in note")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.findRequested()
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true
        }

        ToolButton {
            visible: root.showMobileActions && root.microphoneVisible
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("🎤")
            font.pixelSize: 17
            padding: 0
            Accessible.name: qsTr("Voice input")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.microphoneRequested()
        }

        ToolButton {
            visible: !root.compact && root.showDeleteButton
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("Delete")
            Accessible.name: qsTr("Delete note")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.deleteRequested()
        }

        ToolButton {
            id: overflowButton
            visible: root.compact || root.showMobileActions
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            Layout.alignment: Qt.AlignVCenter
            text: qsTr("☰")
            font.pixelSize: 19
            padding: 0
            Accessible.name: qsTr("More actions")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: overflowMenu.open()

            Menu {
                id: overflowMenu

                MenuItem {
                    text: qsTr("Find")
                    onTriggered: root.findRequested()
                }
                MenuItem {
                    text: qsTr("Copy note")
                    onTriggered: root.copyDocument()
                }

                MenuSeparator { visible: root.compact }

                MenuItem {
                    visible: root.compact
                    text: qsTr("Strikethrough")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    onTriggered: root.blockEditor.applyActiveInlineStyle("strike")
                }
                MenuItem {
                    visible: root.compact
                    text: qsTr("Inline code")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    onTriggered: root.blockEditor.applyActiveInlineStyle("code")
                }
                MenuItem {
                    visible: root.compact
                    text: qsTr("Edit link")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    onTriggered: root.blockEditor.editActiveLink()
                }
                // Menu.visible controls whether the popup itself is open. Binding it to
                // compact therefore tries to open a submenu while the QML object tree is
                // still being finalized. Create compact-only submenus dynamically instead.
                Instantiator {
                    model: root.compact ? ["heading", "list"] : []

                    delegate: Menu {
                        required property string modelData
                        title: modelData === "heading" ? qsTr("Paragraph style") : qsTr("Insert list")
                        enabled: modelData === "heading"
                                 ? root.editorBackend && root.editorBackend.markdown
                                 : root.editorBackend !== null

                        MenuItem {
                            visible: modelData === "heading"
                            text: qsTr("Normal paragraph")
                            onTriggered: root.blockEditor.convertActiveToHeading(0)
                        }
                        MenuItem {
                            visible: modelData === "heading"
                            text: qsTr("Heading 1")
                            onTriggered: root.blockEditor.convertActiveToHeading(1)
                        }
                        MenuItem {
                            visible: modelData === "heading"
                            text: qsTr("Heading 2")
                            onTriggered: root.blockEditor.convertActiveToHeading(2)
                        }
                        MenuItem {
                            visible: modelData === "heading"
                            text: qsTr("Heading 3")
                            onTriggered: root.blockEditor.convertActiveToHeading(3)
                        }
                        MenuItem {
                            visible: modelData === "heading"
                            text: qsTr("Heading 4")
                            onTriggered: root.blockEditor.convertActiveToHeading(4)
                        }
                        MenuItem {
                            visible: modelData === "list"
                            text: qsTr("Task list")
                            onTriggered: root.insertList(5)
                        }
                        MenuItem {
                            visible: modelData === "list"
                            text: qsTr("Numbered list")
                            onTriggered: root.insertList(1)
                        }
                        MenuItem {
                            visible: modelData === "list"
                            text: qsTr("Bullet list")
                            onTriggered: root.insertList(2)
                        }
                    }

                    onObjectAdded: function(index, object) {
                        overflowMenu.insertMenu(6 + index, object)
                    }
                    onObjectRemoved: function(index, object) {
                        overflowMenu.removeMenu(object)
                    }
                }
                MenuItem {
                    visible: root.compact
                    text: qsTr("Insert table")
                    onTriggered: root.insertTable()
                }
                MenuItem {
                    visible: root.compact && root.platformBackend !== null
                    text: qsTr("Insert image")
                    enabled: root.platformBackend && root.editorBackend && root.editorBackend.supportsMedia
                    onTriggered: root.insertImage()
                }

                MenuSeparator { visible: root.showMobileActions }

                MenuItem {
                    visible: root.showMobileActions
                    text: qsTr("Share")
                    onTriggered: root.shareRequested()
                }
                MenuItem {
                    visible: root.showMobileActions
                    text: qsTr("Export")
                    onTriggered: root.exportRequested()
                }
                MenuItem {
                    visible: root.showMobileActions && root.shortcutVisible
                    text: qsTr("Add to Home screen")
                    onTriggered: root.addToHomeScreenRequested()
                }

                MenuSeparator { visible: root.showDeleteButton }
                MenuItem {
                    visible: root.showDeleteButton
                    text: qsTr("Delete")
                    onTriggered: root.deleteRequested()
                }
            }
        }
    }
}
