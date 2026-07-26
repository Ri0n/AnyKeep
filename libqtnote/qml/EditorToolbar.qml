import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ToolBar {
    id: root

    required property var editorBackend
    required property var blockEditor
    property var platformBackend: null
    property bool compact: false
    property bool showBackButton: false
    property bool showDeleteButton: false
    property bool showMobileActions: false
    property bool showDesktopActions: false
    property bool microphoneVisible: false
    property bool microphoneBusy: false
    property bool microphoneHoldToRecord: false
    property bool shortcutVisible: false
    property bool pinActionsVisible: false
    property bool pinVisible: false
    property bool alwaysOnTop: false

    signal backRequested()
    signal deleteRequested()
    signal findRequested()
    signal shareRequested()
    signal exportRequested()
    signal printRequested()
    signal pinRequested()
    signal alwaysOnTopRequested(bool enabled)
    signal microphoneRequested()
    signal microphoneReleased()
    signal addToHomeScreenRequested()

    readonly property int bulletListType: 1
    readonly property int taskListType: 2
    readonly property int numberedListType: 5

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
        if (!root.editorBackend || !root.blockEditor || root.editorBackend.markdown === markdown)
            return
        root.blockEditor.flushPendingEditorChanges()
        root.editorBackend.markdown = markdown
    }

    function toggleMarkdownMode() {
        if (root.editorBackend)
            root.setMarkdownMode(!root.editorBackend.markdown)
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

    readonly property int controlSize: 36
    readonly property int iconSize: 20
    readonly property int mandatoryButtonCount: 3
                                                + (showBackButton ? 1 : 0)
                                                + (microphoneVisible ? 1 : 0)
                                                + (showDeleteButton ? 1 : 0)
    readonly property real optionalWidth: width - 16
                                          - mandatoryButtonCount * controlSize
                                          - Math.max(0, mandatoryButtonCount - 1) * 2
    readonly property int optionalSlotCount: Math.max(0, Math.floor(optionalWidth / (controlSize + 2)))
    readonly property int styleSlot: platformBackend !== null ? 4 : 3
    implicitHeight: controlSize + 8

    function themedIconSource(themeName, fallbackName, tintMode) {
        const tint = tintMode && tintMode.length > 0 ? tintMode : "auto"
        return "image://qtnoteicons/" + encodeURIComponent(themeName)
                + "/" + encodeURIComponent(fallbackName)
                + "/" + encodeURIComponent(tint)
    }

    component ThemedIconContent: Item {
        id: iconContent
        required property string themeName
        required property string fallbackName
        property string tintMode: root.showMobileActions ? "light" : "auto"
        property int pixelSize: root.iconSize
        implicitWidth: pixelSize
        implicitHeight: pixelSize

        Image {
            anchors.centerIn: parent
            width: iconContent.pixelSize
            height: iconContent.pixelSize
            source: root.themedIconSource(iconContent.themeName, iconContent.fallbackName,
                                          iconContent.tintMode)
            sourceSize.width: iconContent.pixelSize
            sourceSize.height: iconContent.pixelSize
            fillMode: Image.PreserveAspectFit
            smooth: true
            opacity: iconContent.enabled ? 1.0 : 0.38
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.margins: 4
        spacing: 2

        ToolButton {
            visible: root.showBackButton
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            text: qsTr("‹")
            font.pixelSize: 27
            padding: 0
            Accessible.name: qsTr("Back")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.backRequested()
        }

        ToolButton {
            id: microphoneButton
            visible: root.microphoneVisible
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            padding: 0
            enabled: !root.microphoneBusy
            display: AbstractButton.IconOnly
            contentItem: Item {
                implicitWidth: root.iconSize
                implicitHeight: root.iconSize

                ThemedIconContent {
                    anchors.centerIn: parent
                    visible: !root.microphoneBusy
                    themeName: "audio-input-microphone-symbolic"
                    fallbackName: "microphone.svg"
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    width: root.iconSize
                    height: root.iconSize
                    visible: root.microphoneBusy
                    running: visible
                }
            }
            Accessible.name: root.microphoneHoldToRecord ? qsTr("Hold to dictate text") : qsTr("Voice input")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onPressed: {
                if (root.microphoneHoldToRecord)
                    root.microphoneRequested()
            }
            onReleased: {
                if (root.microphoneHoldToRecord)
                    root.microphoneReleased()
            }
            onClicked: {
                if (!root.microphoneHoldToRecord)
                    root.microphoneRequested()
            }
        }

        ToolButton {
            id: modeButton
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            padding: 0
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "__bundled__"
                fallbackName: root.editorBackend && root.editorBackend.markdown ? "markdown.svg" : "txt.svg"
                pixelSize: 22
            }
            Accessible.name: root.editorBackend && root.editorBackend.markdown
                             ? qsTr("Switch to plain text") : qsTr("Switch to Markdown")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.toggleMarkdownMode()
        }

        ToolButton {
            id: listButton
            visible: root.optionalSlotCount >= 1
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "format-list-unordered-symbolic"
                fallbackName: "format-list-unordered-symbolic.svg"
            }
            enabled: root.editorBackend !== null
            Accessible.name: qsTr("Insert list")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: listMenu.open()

            Menu {
                id: listMenu
                MenuItem { text: qsTr("Bullet list"); onTriggered: root.insertList(root.bulletListType) }
                MenuItem { text: qsTr("Numbered list"); onTriggered: root.insertList(root.numberedListType) }
                MenuItem { text: qsTr("Task list"); onTriggered: root.insertList(root.taskListType) }
            }
        }

        ToolButton {
            visible: root.optionalSlotCount >= 2
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
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
            visible: root.platformBackend !== null && root.optionalSlotCount >= 3
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "insert-image-symbolic"
                fallbackName: "insert-image-symbolic.svg"
            }
            enabled: root.platformBackend && root.editorBackend && root.editorBackend.canInsertImages
            Accessible.name: qsTr("Insert image")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.insertImage()
        }

        ToolButton {
            id: styleButton
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot
            Layout.preferredWidth: 42
            Layout.preferredHeight: root.controlSize
            text: root.activeBlockStyleLabel() + qsTr(" ▾")
            font.bold: root.activeHeadingLevel() > 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Paragraph style")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: headingMenu.open()

            Menu {
                id: headingMenu
                MenuItem { text: qsTr("Normal paragraph"); onTriggered: root.blockEditor.convertActiveToHeading(0) }
                MenuItem { text: qsTr("Heading 1"); onTriggered: root.blockEditor.convertActiveToHeading(1) }
                MenuItem { text: qsTr("Heading 2"); onTriggered: root.blockEditor.convertActiveToHeading(2) }
                MenuItem { text: qsTr("Heading 3"); onTriggered: root.blockEditor.convertActiveToHeading(3) }
                MenuItem { text: qsTr("Heading 4"); onTriggered: root.blockEditor.convertActiveToHeading(4) }
            }
        }

        ToolButton {
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 1
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
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
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 2
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
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
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 3
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
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
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 4
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            text: qsTr("</>")
            font.family: "monospace"
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Inline code")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.applyActiveInlineStyle("code")
        }
        ToolButton {
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 5
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            text: qsTr("🔗")
            padding: 0
            enabled: root.editorBackend && root.editorBackend.markdown
            Accessible.name: qsTr("Edit link")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.editActiveLink()
        }

        Item {
            Layout.fillWidth: true
            Layout.minimumWidth: 0
            Layout.preferredWidth: 0
            Layout.fillHeight: true
        }

        ToolButton {
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
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

        ToolButton {
            visible: root.showDeleteButton
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            display: AbstractButton.IconOnly
            contentItem: ThemedIconContent {
                themeName: "user-trash-full-symbolic"
                fallbackName: "user-trash-full-symbolic.svg"
            }
            Accessible.name: qsTr("Delete note")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.deleteRequested()
        }

        ToolButton {
            id: overflowButton
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            text: qsTr("⋮")
            font.pixelSize: 23
            padding: 0
            Accessible.name: qsTr("More actions")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: {
                if (root.showMobileActions)
                    mobileOverflowMenu.open()
                else
                    desktopOverflowMenu.open()
            }

            Menu {
                id: desktopOverflowMenu

                MenuItem {
                    text: root.editorBackend && root.editorBackend.undoText.length > 0
                          ? qsTr("Undo %1").arg(root.editorBackend.undoText) : qsTr("Undo")
                    enabled: root.editorBackend && root.editorBackend.canUndo
                    onTriggered: root.editorBackend.undo()
                }
                MenuItem {
                    text: root.editorBackend && root.editorBackend.redoText.length > 0
                          ? qsTr("Redo %1").arg(root.editorBackend.redoText) : qsTr("Redo")
                    enabled: root.editorBackend && root.editorBackend.canRedo
                    onTriggered: root.editorBackend.redo()
                }
                MenuItem { text: qsTr("Copy note"); onTriggered: root.copyDocument() }
                MenuItem { text: qsTr("Find in note"); onTriggered: root.findRequested() }
                MenuItem {
                    text: root.editorBackend && root.editorBackend.markdown
                          ? qsTr("Switch to plain text") : qsTr("Switch to Markdown")
                    onTriggered: root.toggleMarkdownMode()
                }

                Menu {
                    title: qsTr("Insert")
                    MenuItem { text: qsTr("Bullet list"); onTriggered: root.insertList(root.bulletListType) }
                    MenuItem { text: qsTr("Numbered list"); onTriggered: root.insertList(root.numberedListType) }
                    MenuItem { text: qsTr("Task list"); onTriggered: root.insertList(root.taskListType) }
                    MenuItem { text: qsTr("Table"); onTriggered: root.insertTable() }
                    MenuItem {
                        text: qsTr("Image")
                        enabled: root.platformBackend && root.editorBackend && root.editorBackend.canInsertImages
                        onTriggered: root.insertImage()
                    }
                }
                Menu {
                    title: qsTr("Paragraph style")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    MenuItem { text: qsTr("Normal paragraph"); onTriggered: root.blockEditor.convertActiveToHeading(0) }
                    MenuItem { text: qsTr("Heading 1"); onTriggered: root.blockEditor.convertActiveToHeading(1) }
                    MenuItem { text: qsTr("Heading 2"); onTriggered: root.blockEditor.convertActiveToHeading(2) }
                    MenuItem { text: qsTr("Heading 3"); onTriggered: root.blockEditor.convertActiveToHeading(3) }
                    MenuItem { text: qsTr("Heading 4"); onTriggered: root.blockEditor.convertActiveToHeading(4) }
                }
                Menu {
                    title: qsTr("Formatting")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    MenuItem { text: qsTr("Bold"); onTriggered: root.blockEditor.applyActiveInlineStyle("bold") }
                    MenuItem { text: qsTr("Italic"); onTriggered: root.blockEditor.applyActiveInlineStyle("italic") }
                    MenuItem { text: qsTr("Strikethrough"); onTriggered: root.blockEditor.applyActiveInlineStyle("strike") }
                    MenuItem { text: qsTr("Inline code"); onTriggered: root.blockEditor.applyActiveInlineStyle("code") }
                    MenuItem { text: qsTr("Edit link"); onTriggered: root.blockEditor.editActiveLink() }
                }

                MenuSeparator { visible: root.showDesktopActions }
                MenuItem {
                    visible: root.showDesktopActions
                    text: qsTr("Print")
                    onTriggered: root.printRequested()
                }
                MenuItem {
                    visible: root.showDesktopActions
                    text: qsTr("Export")
                    onTriggered: root.exportRequested()
                }
                Menu {
                    enabled: root.pinActionsVisible
                    title: qsTr("Pin")
                    MenuItem {
                        text: qsTr("Pin to desktop")
                        enabled: root.pinVisible
                        onTriggered: root.pinRequested()
                    }
                    MenuItem {
                        text: qsTr("Keep on top")
                        checkable: true
                        checked: root.alwaysOnTop
                        onTriggered: root.alwaysOnTopRequested(!root.alwaysOnTop)
                    }
                }

                MenuSeparator { visible: root.showDeleteButton }
                MenuItem {
                    visible: root.showDeleteButton
                    text: qsTr("Delete")
                    onTriggered: root.deleteRequested()
                }
            }

            Menu {
                id: mobileOverflowMenu

                MenuItem {
                    text: root.editorBackend && root.editorBackend.undoText.length > 0
                          ? qsTr("Undo %1").arg(root.editorBackend.undoText) : qsTr("Undo")
                    enabled: root.editorBackend && root.editorBackend.canUndo
                    onTriggered: root.editorBackend.undo()
                }
                MenuItem {
                    text: root.editorBackend && root.editorBackend.redoText.length > 0
                          ? qsTr("Redo %1").arg(root.editorBackend.redoText) : qsTr("Redo")
                    enabled: root.editorBackend && root.editorBackend.canRedo
                    onTriggered: root.editorBackend.redo()
                }
                MenuItem { text: qsTr("Copy note"); onTriggered: root.copyDocument() }
                MenuItem { text: qsTr("Find in note"); onTriggered: root.findRequested() }
                MenuItem {
                    text: root.editorBackend && root.editorBackend.markdown
                          ? qsTr("Switch to plain text") : qsTr("Switch to Markdown")
                    onTriggered: root.toggleMarkdownMode()
                }

                Menu {
                    title: qsTr("Insert")
                    MenuItem { text: qsTr("Bullet list"); onTriggered: root.insertList(root.bulletListType) }
                    MenuItem { text: qsTr("Numbered list"); onTriggered: root.insertList(root.numberedListType) }
                    MenuItem { text: qsTr("Task list"); onTriggered: root.insertList(root.taskListType) }
                    MenuItem { text: qsTr("Table"); onTriggered: root.insertTable() }
                    MenuItem {
                        text: qsTr("Image")
                        enabled: root.platformBackend && root.editorBackend && root.editorBackend.canInsertImages
                        onTriggered: root.insertImage()
                    }
                }
                Menu {
                    title: qsTr("Paragraph style")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    MenuItem { text: qsTr("Normal paragraph"); onTriggered: root.blockEditor.convertActiveToHeading(0) }
                    MenuItem { text: qsTr("Heading 1"); onTriggered: root.blockEditor.convertActiveToHeading(1) }
                    MenuItem { text: qsTr("Heading 2"); onTriggered: root.blockEditor.convertActiveToHeading(2) }
                    MenuItem { text: qsTr("Heading 3"); onTriggered: root.blockEditor.convertActiveToHeading(3) }
                    MenuItem { text: qsTr("Heading 4"); onTriggered: root.blockEditor.convertActiveToHeading(4) }
                }
                Menu {
                    title: qsTr("Formatting")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    MenuItem { text: qsTr("Bold"); onTriggered: root.blockEditor.applyActiveInlineStyle("bold") }
                    MenuItem { text: qsTr("Italic"); onTriggered: root.blockEditor.applyActiveInlineStyle("italic") }
                    MenuItem { text: qsTr("Strikethrough"); onTriggered: root.blockEditor.applyActiveInlineStyle("strike") }
                    MenuItem { text: qsTr("Inline code"); onTriggered: root.blockEditor.applyActiveInlineStyle("code") }
                    MenuItem { text: qsTr("Edit link"); onTriggered: root.blockEditor.editActiveLink() }
                }

                MenuSeparator { }
                MenuItem { text: qsTr("Share"); onTriggered: root.shareRequested() }
                MenuItem {
                    visible: root.shortcutVisible
                    height: visible ? implicitHeight : 0
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
