import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Popup {
    id: root
    required property var editorView
    objectName: "inlineDatePickerPopup"
    parent: Overlay.overlay
    modal: false
    focus: true
    padding: editorView.touchMode ? 12 : 8
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    width: editorView.touchMode
           ? Math.max(280, parent ? Math.min(360, parent.width - 24) : 320)
           : Math.max(280, parent ? Math.min(320, parent.width - 16) : 304)

    property var editor: null
    property int selectionStart: 0
    property int selectionEnd: 0
    property date selectedDate: new Date()
    property int visibleMonth: selectedDate.getMonth()
    property int visibleYear: selectedDate.getFullYear()
    signal dateChosen(date value)

    function normalizedDate(value) {
        return new Date(value.getFullYear(), value.getMonth(), value.getDate())
    }

    function dateFromIso(value) {
        const match = /^(\d{4})-(\d{2})-(\d{2})$/.exec(String(value || ""))
        if (!match)
            return null
        const year = Number(match[1])
        const month = Number(match[2])
        const day = Number(match[3])
        if (year < 1 || month < 1 || month > 12 || day < 1 || day > 31)
            return null
        const result = new Date(0)
        result.setHours(0, 0, 0, 0)
        result.setFullYear(year, month - 1, day)
        if (result.getFullYear() !== year || result.getMonth() !== month - 1
                || result.getDate() !== day)
            return null
        return result
    }

    function sameDay(left, right) {
        return left && right && left.getFullYear() === right.getFullYear()
                && left.getMonth() === right.getMonth() && left.getDate() === right.getDate()
    }

    function shiftMonth(delta) {
        const shifted = new Date(visibleYear, visibleMonth + delta, 1)
        visibleYear = shifted.getFullYear()
        visibleMonth = shifted.getMonth()
    }

    function positionForTarget(target, start) {
        if (!target || !Overlay.overlay)
            return
        const rectangle = target.positionToRectangle(Math.max(0, start))
        const point = target.mapToItem(Overlay.overlay, rectangle.x, rectangle.y + rectangle.height)
        const inset = editorView.touchMode ? 12 : 8
        x = Math.max(inset, Math.min(Overlay.overlay.width - width - inset, point.x))
        const popupHeight = implicitHeight > 0 ? implicitHeight : 300
        if (point.y + 6 + popupHeight <= Overlay.overlay.height - inset)
            y = point.y + 6
        else
            y = Math.max(inset, point.y - rectangle.height - popupHeight - 6)
    }

    function openFor(target, start, end, dateText) {
        editor = target
        selectionStart = Math.max(0, Number(start))
        selectionEnd = Math.max(selectionStart, Number(end))
        const parsed = dateFromIso(dateText)
        selectedDate = parsed ? parsed : normalizedDate(new Date())
        visibleMonth = selectedDate.getMonth()
        visibleYear = selectedDate.getFullYear()
        open()
        Qt.callLater(function() {
            if (root.visible && root.editor)
                root.positionForTarget(root.editor, root.selectionStart)
        })
    }

    onClosed: {
        if (editor)
            editor.forceActiveFocus()
    }

    contentItem: ColumnLayout {
        spacing: root.editorView.touchMode ? 8 : 5

        RowLayout {
            Layout.fillWidth: true
            ToolButton {
                text: "‹"
                Layout.minimumWidth: root.editorView.touchMode ? 44 : 34
                Layout.minimumHeight: root.editorView.touchMode ? 44 : 32
                onClicked: root.shiftMonth(-1)
            }
            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: monthGrid.title
                font.bold: true
            }
            ToolButton {
                text: "›"
                Layout.minimumWidth: root.editorView.touchMode ? 44 : 34
                Layout.minimumHeight: root.editorView.touchMode ? 44 : 32
                onClicked: root.shiftMonth(1)
            }
        }

        DayOfWeekRow {
            Layout.fillWidth: true
            locale: monthGrid.locale
        }

        MonthGrid {
            id: monthGrid
            Layout.fillWidth: true
            Layout.preferredHeight: root.editorView.touchMode ? 252 : 210
            month: root.visibleMonth
            year: root.visibleYear
            locale: Qt.locale()

            delegate: Rectangle {
                required property var model
                implicitWidth: root.editorView.touchMode ? 42 : 36
                implicitHeight: root.editorView.touchMode ? 40 : 32
                radius: Math.min(width, height) / 2
                color: root.sameDay(model.date, root.selectedDate)
                       ? root.palette.highlight : "transparent"
                border.width: model.today && !root.sameDay(model.date, root.selectedDate) ? 1 : 0
                border.color: root.palette.highlight
                opacity: model.month === monthGrid.month ? 1 : 0.35

                Text {
                    anchors.centerIn: parent
                    text: parent.model.day
                    font: monthGrid.font
                    color: root.sameDay(parent.model.date, root.selectedDate)
                           ? root.palette.highlightedText : root.palette.text
                }
            }

            onClicked: function(date) {
                root.selectedDate = root.normalizedDate(date)
                root.dateChosen(root.selectedDate)
                root.close()
            }
        }
    }
}
