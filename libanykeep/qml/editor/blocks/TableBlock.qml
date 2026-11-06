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
    property int cachedColumnCount: 0
    // Per column: the widest unwrapped cell, and the longest word of the
    // header and of the other cells (the narrowest widths that still wrap
    // between words only), padding included.
    property var comfortableColumnWidths: []
    property var headerWordWidths: []
    property var bodyWordWidths: []
    property var distributedColumnWidths: []
    readonly property real editorPointSize: editorView.editorPointSize
    readonly property string editorFontFamily: editorView.editorFont.family
    readonly property real minimumColumnWidth: editorView.touchMode ? 56 : 42
    readonly property bool tableFocused: {
        const editor = tableRoot.editorView.activeEditor
        return Boolean(editor && editor.tableCell
                       && Number(editor.blockIndex) === Number(block.index))
    }
    width: block.width
    implicitHeight: tableGrid.implicitHeight
    height: implicitHeight

    // The inactive group of the Windows palette can expose a light Base even
    // while the application is dark. These are document surfaces, so keep
    // them stable when the note window loses focus.
    SystemPalette {
        id: activePalette
        colorGroup: SystemPalette.Active
    }

    onTableDataChanged: syncCells()
    // Cells are measured in the editor font: any change of it, or of the
    // cell size and padding, measures them again, once.
    readonly property font editorFont: editorView.editorFont
    onEditorFontChanged: remeasureTimer.restart()
    onEditorPointSizeChanged: remeasureTimer.restart()
    onTouchModeChanged: remeasureTimer.restart()
    Component.onCompleted: syncCells()

    readonly property bool touchMode: editorView.touchMode
    readonly property real cellSidePadding: touchMode ? 8 : 6
    readonly property real columnHandleWidth: touchMode ? 22 : 17

    function cellPadding(cellIndex) {
        return 2 * cellSidePadding + (cellIndex < columns ? columnHandleWidth : 0)
    }

    // { natural, word } widths of the given cells, padding included, measured
    // from their Markdown without any cell editor. firstIndex is the cell
    // index of values[0]; the header row is bold.
    function measuredCells(values, firstIndex) {
        const backend = tableRoot.editorView.editorBackend
        let widths = []
        if (backend && values.length > 0) {
            const rendered = []
            for (let index = 0; index < values.length; ++index)
                rendered.push(tableRoot.editorView.markdownTableCellForRendering(String(values[index] || "")))
            // The cells' own font: the editor font at the cells' point size.
            // The font is only read: value types are references in QML, and
            // writing to one would write to the editor's font property.
            widths = backend.markdownCellWidths(rendered, tableRoot.editorView.editorFont, editorPointSize,
                                                Math.max(0, columns - firstIndex))
        }
        const result = []
        for (let index = 0; index < values.length; ++index) {
            const padding = cellPadding(firstIndex + index)
            const measured = widths[index] || { natural: 0, word: 0 }
            result.push({
                natural: Math.max(1, Number(measured.natural) + padding),
                word: Math.max(1, Number(measured.word) + padding)
            })
        }
        return result
    }

    // Column widths follow from all cells at once, after every cell has
    // been measured.
    function applyColumnMetrics(cells) {
        const natural = new Array(columns).fill(1)
        const headerWord = new Array(columns).fill(0)
        const bodyWord = new Array(columns).fill(0)
        for (let index = 0; index < cells.length && columns > 0; ++index) {
            const column = index % columns
            natural[column] = Math.max(natural[column], cells[index].natural)
            if (index < columns)
                headerWord[column] = cells[index].word
            else
                bodyWord[column] = Math.max(bodyWord[column], cells[index].word)
        }
        comfortableColumnWidths = natural
        headerWordWidths = headerWord
        bodyWordWidths = bodyWord
        distributeColumnWidths()
    }

    function modelCells() {
        const cells = []
        for (let index = 0; index < cellModel.count; ++index) {
            const cell = cellModel.get(index)
            cells.push({ natural: Number(cell.comfortableWidth || 1), word: Number(cell.wordWidth || 0) })
        }
        return cells
    }

    function syncCells() {
        const values = tableData.values || []
        const shapeChanged = cachedColumnCount !== columns
                || cellModel.count !== values.length
        // A new or reshaped table measures all its cells and decides the
        // column widths before any cell is created or laid out again.
        const measured = shapeChanged ? measuredCells(values, 0) : null
        if (shapeChanged) {
            cachedColumnCount = columns
            applyColumnMetrics(measured)
        }
        while (cellModel.count > values.length)
            cellModel.remove(cellModel.count - 1)
        const changed = []
        for (let index = 0; index < cellModel.count; ++index) {
            const value = values[index] || ""
            if (cellModel.get(index).cellText !== value) {
                cellModel.setProperty(index, "cellText", value)
                if (!shapeChanged)
                    changed.push(index)
            }
            if (shapeChanged) {
                cellModel.setProperty(index, "comfortableWidth", measured[index].natural)
                cellModel.setProperty(index, "wordWidth", measured[index].word)
            }
        }
        while (cellModel.count < values.length) {
            const index = cellModel.count
            cellModel.append({
                cellText: values[index] || "",
                comfortableWidth: measured[index].natural,
                wordWidth: measured[index].word
            })
        }
        for (const index of changed)
            updateCellWidths(index, measuredCells([values[index] || ""], index)[0])
    }

    // After a font or padding change. Existing cells are measured as they
    // are shown, since their documents are not imported again.
    function resetComfortableWidths() {
        const measured = measuredCells(tableData.values || [], 0)
        const backend = tableRoot.editorView.editorBackend
        for (let index = 0; index < cellModel.count && index < measured.length; ++index) {
            const cell = cellRepeater.itemAt(index)
            if (cell && backend) {
                const live = backend.textWidths(cell.textDocument)
                const padding = cell.leftPadding + cell.rightPadding
                if (live.natural !== undefined)
                    measured[index] = {
                        natural: Math.max(1, Number(live.natural) + padding),
                        word: Math.max(1, Number(live.word) + padding)
                    }
            }
            cellModel.setProperty(index, "comfortableWidth", measured[index].natural)
            cellModel.setProperty(index, "wordWidth", measured[index].word)
        }
        applyColumnMetrics(measured)
    }

    function updateCellWidths(cellIndex, widths) {
        if (!widths || cellIndex < 0 || cellIndex >= cellModel.count || columns <= 0)
            return
        const cell = cellModel.get(cellIndex)
        const natural = Math.max(1, Number(widths.natural) || 0)
        const word = Math.max(1, Number(widths.word) || 0)
        if (Math.abs(Number(cell.comfortableWidth || 0) - natural) < 0.25
                && Math.abs(Number(cell.wordWidth || 0) - word) < 0.25)
            return
        cellModel.setProperty(cellIndex, "comfortableWidth", natural)
        cellModel.setProperty(cellIndex, "wordWidth", word)
        columnMetricsTimer.restart()
    }

    // The narrowest width of each column, given the available width:
    // - no word is broken when everything fits;
    // - otherwise words of the other cells give way first, while the header
    //   still wraps between words only;
    // - when even the header words do not fit, the headers are broken too,
    //   down to the general minimum.
    function columnFloors(available) {
        const base = Math.min(minimumColumnWidth, available / columns)
        const header = []
        const body = []
        let headerSum = 0
        let bodySum = 0
        for (let column = 0; column < columns; ++column) {
            header.push(Math.max(base, Number(headerWordWidths[column] || 0)))
            body.push(Math.max(header[column], Number(bodyWordWidths[column] || 0)))
            headerSum += header[column]
            bodySum += body[column]
        }
        if (bodySum <= available)
            return body
        if (headerSum <= available) {
            const share = bodySum > headerSum ? (available - headerSum) / (bodySum - headerSum) : 0
            return body.map(function(value, column) {
                return header[column] + (value - header[column]) * share
            })
        }
        const baseSum = base * columns
        const share = headerSum > baseSum ? Math.max(0, available - baseSum) / (headerSum - baseSum) : 0
        return header.map(function(value) { return base + (value - base) * share })
    }

    function distributeColumnWidths() {
        if (columns <= 0) {
            distributedColumnWidths = []
            return
        }

        const available = Math.max(0, Number(width) || 0)
        const floors = columnFloors(available)
        const result = new Array(columns).fill(0)
        let remainingWidth = available
        let remainingColumns = []
        for (let column = 0; column < columns; ++column)
            remainingColumns.push(column)

        // Give columns that would get less than their floor exactly the floor,
        // then share the rest in proportion to the unwrapped widths. A long
        // URL in one column therefore cannot squeeze its neighbours below
        // their words.
        while (remainingColumns.length > 0) {
            let weightSum = 0
            for (const column of remainingColumns)
                weightSum += Math.max(1, Number(comfortableColumnWidths[column] || 1))
            const clamped = []
            for (const column of remainingColumns) {
                const weight = Math.max(1, Number(comfortableColumnWidths[column] || 1))
                if (remainingWidth * weight / weightSum < floors[column])
                    clamped.push(column)
            }
            if (clamped.length === 0) {
                let assigned = 0
                for (let position = 0; position < remainingColumns.length; ++position) {
                    const column = remainingColumns[position]
                    const columnWidth = position === remainingColumns.length - 1
                            ? remainingWidth - assigned
                            : remainingWidth * Math.max(1, Number(comfortableColumnWidths[column] || 1))
                              / weightSum
                    result[column] = Math.max(0, columnWidth)
                    assigned += columnWidth
                }
                break
            }
            for (const column of clamped) {
                result[column] = floors[column]
                remainingWidth -= floors[column]
            }
            remainingColumns = remainingColumns.filter(function(column) {
                return clamped.indexOf(column) < 0
            })
        }
        distributedColumnWidths = result
    }

    onWidthChanged: distributeColumnWidths()
    onMinimumColumnWidthChanged: distributeColumnWidths()

    Timer {
        id: remeasureTimer
        interval: 0
        onTriggered: tableRoot.resetComfortableWidths()
    }

    Timer {
        id: columnMetricsTimer
        interval: 0
        onTriggered: tableRoot.applyColumnMetrics(tableRoot.modelCells())
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
                required property real comfortableWidth
                required property real wordWidth
                readonly property int columnIndex: tableRoot.columns > 0
                                                   ? index % tableRoot.columns : -1
                readonly property bool headerCell: index < tableRoot.columns
                readonly property bool drawsRightGridBorder: columnIndex === tableRoot.columns - 1
                readonly property bool drawsBottomGridBorder: index >= cellModel.count
                                                               - tableRoot.columns
                readonly property color gridBorderColor: Qt.rgba(activePalette.text.r,
                                                                 activePalette.text.g,
                                                                 activePalette.text.b, 0.28)
                // Qt's Windows dark palette uses AlternateBase as a darkened
                // system accent. Zebra rows should remain neutral instead of
                // inheriting the user's accent colour.
                readonly property color neutralAlternateBase: Qt.rgba(
                    activePalette.base.r * 0.92 + activePalette.text.r * 0.08,
                    activePalette.base.g * 0.92 + activePalette.text.g * 0.08,
                    activePalette.base.b * 0.92 + activePalette.text.b * 0.08,
                    1.0)
                property real columnReorderOffsetX: tableColumnLayout.translationByOrder(
                                                        tableCell,
                                                        tableColumnReorder.targetBoundary,
                                                        tableColumnReorder.draggedExtent)
                readonly property real assignedColumnWidth: columnIndex >= 0
                        && columnIndex < tableRoot.distributedColumnWidths.length
                        ? Number(tableRoot.distributedColumnWidths[columnIndex]) : 0
                Layout.fillWidth: false
                Layout.fillHeight: true
                Layout.preferredWidth: assignedColumnWidth
                Layout.minimumWidth: assignedColumnWidth
                Layout.maximumWidth: assignedColumnWidth
                font.family: tableRoot.editorFontFamily
                font.pointSize: tableRoot.editorPointSize
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

                property bool comfortableWidthUpdatePending: false

                // While the user types, measure the live document as shown.
                function scheduleComfortableWidthUpdate() {
                    if (comfortableWidthUpdatePending)
                        return
                    comfortableWidthUpdatePending = true
                    Qt.callLater(function() {
                        if (!tableCell)
                            return
                        tableCell.comfortableWidthUpdatePending = false
                        const backend = tableRoot.editorView.editorBackend
                        if (!backend)
                            return
                        const widths = backend.textWidths(tableCell.textDocument)
                        const padding = tableCell.leftPadding + tableCell.rightPadding
                        tableRoot.updateCellWidths(tableCell.index, {
                            natural: Number(widths.natural) + padding,
                            word: Number(widths.word) + padding
                        })
                    })
                }

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
                leftPadding: tableRoot.cellSidePadding
                rightPadding: tableRoot.cellSidePadding
                              + (headerCell ? tableRoot.columnHandleWidth : 0)
                topPadding: tableRoot.editorView.touchMode ? 8 : 3
                bottomPadding: tableRoot.editorView.touchMode ? 8 : 3
                background: Rectangle {
                    color: Math.floor(tableCell.index / tableRoot.columns) % 2
                           ? tableCell.neutralAlternateBase : activePalette.base
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
                onTextChanged: {
                    commitChangedText(activeFocus)
                    // Other text changes come from the model, where
                    // syncCells() has measured them already.
                    if (activeFocus)
                        scheduleComfortableWidthUpdate()
                }
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
                    width: tableRoot.columnHandleWidth
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
