pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "reorder" as Reorder

Item {
    id: root

    property var ruleModel: typeof rulesController !== "undefined" ? rulesController : null
    property string selectedRuleId: ""
    property string editedName: ""
    property int editedCombiner: 0
    property var editedConditions: []
    property var editedActions: []
    property bool editedStopProcessing: false
    property bool dirty: false
    property string localError: ""

    readonly property var conditionKinds: [
        { value: 0, label: ruleModel ? ruleModel.conditionLabel(0) : qsTr("Title matches pattern") },
        { value: 1, label: ruleModel ? ruleModel.conditionLabel(1) : qsTr("Has tag") },
        { value: 2, label: ruleModel ? ruleModel.conditionLabel(2) : qsTr("Text contains") },
        { value: 3, label: ruleModel ? ruleModel.conditionLabel(3) : qsTr("Storage is") }
    ]
    readonly property var actionKinds: [
        { value: 0, label: ruleModel ? ruleModel.actionLabel(0) : qsTr("Assign folder") },
        { value: 1, label: ruleModel ? ruleModel.actionLabel(1) : qsTr("Save to storage") },
        { value: 2, label: ruleModel ? ruleModel.actionLabel(2) : qsTr("Require encryption (planned)") }
    ]
    readonly property var activePayload: reorderController.sourcePayload
    readonly property bool dragging: reorderController.dragging
    readonly property real draggedExtent: reorderController.draggedExtent
    readonly property int previewCount: reorderController.previewCount
    readonly property string visibleError: localError.length > 0
                                       ? localError : (ruleModel ? String(ruleModel.errorString || "") : "")

    objectName: "rulesPage"
    implicitWidth: 680
    implicitHeight: 400

    SystemPalette {
        id: applicationPalette
    }

    function cloneEntries(entries) {
        const copy = []
        for (const entry of entries || [])
            copy.push(Object.assign({}, entry))
        return copy
    }

    function indexForValue(entries, value, propertyName) {
        const key = propertyName || "value"
        for (let index = 0; index < (entries || []).length; ++index) {
            if (String(entries[index][key]) === String(value))
                return index
        }
        return -1
    }

    function loadRule(ruleId) {
        if (!ruleModel || String(ruleId).length === 0)
            return false
        const details = ruleModel.ruleDetails(String(ruleId))
        if (!details || String(details.id || "").length === 0)
            return false
        selectedRuleId = String(details.id)
        editedName = String(details.name || "")
        editedCombiner = Number(details.conditionCombiner || 0)
        editedConditions = cloneEntries(details.conditions)
        editedActions = cloneEntries(details.actions)
        editedStopProcessing = Boolean(details.stopProcessing)
        dirty = false
        localError = ""
        return true
    }

    function saveCurrent() {
        if (!ruleModel || selectedRuleId.length === 0 || !dirty)
            return true
        const saved = ruleModel.updateRule(selectedRuleId, editedName, editedCombiner,
                                           editedConditions, editedActions, editedStopProcessing)
        if (saved) {
            dirty = false
            localError = ""
            return true
        }
        localError = String(ruleModel.errorString || qsTr("Could not save the rule"))
        return false
    }

    function selectRule(ruleId) {
        const target = String(ruleId || "")
        if (target.length === 0 || target === selectedRuleId)
            return
        if (!saveCurrent())
            return
        loadRule(target)
    }

    function selectFirstRule() {
        if (selectedRuleId.length > 0 || listView.count <= 0)
            return
        listView.positionViewAtBeginning()
        Qt.callLater(function() {
            const first = listView.itemAtIndex(0)
            if (first)
                root.selectRule(first.itemId)
        })
    }

    function addRule() {
        if (!saveCurrent() || !ruleModel)
            return
        const id = String(ruleModel.createRule() || "")
        if (id.length === 0) {
            localError = String(ruleModel.errorString || qsTr("Could not create the rule"))
            return
        }
        loadRule(id)
    }

    function removeSelectedRule() {
        if (!ruleModel || selectedRuleId.length === 0)
            return
        const id = selectedRuleId
        if (!ruleModel.removeRule(id)) {
            localError = String(ruleModel.errorString || qsTr("Could not remove the rule"))
            return
        }
        selectedRuleId = ""
        editedName = ""
        editedConditions = []
        editedActions = []
        dirty = false
        Qt.callLater(selectFirstRule)
    }

    function replaceCondition(index, changes) {
        const entries = cloneEntries(editedConditions)
        if (index < 0 || index >= entries.length)
            return
        entries[index] = Object.assign({}, entries[index], changes)
        editedConditions = entries
        dirty = true
    }

    function replaceAction(index, changes) {
        const entries = cloneEntries(editedActions)
        if (index < 0 || index >= entries.length)
            return
        entries[index] = Object.assign({}, entries[index], changes)
        editedActions = entries
        dirty = true
    }

    function addCondition() {
        const entries = cloneEntries(editedConditions)
        entries.push({ kind: 0, value: "*", negated: false })
        editedConditions = entries
        dirty = true
    }

    function removeCondition(index) {
        const entries = cloneEntries(editedConditions)
        entries.splice(index, 1)
        editedConditions = entries
        dirty = true
    }

    function defaultAction(kind) {
        const action = { kind: Number(kind), folderId: "", storageId: "" }
        if (Number(kind) === 1 && ruleModel && ruleModel.storageChoices.length > 0)
            action.storageId = String(ruleModel.storageChoices[0].id || "")
        return action
    }

    function addAction() {
        const entries = cloneEntries(editedActions)
        entries.push(defaultAction(0))
        editedActions = entries
        dirty = true
    }

    function removeAction(index) {
        const entries = cloneEntries(editedActions)
        entries.splice(index, 1)
        editedActions = entries
        dirty = true
    }

    Component.onCompleted: Qt.callLater(selectFirstRule)

    Connections {
        target: root.ruleModel
        enabled: root.ruleModel !== null

        function onModelReset() {
            Qt.callLater(function() {
                if (!root.ruleModel)
                    return
                if (root.selectedRuleId.length > 0
                        && Object.keys(root.ruleModel.ruleDetails(root.selectedRuleId)).length === 0) {
                    root.selectedRuleId = ""
                    root.dirty = false
                }
                if (root.selectedRuleId.length === 0)
                    root.selectFirstRule()
                else if (!root.dirty)
                    root.loadRule(root.selectedRuleId)
            })
        }
    }

    Reorder.FlatListReorderController {
        id: reorderController

        anchors.fill: parent
        geometryItem: root
        listView: listView
        model: root.ruleModel
        compensateForScroll: false
        previewObjectName: "ruleDragPreview"
        previewObjectNamePrefix: "ruleDragPreviewItem-"
        keyProvider: function(item) { return String(item.itemId) }
        extentProvider: function(item) { return Number(item.height) }
        payloadProvider: function(item) {
            return {
                sourceRow: Number(item.index),
                ruleId: String(item.itemId)
            }
        }
        commitHandler: function(payload, destination) {
            return root.ruleModel.moveRule(Number(payload.sourceRow), destination)
        }
    }

    SplitView {
        anchors.fill: parent
        anchors.margins: 8
        orientation: width >= 680 ? Qt.Horizontal : Qt.Vertical

        Frame {
            id: ruleListPane

            SplitView.minimumWidth: 220
            SplitView.preferredWidth: 270
            SplitView.minimumHeight: 150
            SplitView.preferredHeight: 170
            padding: 0

            ColumnLayout {
                anchors.fill: parent
                spacing: 0

                ToolBar {
                    Layout.fillWidth: true

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 6
                        anchors.rightMargin: 6

                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Rules")
                            font.bold: true
                        }
                        ToolButton {
                            objectName: "addRuleButton"
                            text: "+"
                            enabled: root.ruleModel && root.ruleModel.available
                            Accessible.name: qsTr("Add rule")
                            ToolTip.visible: hovered
                            ToolTip.text: Accessible.name
                            onClicked: root.addRule()
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.margins: 12
                    visible: !root.ruleModel || !root.ruleModel.available
                    text: root.ruleModel ? String(root.ruleModel.errorString || qsTr("The rule store is unavailable"))
                                         : qsTr("The rule store is unavailable")
                    color: applicationPalette.text
                    wrapMode: Text.Wrap
                }

                ListView {
                    id: listView

                    objectName: "rulesList"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: root.ruleModel && root.ruleModel.available
                    clip: true
                    model: root.ruleModel
                    boundsBehavior: Flickable.StopAtBounds
                    cacheBuffer: Math.max(1000, count * 58)
                    readonly property int verticalScrollBarInset:
                        contentHeight > height
                        ? Math.ceil(Math.max(verticalScrollBar.width, verticalScrollBar.implicitWidth)) : 0
                    ScrollBar.vertical: ScrollBar { id: verticalScrollBar }

                    delegate: ItemDelegate {
                        id: rowDelegate

                        required property int index
                        required property var model
                        property bool internalDragActive: false
                        readonly property string itemId: String(model.ruleId || "")
                        readonly property bool sourceActive: reorderController.sourceActive(rowDelegate)
                        readonly property bool targetBefore: reorderController.targetBefore(rowDelegate)
                        readonly property bool targetAfter: reorderController.targetAfter(rowDelegate)
                        readonly property real reorderOffset: displacement.displacement

                        objectName: "ruleRow-" + itemId
                        width: Math.max(0, listView.width - listView.verticalScrollBarInset)
                        height: 58
                        padding: 0
                        opacity: sourceActive ? 0 : 1
                        highlighted: root.selectedRuleId === itemId && !root.dragging
                        hoverEnabled: !root.dragging
                        transform: Translate { y: rowDelegate.reorderOffset }

                        Reorder.ReorderDisplacement {
                            id: displacement

                            animationEnabled: root.dragging && !reorderController.committingDrop
                            sourceActive: rowDelegate.sourceActive
                            targetBefore: rowDelegate.targetBefore
                            targetAfter: rowDelegate.targetAfter
                            naturalExtent: rowDelegate.height
                            draggedExtent: root.draggedExtent
                            displacement: reorderController.rowTranslation(rowDelegate)
                        }

                        background: Rectangle {
                            radius: 4
                            color: rowDelegate.highlighted ? rowDelegate.palette.highlight
                                  : rowDelegate.hovered ? Qt.rgba(rowDelegate.palette.button.r,
                                                                    rowDelegate.palette.button.g,
                                                                    rowDelegate.palette.button.b, 0.45)
                                                        : "transparent"

                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                y: 0
                                height: 3
                                visible: rowDelegate.targetBefore
                                color: rowDelegate.palette.highlight
                            }
                            Rectangle {
                                anchors.left: parent.left
                                anchors.right: parent.right
                                y: parent.height - height
                                height: 3
                                visible: rowDelegate.targetAfter
                                color: rowDelegate.palette.highlight
                            }
                        }

                        contentItem: RowLayout {
                            spacing: 6

                            Item {
                                Layout.preferredWidth: 25
                                Layout.fillHeight: true

                                Label {
                                    anchors.centerIn: parent
                                    text: "☰"
                                    color: rowDelegate.palette.mid
                                    font.pixelSize: 16
                                }
                                Reorder.ReorderDragHandle {
                                    anchors.fill: parent
                                    onDragStarted: reorderController.beginDrag(rowDelegate)
                                    onDragMoved: function(dx, dy) {
                                        reorderController.moveDrag(rowDelegate, dx, dy)
                                    }
                                    onDragFinished: reorderController.finishDrag(rowDelegate)
                                }
                            }

                            CheckBox {
                                objectName: "ruleEnabled-" + rowDelegate.itemId
                                checked: Boolean(rowDelegate.model.enabled)
                                Accessible.name: qsTr("Enable %1").arg(String(rowDelegate.model.name || ""))
                                onToggled: {
                                    if (root.ruleModel
                                            && Boolean(rowDelegate.model.enabled) !== checked) {
                                        root.ruleModel.setRuleEnabled(rowDelegate.itemId, checked)
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 0

                                Label {
                                    Layout.fillWidth: true
                                    text: String(rowDelegate.model.name || "")
                                    color: rowDelegate.highlighted ? rowDelegate.palette.highlightedText
                                                                  : rowDelegate.palette.text
                                    elide: Text.ElideRight
                                    font.bold: Boolean(rowDelegate.model.enabled)
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: String(rowDelegate.model.summary || "")
                                    color: rowDelegate.highlighted ? rowDelegate.palette.highlightedText
                                                                  : rowDelegate.palette.placeholderText
                                    elide: Text.ElideRight
                                    font.pixelSize: Math.max(9, Application.font.pixelSize - 2)
                                }
                            }
                        }

                        onClicked: root.selectRule(itemId)
                    }
                }

                Label {
                    Layout.fillWidth: true
                    Layout.margins: 12
                    visible: root.ruleModel && root.ruleModel.available && listView.count === 0
                    text: qsTr("No rules yet. Add one to route notes automatically.")
                    wrapMode: Text.Wrap
                    color: applicationPalette.placeholderText
                }
            }
        }

        Frame {
            id: editorPane

            // SplitView owns this pane's width. Deriving an implicit width
            // from the ScrollView would feed its availableWidth back into the
            // Frame's implicitWidth calculation.
            implicitWidth: 0
            SplitView.fillWidth: true
            SplitView.fillHeight: true
            padding: 0

            ScrollView {
                id: editorScroll

                anchors.fill: parent
                clip: true
                contentWidth: availableWidth

                ColumnLayout {
                    width: editorScroll.availableWidth
                    spacing: 10

                    Label {
                        Layout.fillWidth: true
                        Layout.margins: 16
                        visible: root.selectedRuleId.length === 0
                        text: qsTr("Select a rule, or add a new one.")
                        horizontalAlignment: Text.AlignHCenter
                        color: applicationPalette.placeholderText
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        Layout.margins: 12
                        visible: root.selectedRuleId.length > 0
                        spacing: 10

                        RowLayout {
                            Layout.fillWidth: true

                            Label { text: qsTr("Name") }
                            TextField {
                                id: nameField

                                Layout.fillWidth: true
                                text: root.editedName
                                placeholderText: qsTr("Rule name")
                                onTextEdited: {
                                    root.editedName = text
                                    root.dirty = true
                                }
                            }
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            Label { text: qsTr("Match") }
                            ComboBox {
                                id: combinerBox

                                Layout.preferredWidth: 150
                                model: [
                                    { value: 0, label: qsTr("All conditions") },
                                    { value: 1, label: qsTr("Any condition") }
                                ]
                                textRole: "label"
                                valueRole: "value"
                                currentIndex: root.indexForValue(model, root.editedCombiner)
                                onActivated: function(index) {
                                    root.editedCombiner = Number(model[index].value)
                                    root.dirty = true
                                }
                            }
                            Item { Layout.fillWidth: true }
                            CheckBox {
                                id: stopProcessingCheck

                                text: qsTr("Stop after this rule")
                                checked: root.editedStopProcessing
                                onToggled: {
                                    root.editedStopProcessing = checked
                                    root.dirty = true
                                }
                            }
                        }

                        GroupBox {
                            title: qsTr("Conditions")
                            Layout.fillWidth: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 6

                                Repeater {
                                    model: root.editedConditions

                                    delegate: RowLayout {
                                        required property int index
                                        required property var modelData
                                        readonly property var condition: modelData

                                        Layout.fillWidth: true

                                        ComboBox {
                                            Layout.preferredWidth: 170
                                            model: root.conditionKinds
                                            textRole: "label"
                                            valueRole: "value"
                                            currentIndex: root.indexForValue(root.conditionKinds,
                                                                             Number(condition.kind))
                                            onActivated: function(index) {
                                                root.replaceCondition(parent.index,
                                                                      { kind: Number(model[index].value) })
                                            }
                                        }
                                        TextField {
                                            Layout.fillWidth: true
                                            text: String(condition.value || "")
                                            placeholderText: qsTr("Value")
                                            onTextEdited: root.replaceCondition(parent.index, { value: text })
                                        }
                                        CheckBox {
                                            text: qsTr("Not")
                                            checked: Boolean(condition.negated)
                                            onToggled: root.replaceCondition(parent.index, { negated: checked })
                                        }
                                        ToolButton {
                                            text: "−"
                                            enabled: root.editedConditions.length > 1
                                            Accessible.name: qsTr("Remove condition")
                                            onClicked: root.removeCondition(parent.index)
                                        }
                                    }
                                }

                                Button {
                                    text: qsTr("Add condition")
                                    onClicked: root.addCondition()
                                }
                            }
                        }

                        GroupBox {
                            title: qsTr("Actions")
                            Layout.fillWidth: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 6

                                Repeater {
                                    model: root.editedActions

                                    delegate: ColumnLayout {
                                        required property int index
                                        required property var modelData
                                        readonly property var action: modelData

                                        Layout.fillWidth: true
                                        spacing: 4

                                        RowLayout {
                                            Layout.fillWidth: true

                                            ComboBox {
                                                Layout.preferredWidth: 190
                                                model: root.actionKinds
                                                textRole: "label"
                                                valueRole: "value"
                                                currentIndex: root.indexForValue(root.actionKinds,
                                                                                 Number(action.kind))
                                                onActivated: function(index) {
                                                    root.replaceAction(parent.parent.index,
                                                                       root.defaultAction(model[index].value))
                                                }
                                            }
                                            Item { Layout.fillWidth: true }
                                            ToolButton {
                                                text: "−"
                                                enabled: root.editedActions.length > 1
                                                Accessible.name: qsTr("Remove action")
                                                onClicked: root.removeAction(parent.parent.index)
                                            }
                                        }

                                        ComboBox {
                                            Layout.fillWidth: true
                                            visible: Number(action.kind) === 0
                                            model: root.ruleModel ? root.ruleModel.folderChoices : []
                                            textRole: "label"
                                            valueRole: "id"
                                            currentIndex: root.indexForValue(root.ruleModel
                                                                             ? root.ruleModel.folderChoices : [],
                                                                             String(action.folderId || ""), "id")
                                            onActivated: function(index) {
                                                root.replaceAction(parent.index,
                                                                   { folderId: String(model[index].id || "") })
                                            }
                                        }

                                        ComboBox {
                                            Layout.fillWidth: true
                                            visible: Number(action.kind) === 1
                                            model: root.ruleModel ? root.ruleModel.storageChoices : []
                                            textRole: "label"
                                            valueRole: "id"
                                            currentIndex: root.indexForValue(root.ruleModel
                                                                             ? root.ruleModel.storageChoices : [],
                                                                             String(action.storageId || ""), "id")
                                            onActivated: function(index) {
                                                root.replaceAction(parent.index,
                                                                   { storageId: String(model[index].id || "") })
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: Number(action.kind) === 2
                                            text: qsTr("Encryption enforcement will be added with the encryption policy.")
                                            color: applicationPalette.placeholderText
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }

                                Button {
                                    text: qsTr("Add action")
                                    onClicked: root.addAction()
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: root.visibleError.length > 0
                            text: root.visibleError
                            color: applicationPalette.text
                            wrapMode: Text.Wrap
                        }

                        RowLayout {
                            Layout.fillWidth: true

                            Button {
                                objectName: "saveRuleButton"
                                text: qsTr("Save rule")
                                enabled: root.dirty
                                onClicked: root.saveCurrent()
                            }
                            Button {
                                text: qsTr("Revert")
                                enabled: root.dirty
                                onClicked: root.loadRule(root.selectedRuleId)
                            }
                            Item { Layout.fillWidth: true }
                            Button {
                                objectName: "removeRuleButton"
                                text: qsTr("Remove rule")
                                onClicked: root.removeSelectedRule()
                            }
                        }
                    }
                }
            }
        }
    }
}
