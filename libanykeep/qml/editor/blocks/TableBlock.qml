import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components" as Editor
import "../support/TableBlockBehavior.js" as TableBlockBehavior
import "../../reorder" as Reorder

Item {
    id: tableRoot
    required property var editorView
    required property var linkEditorPopup

    required property var block
    property var tableData: block.table
    property int columns: Number(tableData.columns || 0)
    readonly property bool tableFocused: {
        const editor = tableRoot.editorView.activeEditor
        return Boolean(editor && editor.tableCell
                       && Number(editor.blockIndex) === Number(block.index))
    }
    width: block.width
    implicitHeight: tableGrid.implicitHeight
    height: implicitHeight

    onTableDataChanged: syncCells()
    Component.onCompleted: syncCells()

    function syncCells() {
        const values = tableData.values || []
        while (cellModel.count > values.length)
            cellModel.remove(cellModel.count - 1)
        while (cellModel.count < values.length)
            cellModel.append({ cellText: "" })
        for (let index = 0; index < values.length; ++index) {
            const value = values[index] || ""
            if (cellModel.get(index).cellText !== value)
                cellModel.setProperty(index, "cellText", value)
        }
    }

    function focusCell(cellIndex, position) {
        tableRoot.editorView.focusEditorAddress({
            blockIndex: block.index,
            listItemIndex: -1,
            tableCellIndex: cellIndex,
            field: "tableCell",
            cursorPosition: position === undefined ? 0 : position
        })
    }

    function focusCellVertically(cellIndex, x, atBottom) {
        Qt.callLater(function() {
            const cell = cellRepeater.itemAt(cellIndex)
            if (!cell)
                return
            cell.forceActiveFocus()
            const y = atBottom ? Math.max(0, cell.height - cell.bottomPadding - 1)
                               : cell.topPadding + 1
            cell.cursorPosition = cell.positionAt(x, y)
            tableRoot.editorView.activeEditor = cell
        })
    }

    function insertRow(tableRow, focusColumn) {
        return tableRoot.editorView.runEditTransaction("insert-table-row", function() {
            tableRoot.editorView.blockModel.insertTableRow(block.index, tableRow)
            focusCell(tableRow * columns + focusColumn, 0)
            return true
        })
    }

    function cellCount() { return cellModel.count }
    function rowCount() { return columns > 0 ? cellModel.count / columns : 0 }
    function cellLength(index) {
        const cell = cellRepeater.itemAt(index)
        return cell ? cell.length : 0
    }
    function rowEmpty(row) {
        for (let column = 0; column < columns; ++column)
            if (String(tableData.values[row * columns + column] || "").trim().length > 0)
                return false
        return true
    }
    function tableEmpty() {
        for (const value of tableData.values || [])
            if (String(value || "").trim().length > 0)
                return false
        return true
    }
    function handleCellKey(event, cell) {
        return TableBlockBehavior.handleKey(tableRoot, tableRoot.editorView, event, cell)
    }

    function headerItems() {
        const result = []
        for (let column = 0; column < columns; ++column) {
            const item = cellRepeater.itemAt(column)
            if (item)
                result.push(item)
        }
        return result
    }

    function columnPreviewItems(column) {
        const result = []
        for (let index = column; index < cellRepeater.count; index += columns) {
            const item = cellRepeater.itemAt(index)
            if (item)
                result.push({ sourceItem: item })
        }
        return result
    }

    function adjustedColumn(column, from, to) {
        if (column === from)
            return to
        if (from < to && column > from && column <= to)
            return column - 1
        if (from > to && column >= to && column < from)
            return column + 1
        return column
    }

    function adjustedFocusAddress(address, from, to) {
        if (!address || Number(address.blockIndex) !== Number(block.index)
                || Number(address.tableCellIndex) < 0) {
            return {
                blockIndex: block.index,
                listItemIndex: -1,
                tableCellIndex: to,
                field: "tableCell",
                cursorPosition: 0,
                selectionStart: 0,
                selectionEnd: 0
            }
        }
        const result = Object.assign({}, address)
        const oldCell = Number(result.tableCellIndex)
        const tableRow = Math.floor(oldCell / columns)
        const oldColumn = oldCell % columns
        result.tableCellIndex = tableRow * columns
                + adjustedColumn(oldColumn, from, to)
        return result
    }

    function startColumnDrag(cell, handle) {
        if (!cell || !handle || columns <= 1)
            return false
        const column = Number(cell.columnIndex)
        const focusAddress = tableRoot.editorView.activeEditor
                && Number(tableRoot.editorView.activeEditor.blockIndex) === Number(block.index)
                ? Object.assign({}, tableRoot.editorView.editorAddress(tableRoot.editorView.activeEditor)) : null
        return tableColumnReorder.beginDrag({
            sources: [{
                item: cell,
                key: column,
                order: column,
                naturalExtent: Number(cell.width)
            }],
            previewItems: columnPreviewItems(column),
            pointerItem: handle,
            payload: {
                from: column,
                focusAddress: focusAddress
            },
            targetByDraggedLeading: true
        })
    }

    ListModel { id: cellModel }

    Reorder.LinearReorderLayout {
        id: tableColumnLayout

        geometryItem: tableRoot
        orientation: Qt.Horizontal
        sourceEntries: tableColumnReorder.sourceEntries
        keyProvider: function(item) { return Number(item.columnIndex) }
        orderProvider: function(item) { return Number(item.columnIndex) }
        extentProvider: function(item) { return Number(item.width) }
        offsetProvider: function(item) { return Number(item.columnReorderOffsetX) }
    }

    Reorder.GenericReorderController {
        id: tableColumnReorder

        anchors.fill: parent
        geometryItem: tableRoot
        orientation: Qt.Horizontal
        compensateForScroll: false
        previewLockCrossAxis: true
        previewObjectName: "tableColumnReorderPreview-" + tableRoot.block.index
        previewObjectNamePrefix: "tableColumnReorderPreviewItem-"
        boundaryProvider: function() {
            return tableColumnLayout.boundaries(tableRoot.headerItems())
        }
        commitHandler: function(payload, boundary) {
            if (!payload || !boundary)
                return false
            const from = Number(payload.from)
            const to = Math.max(0, Math.min(Number(boundary.finalIndex),
                                            tableRoot.columns - 1))
            if (from === to)
                return false
            const focusAddress = tableRoot.adjustedFocusAddress(
                        payload.focusAddress, from, to)
            let moved = false
            tableRoot.editorView.runEditTransaction("move-table-column", function() {
                tableRoot.editorView.prepareForStructuralMutation()
                moved = tableRoot.editorView.blockModel.moveTableColumn(tableRoot.block.index,
                                                        from, to)
                if (moved)
                    tableRoot.editorView.focusEditorAddress(focusAddress)
                return moved
            })
            return moved
        }
    }

    GridLayout {
        id: tableGrid

        width: parent.width
        height: implicitHeight
        columns: tableRoot.columns
        columnSpacing: 0
        rowSpacing: 0

        Repeater {
            id: cellRepeater
            model: cellModel

            Editor.NoteBlockTextArea {
                editorView: tableRoot.editorView
                linkPopup: tableRoot.linkEditorPopup
                id: tableCell

                blockIndex: tableRoot.block.index
                required property int index
                required property string cellText
                readonly property int columnIndex: tableRoot.columns > 0
                                                   ? index % tableRoot.columns : -1
                readonly property bool headerCell: index < tableRoot.columns
                readonly property bool drawsRightGridBorder: columnIndex === tableRoot.columns - 1
                readonly property bool drawsBottomGridBorder: index >= cellModel.count
                                                               - tableRoot.columns
                readonly property color gridBorderColor: Qt.rgba(tableCell.palette.text.r,
                                                                 tableCell.palette.text.g,
                                                                 tableCell.palette.text.b, 0.28)
                property real columnReorderOffsetX: tableColumnLayout.translationByOrder(
                                                        tableCell,
                                                        tableColumnReorder.targetBoundary,
                                                        tableColumnReorder.draggedExtent)
                Layout.fillWidth: true
                Layout.fillHeight: true
                font.bold: headerCell
                sourceText: tableRoot.editorView.markdownTableCellForRendering(cellText)
                textFormat: TextEdit.MarkdownText
                tableCell: true
                tableCellIndex: index
                editorField: "tableCell"
                tableRow: Math.floor(index / tableRoot.columns)
                canRemoveTableRow: cellModel.count / tableRoot.columns > 1
                canRemoveTableColumn: tableRoot.columns > 1
                transform: Translate { x: tableCell.columnReorderOffsetX }

                Behavior on columnReorderOffsetX {
                    enabled: tableColumnReorder.dragging
                             && !tableColumnReorder.committingDrop
                    NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
                }

                keyHandler: function(event) {
                    return tableRoot.handleCellKey(event, tableCell)
                }
                insertRowAbove: function() {
                    tableRoot.insertRow(Math.floor(index / tableRoot.columns),
                                        index % tableRoot.columns)
                }
                insertRowBelow: function() {
                    tableRoot.insertRow(Math.floor(index / tableRoot.columns) + 1,
                                        index % tableRoot.columns)
                }
                removeRow: function() {
                    tableRoot.editorView.runEditTransaction("remove-table-row", function() {
                        const tableRow = Math.floor(index / tableRoot.columns)
                        const targetRow = Math.min(tableRow,
                                                   cellModel.count / tableRoot.columns - 2)
                        const target = targetRow * tableRoot.columns
                                + index % tableRoot.columns
                        tableRoot.editorView.blockModel.removeTableRow(tableRoot.block.index, tableRow)
                        tableRoot.focusCell(target, 0)
                    })
                }
                insertColumnLeft: function() {
                    tableRoot.editorView.runEditTransaction("insert-table-column", function() {
                        const oldColumns = tableRoot.columns
                        const tableRow = Math.floor(index / oldColumns)
                        const column = index % oldColumns
                        tableRoot.editorView.blockModel.insertTableColumn(tableRoot.block.index, column)
                        tableRoot.focusCell(tableRow * (oldColumns + 1) + column, 0)
                    })
                }
                insertColumnRight: function() {
                    tableRoot.editorView.runEditTransaction("insert-table-column", function() {
                        const oldColumns = tableRoot.columns
                        const tableRow = Math.floor(index / oldColumns)
                        const column = index % oldColumns
                        tableRoot.editorView.blockModel.insertTableColumn(tableRoot.block.index, column + 1)
                        tableRoot.focusCell(tableRow * (oldColumns + 1) + column + 1, 0)
                    })
                }
                removeColumn: function() {
                    tableRoot.editorView.runEditTransaction("remove-table-column", function() {
                        const oldColumns = tableRoot.columns
                        const tableRow = Math.floor(index / oldColumns)
                        const column = index % oldColumns
                        tableRoot.editorView.blockModel.removeTableColumn(tableRoot.block.index, column)
                        tableRoot.focusCell(tableRow * (oldColumns - 1)
                                            + Math.min(column, oldColumns - 2), 0)
                    })
                }
                leftPadding: tableRoot.editorView.touchMode ? 8 : 6
                rightPadding: (tableRoot.editorView.touchMode ? 8 : 6)
                              + (headerCell ? columnHandle.width : 0)
                topPadding: tableRoot.editorView.touchMode ? 8 : 3
                bottomPadding: tableRoot.editorView.touchMode ? 8 : 3
                background: Rectangle {
                    color: Math.floor(tableCell.index / tableRoot.columns) % 2
                           ? tableCell.palette.alternateBase : tableCell.palette.base
                    border.width: 0

                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        height: 1
                        color: tableCell.gridBorderColor
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: tableCell.gridBorderColor
                    }

                    Rectangle {
                        visible: tableCell.drawsRightGridBorder
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: tableCell.gridBorderColor
                    }

                    Rectangle {
                        visible: tableCell.drawsBottomGridBorder
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: tableCell.gridBorderColor
                    }
                }
                commitText: function() {
                    tableRoot.editorView.blockModel.setTableCell(
                        tableRoot.block.index, index,
                        tableRoot.editorView.editorBackend.markdownTableCellText(textDocument))
                    // setTableCell echoes the canonical value through
                    // sourceText. The current QTextDocument already owns
                    // that edit; applying the echo on focus loss would
                    // parse its line separators for a second time.
                    sourceTextPending = false
                }
                onTextChanged: commitChangedText(activeFocus)
                onLinkActivated: link => Qt.openUrlExternally(link)

                Item {
                    id: columnHandle

                    readonly property bool activeVisual: columnDragHandle.hovered
                                                         || columnDragHandle.dragging
                    visible: tableCell.headerCell
                    enabled: visible && tableRoot.tableFocused
                    opacity: enabled ? 1 : 0
                    anchors.top: parent.top
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    width: tableRoot.editorView.touchMode ? 22 : 17
                    z: 50

                    Behavior on opacity {
                        NumberAnimation { duration: 480; easing.type: Easing.OutCubic }
                    }

                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: 1
                        radius: 2
                        color: columnHandle.activeVisual
                               ? Qt.rgba(tableCell.palette.highlight.r,
                                         tableCell.palette.highlight.g,
                                         tableCell.palette.highlight.b, 0.20)
                               : Qt.rgba(tableCell.palette.highlight.r,
                                         tableCell.palette.highlight.g,
                                         tableCell.palette.highlight.b, 0.10)
                        border.width: 1
                        border.color: Qt.rgba(tableCell.palette.highlight.r,
                                              tableCell.palette.highlight.g,
                                              tableCell.palette.highlight.b,
                                              columnHandle.activeVisual ? 0.55 : 0.30)

                        Behavior on color {
                            ColorAnimation { duration: 120; easing.type: Easing.OutCubic }
                        }
                    }

                    Row {
                        anchors.centerIn: parent
                        spacing: 2

                        Repeater {
                            model: 2
                            Rectangle {
                                width: 1
                                height: Math.max(9, columnHandle.height * 0.38)
                                color: tableCell.palette.text
                                opacity: columnHandle.activeVisual ? 0.72 : 0.42

                                Behavior on opacity {
                                    NumberAnimation {
                                        duration: 120
                                        easing.type: Easing.OutCubic
                                    }
                                }
                            }
                        }
                    }

                    Reorder.ReorderDragHandle {
                        id: columnDragHandle

                        anchors.fill: parent
                        dragEnabled: tableRoot.columns > 1
                                     && (!tableColumnReorder.dragging || dragging)
                        onTapped: {
                            tableCell.forceActiveFocus()
                            tableRoot.editorView.activeEditor = tableCell
                        }
                        onDragStarted: tableRoot.startColumnDrag(tableCell, columnHandle)
                        onDragMoved: function(dx, dy) {
                            tableColumnReorder.moveDrag(dx, dy)
                        }
                        onDragFinished: tableColumnReorder.finishDrag()
                    }
                }
            }
        }
    }
}
