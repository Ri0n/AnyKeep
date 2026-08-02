import QtQuick

Item {
    required property string alignment
    required property color tint
    implicitWidth: 18
    implicitHeight: 16

    Repeater {
        model: [14, 10, 13]
        delegate: Rectangle {
            required property int index
            required property int modelData
            width: modelData
            height: 1.5
            radius: 0.75
            color: parent.tint
            y: 3 + index * 4
            x: parent.alignment === "left" ? 1
               : parent.alignment === "right" ? parent.width - width - 1
               : (parent.width - width) / 2
        }
    }
}
