import QtQuick 2.2
import Sailfish.Silica 1.0

Page {
    allowedOrientations: Orientation.All

    SilicaListView {
        id: list
        anchors.fill: parent
        model: controller.logLines
        header: PageHeader { title: qsTr("Protokoll") }

        PullDownMenu {
            MenuItem {
                text: qsTr("Leeren")
                onClicked: controller.clearLog()
            }
            MenuItem {
                text: qsTr("In Zwischenablage kopieren")
                onClicked: Clipboard.text = controller.logLines.join("\n")
            }
        }

        delegate: Label {
            x: Theme.horizontalPageMargin
            width: list.width - 2 * Theme.horizontalPageMargin
            text: modelData
            wrapMode: Text.Wrap
            font.pixelSize: Theme.fontSizeExtraSmall
            font.family: "monospace"
        }
        onCountChanged: positionViewAtEnd()
        VerticalScrollDecorator {}
    }
}
