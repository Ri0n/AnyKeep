pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "reorder" as Reorder

Item {
    id: root

    readonly property color backgroundColor: applicationPalette.base
    property var reorderModel: typeof settingsReorderModel !== "undefined"
                               ? settingsReorderModel : null
    property bool pluginMode: typeof settingsPluginMode !== "undefined"
                              ? Boolean(settingsPluginMode) : false
    property string pluginIconProviderPrefix:
        typeof settingsPluginIconProviderPrefix !== "undefined"
        ? String(settingsPluginIconProviderPrefix) : ""
    property int rowHeight: 46
    readonly property var activePayload: reorderController.sourcePayload
    readonly property int sourceRow: activePayload ? Number(activePayload.sourceRow) : -1
    readonly property int targetRow: reorderController.targetBoundary
                                     ? Number(reorderController.targetBoundary.finalRow) : sourceRow
    readonly property bool dragging: reorderController.dragging
    readonly property int previewCount: reorderController.previewCount
    readonly property int verticalScrollBarInset:
        listView.contentHeight > listView.height
        ? Math.ceil(Math.max(verticalScrollBar.width, verticalScrollBar.implicitWidth)) : 0

    implicitWidth: 360
    implicitHeight: pluginMode ? 280 : 150

    signal configureRequested(string itemId)

    SystemPalette {
        id: applicationPalette
    }

    Rectangle {
        anchors.fill: parent
        color: root.backgroundColor
    }

    Reorder.FlatListReorderController {
        id: reorderController

        anchors.fill: parent
        geometryItem: root
        listView: listView
        model: root.reorderModel
        compensateForScroll: false
        previewObjectName: "settingsDragPreview"
        previewObjectNamePrefix: "settingsDragPreviewItem-"
        keyProvider: function(item) { return String(item.itemId) }
        extentProvider: function() { return root.rowHeight }
        payloadProvider: function(item) {
            return {
                sourceRow: Number(item.index),
                itemId: String(item.itemId)
            }
        }
        commitHandler: function(payload, destination) {
            return root.pluginMode
                    ? root.reorderModel.movePlugin(Number(payload.sourceRow), destination)
                    : root.reorderModel.reorderStorage(Number(payload.sourceRow), destination)
        }
    }

    ListView {
        id: listView

        anchors.fill: parent
        clip: true
        model: root.reorderModel
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: Math.max(1000, count * root.rowHeight)
        currentIndex: -1
        ScrollBar.vertical: ScrollBar { id: verticalScrollBar }

        delegate: ItemDelegate {
            id: rowDelegate

            required property int index
            required property var model
            property bool internalDragActive: false
            readonly property string itemId: root.pluginMode
                                                     ? String(model.pluginId || "")
                                                     : String(model.storageId || "")
            readonly property string subtitle: root.pluginMode
                                               ? String(model.versionText || "") : ""
            readonly property string resolvedIconSource: {
                const source = String(model.iconSource || "")
                if (source.length > 0)
                    return source
                if (root.pluginMode && root.pluginIconProviderPrefix.length > 0)
                    return root.pluginIconProviderPrefix + encodeURIComponent(itemId)
                return ""
            }
            readonly property bool available: root.pluginMode
                                              ? Number(model.loadStatus) === 2
                                              : Boolean(model.accessible)
            readonly property bool configurable: Boolean(model.configurable)
            readonly property real reorderOffset: displacement.displacement
            readonly property bool sourceActive: reorderController.sourceActive(rowDelegate)
            readonly property bool targetBefore: reorderController.targetBefore(rowDelegate)
            readonly property bool targetAfter: reorderController.targetAfter(rowDelegate)

            objectName: "settingsRow-" + itemId
            width: Math.max(0, listView.width - root.verticalScrollBarInset)
            height: root.rowHeight
            leftPadding: 0
            rightPadding: 0
            topPadding: 0
            bottomPadding: 0
            opacity: sourceActive ? 0 : 1
            highlighted: ListView.isCurrentItem && !root.dragging
            hoverEnabled: !root.dragging
            transform: Translate { y: rowDelegate.reorderOffset }
            ToolTip.visible: hovered && String(model.tooltip || "").length > 0
            ToolTip.text: String(model.tooltip || "")

            Reorder.ReorderDisplacement {
                id: displacement

                animationEnabled: root.dragging && !reorderController.committingDrop
                sourceActive: rowDelegate.sourceActive
                targetBefore: rowDelegate.targetBefore
                targetAfter: rowDelegate.targetAfter
                naturalExtent: root.rowHeight
                draggedExtent: root.rowHeight
                displacement: reorderController.rowTranslation(rowDelegate)
            }

            background: Rectangle {
                radius: 4
                color: rowDelegate.highlighted
                       ? rowDelegate.palette.highlight
                       : rowDelegate.hovered
                         ? Qt.rgba(rowDelegate.palette.button.r,
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
                spacing: 8

                Item {
                    Layout.preferredWidth: 26
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

                Item {
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22

                    Image {
                        id: rowIcon
                        anchors.fill: parent
                        source: rowDelegate.resolvedIconSource
                        sourceSize.width: 22
                        sourceSize.height: 22
                        fillMode: Image.PreserveAspectFit
                    }

                    Label {
                        anchors.centerIn: parent
                        visible: rowIcon.status !== Image.Ready
                        text: root.pluginMode ? "◆" : "▣"
                        color: rowDelegate.palette.text
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 0

                    Label {
                        Layout.fillWidth: true
                        text: String(rowDelegate.model.name || "")
                        color: rowDelegate.highlighted
                               ? rowDelegate.palette.highlightedText
                               : Qt.rgba(rowDelegate.palette.text.r,
                                         rowDelegate.palette.text.g,
                                         rowDelegate.palette.text.b,
                                         rowDelegate.available ? 1 : 0.55)
                        font.bold: root.pluginMode && rowDelegate.available
                        elide: Text.ElideRight
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: rowDelegate.subtitle.length > 0
                        text: rowDelegate.subtitle
                        color: rowDelegate.highlighted
                               ? rowDelegate.palette.highlightedText
                               : rowDelegate.palette.placeholderText
                        font.pixelSize: Math.max(9, Application.font.pixelSize - 2)
                        elide: Text.ElideRight
                    }
                }

                Item {
                    Layout.preferredWidth: 36
                    Layout.fillHeight: true

                    ToolButton {
                        anchors.centerIn: parent
                        objectName: "settingsConfigureButton-" + rowDelegate.itemId
                        visible: rowDelegate.configurable
                        width: 32
                        height: 32
                        padding: 6
                        display: AbstractButton.IconOnly
                        contentItem: ThemedIcon {
                            themeName: "preferences-system-symbolic"
                            fallbackName: "preferences-system-symbolic.svg"
                            recolorFallback: true
                            fallbackTintMode: String(rowDelegate.palette.buttonText)
                        }
                        Accessible.name: qsTr("Configure %1")
                                         .arg(String(rowDelegate.model.name || ""))
                        ToolTip.visible: hovered
                        ToolTip.text: Accessible.name
                        onClicked: root.configureRequested(rowDelegate.itemId)
                    }
                }

                CheckBox {
                    id: policyCheck

                    objectName: "settingsPolicyCheck-" + rowDelegate.itemId
                    visible: root.pluginMode
                    tristate: true
                    checkState: {
                        const policy = Number(rowDelegate.model.loadPolicy)
                        return policy === 0 ? Qt.PartiallyChecked
                                           : (policy === 1 ? Qt.Checked : Qt.Unchecked)
                    }
                    Accessible.name: qsTr("Plugin load policy for %1")
                                     .arg(String(rowDelegate.model.name || ""))
                    onClicked: {
                        const policy = checkState === Qt.PartiallyChecked
                                ? 0 : (checkState === Qt.Checked ? 1 : 2)
                        root.reorderModel.setLoadPolicy(rowDelegate.index, policy)
                    }
                }
            }

            onClicked: listView.currentIndex = index

            TapHandler {
                acceptedButtons: Qt.LeftButton
                onDoubleTapped: {
                    if (rowDelegate.configurable)
                        root.configureRequested(rowDelegate.itemId)
                }
            }
        }
    }
}
