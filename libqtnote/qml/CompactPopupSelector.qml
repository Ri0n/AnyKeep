pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Control {
    id: root

    property var model: []
    property string textRole: "text"
    property string valueRole: "value"
    property int currentIndex: -1
    property int minimumControlWidth: 48
    property int minimumPopupWidth: 180
    property int maximumPopupHeight: 360
    property int itemHorizontalPadding: 12
    property color backgroundColor: palette.alternateBase
    property color borderColor: palette.midlight
    property color hoverColor: palette.button

    readonly property int count: {
        if (!model)
            return 0
        if (model.count !== undefined)
            return Number(model.count)
        if (model.length !== undefined)
            return Number(model.length)
        return 0
    }
    readonly property string displayText: String(textAt(currentIndex) || "")
    readonly property var currentValue: valueAt(currentIndex)
    readonly property bool popupVisible: selectorPopup.visible
    readonly property real popupImplicitWidth: {
        let maximumWidth = selectorFontMetrics.advanceWidth(displayText)
        for (let index = 0; index < count; ++index)
            maximumWidth = Math.max(maximumWidth,
                                    selectorFontMetrics.advanceWidth(String(textAt(index) || "")))
        return Math.max(minimumPopupWidth,
                        Math.ceil(maximumWidth) + itemHorizontalPadding * 2 + 12)
    }

    signal activated(int index, var value)

    hoverEnabled: true
    activeFocusOnTab: true
    leftPadding: 8
    rightPadding: dropIndicator.implicitWidth + 12
    topPadding: 2
    bottomPadding: 2
    implicitHeight: Math.max(24, selectorFontMetrics.height + topPadding + bottomPadding + 4)
    implicitWidth: Math.max(minimumControlWidth,
                            Math.ceil(selectedTextMetrics.advanceWidth) + leftPadding + rightPadding)

    Accessible.role: Accessible.ComboBox
    Accessible.name: displayText

    function modelItem(index) {
        if (index < 0 || index >= count || !model)
            return undefined
        if (typeof model.get === "function")
            return model.get(index)
        return model[index]
    }

    function roleAt(index, roleName) {
        const item = modelItem(index)
        if (item === undefined || item === null)
            return undefined
        if (!roleName || roleName.length === 0)
            return item
        return item[roleName]
    }

    function textAt(index) {
        return roleAt(index, textRole)
    }

    function valueAt(index) {
        return roleAt(index, valueRole)
    }

    function indexOfValue(value) {
        for (let index = 0; index < count; ++index) {
            if (valueAt(index) === value)
                return index
        }
        return -1
    }

    function activateIndex(index) {
        if (index < 0 || index >= count)
            return
        currentIndex = index
        activated(index, valueAt(index))
    }

    function open() {
        if (!enabled || count <= 0)
            return
        selectorPopup.open()
    }

    function close() {
        selectorPopup.close()
    }

    Keys.onPressed: function(event) {
        if (!enabled)
            return
        if (event.key === Qt.Key_Space || event.key === Qt.Key_Return
                || event.key === Qt.Key_Enter || event.key === Qt.Key_Down) {
            open()
            event.accepted = true
        } else if (event.key === Qt.Key_Up) {
            open()
            event.accepted = true
        }
    }

    TextMetrics {
        id: selectedTextMetrics
        font: root.font
        text: root.displayText
    }

    FontMetrics {
        id: selectorFontMetrics
        font: root.font
    }

    contentItem: Label {
        text: root.displayText
        font: root.font
        color: root.palette.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    Label {
        id: dropIndicator
        anchors.right: parent.right
        anchors.rightMargin: 7
        anchors.verticalCenter: parent.verticalCenter
        text: "\u25be"
        color: root.palette.buttonText
        font.pixelSize: Math.max(10, Math.round(selectorFontMetrics.height * 0.7))
    }

    background: Rectangle {
        radius: 4
        color: selectorTapHandler.pressed ? root.palette.mid
                                          : (root.hovered ? root.hoverColor : root.backgroundColor)
        border.width: 1
        border.color: root.borderColor
    }

    HoverHandler {
        cursorShape: Qt.PointingHandCursor
    }

    TapHandler {
        id: selectorTapHandler
        acceptedButtons: Qt.LeftButton
        onTapped: selectorPopup.visible ? root.close() : root.open()
    }

    Popup {
        id: selectorPopup

        readonly property int scrollBarInset:
            selectorList.contentHeight > selectorList.height
            ? Math.ceil(Math.max(selectorScrollBar.width, selectorScrollBar.implicitWidth)) : 0

        parent: root
        x: root.width - width
        y: root.height
        width: Math.max(root.width, root.popupImplicitWidth + scrollBarInset)
        height: Math.min(root.maximumPopupHeight,
                         Math.max(1, selectorList.contentHeight) + topPadding + bottomPadding)
        padding: 4
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent

        onOpened: {
            selectorList.currentIndex = Math.max(0, root.currentIndex)
            selectorList.positionViewAtIndex(selectorList.currentIndex, ListView.Contain)
            Qt.callLater(function() { selectorList.forceActiveFocus() })
        }

        background: Rectangle {
            radius: 4
            color: root.palette.window
            border.width: 1
            border.color: root.borderColor
        }

        contentItem: ListView {
            id: selectorList

            clip: true
            model: root.model
            currentIndex: Math.max(0, root.currentIndex)
            boundsBehavior: Flickable.StopAtBounds
            keyNavigationEnabled: true

            ScrollBar.vertical: ScrollBar {
                id: selectorScrollBar
                policy: ScrollBar.AsNeeded
            }

            delegate: ItemDelegate {
                id: selectorDelegate

                required property int index
                width: Math.max(0, selectorList.width - selectorPopup.scrollBarInset)
                text: String(root.textAt(index) || "")
                font: root.font
                highlighted: selectorList.currentIndex === index
                leftPadding: root.itemHorizontalPadding
                rightPadding: root.itemHorizontalPadding

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            selectorList.currentIndex = selectorDelegate.index
                    }
                }

                onClicked: {
                    root.activateIndex(index)
                    selectorPopup.close()
                    root.forceActiveFocus()
                }
            }

            Keys.onPressed: function(event) {
                if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter
                        || event.key === Qt.Key_Space) {
                    root.activateIndex(currentIndex)
                    selectorPopup.close()
                    root.forceActiveFocus()
                    event.accepted = true
                } else if (event.key === Qt.Key_Escape) {
                    selectorPopup.close()
                    root.forceActiveFocus()
                    event.accepted = true
                }
            }
        }
    }
}
