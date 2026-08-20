import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../shared" as Shared

ToolBar {
    id: root

    // Keep the command surface visually separate from the document (Base)
    // and aligned with the surrounding window chrome.
    background: Rectangle {
        color: root.palette.window
    }

    required property var editorBackend
    required property var blockEditor
    property var platformBackend: null
    property var folderWorkspace: null
    property bool compact: false
    property bool showBackButton: false
    property bool showDeleteButton: false
    property bool showMobileActions: false
    property bool showDesktopActions: false
    property bool microphoneVisible: false
    property bool microphoneBusy: false
    property bool microphoneRecording: false
    property bool microphoneHoldToRecord: false
    property bool microphoneModeSwitchVisible: false
    property int microphoneMode: 0
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
    signal microphoneModeRequested(int mode)
    signal addToHomeScreenRequested()


    EditorActionControllerImpl {
        id: actions
        editorBackend: root.editorBackend
        blockEditor: root.blockEditor
        platformBackend: root.platformBackend
    }

    readonly property int bulletListType: 1
    readonly property int taskListType: 2
    readonly property int numberedListType: 5

    function runMarkdownCommand(kind, command) { return actions.runMarkdownCommand(kind, command) }
    function insertList(type) { return actions.insertList(type) }
    function insertCodeBlock() { return actions.insertCodeBlock() }
    function insertTable() { return actions.insertTable() }
    function insertBlockQuote() { return actions.insertBlockQuote() }
    function insertImage() { return actions.insertImage() }
    function copyDocument() { return actions.copyDocument() }
    function checkSpellingInNote() {
        if (!root.platformBackend
                || typeof root.platformBackend.checkSpellingInAllDocuments !== "function")
            return
        root.platformBackend.checkSpellingInAllDocuments()
    }

    function assignCurrentNoteFolder(folderId) {
        if (!root.folderWorkspace || !root.editorBackend || !root.blockEditor)
            return false
        // A pending TextArea edit is not necessarily reflected in the C++
        // backend yet.  Flush it before the workspace decides whether the
        // assignment is metadata-only or needs to travel with a dirty draft.
        root.blockEditor.flushPendingEditorChanges()
        return root.folderWorkspace.assignCurrentNoteFolder(folderId)
    }

    function setMarkdownMode(markdown) { actions.setMarkdownMode(markdown) }
    function toggleMarkdownMode() { actions.toggleMarkdownMode() }

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
        if (root.blockEditor && root.blockEditor.activeEditor
                && root.blockEditor.activeEditor.editorField === "blockquote")
            return "\u201c"
        const level = activeHeadingLevel()
        return level > 0 ? qsTr("H%1").arg(level) : qsTr("P")
    }

    function activeInlineStyleEnabled(style) {
        return root.blockEditor && typeof root.blockEditor.activeInlineStyleEnabled === "function"
                && root.blockEditor.activeInlineStyleEnabled(style)
    }

    readonly property int controlSize: 36
    readonly property int iconSize: 24
    readonly property string fallbackIconTintMode: showMobileActions ? "light" : "auto"
    readonly property bool folderPickerAvailable: root.folderWorkspace !== null
                                               && root.folderWorkspace !== undefined
                                               && root.folderWorkspace.folderCatalogAvailable
    readonly property int microphoneSelectorWidth: microphoneVisible && microphoneModeSwitchVisible ? 14 : 0
    readonly property int mandatoryButtonCount: 3
                                                + (showBackButton ? 1 : 0)
                                                + (microphoneVisible ? 1 : 0)
                                                + (folderPickerAvailable ? 1 : 0)
                                                + (showDeleteButton ? 1 : 0)
    readonly property real optionalWidth: width - 16 - microphoneSelectorWidth
                                          - mandatoryButtonCount * controlSize
                                          - Math.max(0, mandatoryButtonCount - 1) * 2
    readonly property int optionalSlotCount: Math.max(0, Math.floor(optionalWidth / (controlSize + 2)))
    readonly property int styleSlot: platformBackend !== null ? 4 : 3
    implicitHeight: controlSize + 10

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 4
        anchors.rightMargin: 4
        anchors.topMargin: 2
        anchors.bottomMargin: 8
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

        Item {
            id: microphoneControl
            visible: root.microphoneVisible
            Layout.preferredWidth: root.controlSize + root.microphoneSelectorWidth
            Layout.preferredHeight: root.controlSize

            ToolButton {
                id: microphoneButton
                anchors.left: parent.left
                anchors.top: parent.top
                width: root.controlSize
                height: root.controlSize
                padding: 0
                // A hold-to-record press must retain its pointer grab while
                // initialization is busy, otherwise disabling the button can
                // swallow onReleased and leave the pending recording running.
                enabled: !root.microphoneBusy || root.microphoneHoldToRecord
                focusPolicy: Qt.NoFocus
                display: AbstractButton.IconOnly
                contentItem: Item {
                    implicitWidth: root.iconSize
                    implicitHeight: root.iconSize

                    Shared.ThemedIconImpl {
                        anchors.centerIn: parent
                        visible: !root.microphoneBusy
                        themeName: "__bundled__"
                        fallbackName: "microphone.svg"
                        recolorFallback: true
                        fallbackTintMode: root.fallbackIconTintMode
                        pixelSize: 22
                    }

                    Rectangle {
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        width: 7
                        height: width
                        radius: width / 2
                        visible: !root.microphoneBusy && root.microphoneRecording
                        color: "#d32f2f"
                        border.width: 1
                        border.color: root.palette.base
                    }

                    BusyIndicator {
                        anchors.centerIn: parent
                        width: root.iconSize
                        height: root.iconSize
                        visible: root.microphoneBusy
                        running: visible
                    }
                }
                Accessible.name: root.microphoneMode === 1
                                 ? (root.microphoneRecording ? qsTr("Stop audio recording")
                                                              : (root.microphoneHoldToRecord
                                                                 ? qsTr("Hold to record audio")
                                                                 : qsTr("Record audio")))
                                 : (root.microphoneHoldToRecord ? qsTr("Hold to dictate text")
                                                                : qsTr("Voice input"))
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
                id: microphoneSelector
                visible: root.microphoneModeSwitchVisible
                anchors.left: microphoneButton.right
                anchors.top: parent.top
                width: root.microphoneSelectorWidth
                height: root.controlSize
                padding: 0
                text: "▾"
                font.pixelSize: 10
                enabled: !root.microphoneBusy && !root.microphoneRecording
                Accessible.name: qsTr("Microphone mode")
                ToolTip.visible: hovered
                ToolTip.text: Accessible.name
                onClicked: microphoneMenu.open()

                Menu {
                    id: microphoneMenu
                    y: microphoneSelector.height

                    MenuItem {
                        text: qsTr("Speech to text")
                        checkable: true
                        autoExclusive: true
                        checked: root.microphoneMode === 0
                        onTriggered: root.microphoneModeRequested(0)
                    }
                    MenuItem {
                        text: qsTr("Audio recording")
                        checkable: true
                        autoExclusive: true
                        checked: root.microphoneMode === 1
                        onTriggered: root.microphoneModeRequested(1)
                    }
                }
            }
        }

        ToolButton {
            id: modeButton
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            padding: 0
            display: AbstractButton.IconOnly
            contentItem: Shared.ThemedIconImpl {
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
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "format-list-unordered-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
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
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "table-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
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
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "insert-image-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
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
                MenuItem { text: qsTr("Block quote"); onTriggered: root.blockEditor.convertActiveToQuote(true) }
            }
        }

        ToolButton {
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 1
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            text: qsTr("B")
            font.pixelSize: 18
            font.bold: true
            checkable: true
            checked: root.activeInlineStyleEnabled("bold")
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
            checkable: true
            checked: root.activeInlineStyleEnabled("italic")
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
            checkable: true
            checked: root.activeInlineStyleEnabled("strike")
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
            padding: 0
            display: AbstractButton.IconOnly
            enabled: root.editorBackend && root.editorBackend.markdown
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "code-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
            }
            Accessible.name: qsTr("Code")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: root.blockEditor.applyActiveInlineStyle("code")
        }
        ToolButton {
            visible: !root.compact && root.optionalSlotCount >= root.styleSlot + 5
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            padding: 0
            display: AbstractButton.IconOnly
            enabled: root.editorBackend && root.editorBackend.markdown
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "insert-link-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
            }
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
            id: folderPickerButton
            objectName: "editorFolderPickerButton"
            visible: root.folderPickerAvailable
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            display: AbstractButton.IconOnly
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "folder-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
            }
            Accessible.name: qsTr("Move to folder")
            ToolTip.visible: hovered
            ToolTip.text: Accessible.name
            onClicked: folderPicker.open()

            Shared.FolderPickerMenuImpl {
                id: folderPicker
                objectName: "editorFolderPicker"
                workspace: root.folderWorkspace
                currentFolderId: root.folderWorkspace
                                 ? String(root.folderWorkspace.currentFolderId || "") : ""
                onFolderSelected: function(folderId) { root.assignCurrentNoteFolder(folderId) }
            }
        }

        ToolButton {
            Layout.preferredWidth: root.controlSize
            Layout.preferredHeight: root.controlSize
            display: AbstractButton.IconOnly
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "edit-find-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: root.iconSize
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
            contentItem: Shared.ThemedIconImpl {
                themeName: "__bundled__"
                fallbackName: "user-trash-full-symbolic.svg"
                recolorFallback: true
                fallbackTintMode: root.fallbackIconTintMode
                pixelSize: 22
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
                    text: qsTr("Check spelling in note")
                    enabled: root.platformBackend && root.platformBackend.spellCheckEnabled
                    onTriggered: root.checkSpellingInNote()
                }
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
                    MenuItem { text: qsTr("Block quote"); onTriggered: root.insertBlockQuote() }
                    MenuItem { text: qsTr("Code block"); onTriggered: root.insertCodeBlock() }
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
                    MenuItem { text: qsTr("Block quote"); onTriggered: root.blockEditor.convertActiveToQuote(true) }
                }
                Menu {
                    title: qsTr("Formatting")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    MenuItem { text: qsTr("Bold"); onTriggered: root.blockEditor.applyActiveInlineStyle("bold") }
                    MenuItem { text: qsTr("Italic"); onTriggered: root.blockEditor.applyActiveInlineStyle("italic") }
                    MenuItem { text: qsTr("Strikethrough"); onTriggered: root.blockEditor.applyActiveInlineStyle("strike") }
                    MenuItem { text: qsTr("Code"); onTriggered: root.blockEditor.applyActiveInlineStyle("code") }
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
                    text: qsTr("Check spelling in note")
                    enabled: root.platformBackend && root.platformBackend.spellCheckEnabled
                    onTriggered: root.checkSpellingInNote()
                }
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
                    MenuItem { text: qsTr("Block quote"); onTriggered: root.insertBlockQuote() }
                    MenuItem { text: qsTr("Code block"); onTriggered: root.insertCodeBlock() }
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
                    MenuItem { text: qsTr("Block quote"); onTriggered: root.blockEditor.convertActiveToQuote(true) }
                }
                Menu {
                    title: qsTr("Formatting")
                    enabled: root.editorBackend && root.editorBackend.markdown
                    MenuItem { text: qsTr("Bold"); onTriggered: root.blockEditor.applyActiveInlineStyle("bold") }
                    MenuItem { text: qsTr("Italic"); onTriggered: root.blockEditor.applyActiveInlineStyle("italic") }
                    MenuItem { text: qsTr("Strikethrough"); onTriggered: root.blockEditor.applyActiveInlineStyle("strike") }
                    MenuItem { text: qsTr("Code"); onTriggered: root.blockEditor.applyActiveInlineStyle("code") }
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
