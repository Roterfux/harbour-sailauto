import QtQuick 2.2
import Sailfish.Silica 1.0

// Home and work address for the car's navigation page ("Zuhause", "Arbeit").
Page {
    allowedOrientations: Orientation.All

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: col.height

        Column {
            id: col
            width: parent.width

            PageHeader { title: qsTr("Ziele") }

            TextField {
                width: parent.width
                label: qsTr("Zuhause")
                placeholderText: qsTr("Straße Nr, PLZ Ort")
                text: controller.homeAddress
                EnterKey.iconSource: "image://theme/icon-m-enter-next"
                EnterKey.onClicked: work.focus = true
                onActiveFocusChanged: if (!activeFocus) controller.homeAddress = text
            }
            TextField {
                id: work
                width: parent.width
                label: qsTr("Arbeit")
                placeholderText: qsTr("Straße Nr, PLZ Ort")
                text: controller.workAddress
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
                onActiveFocusChanged: if (!activeFocus) controller.workAddress = text
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Im Auto unter Navigation: Zuhause/Arbeit antippen schickt die Adresse an Pure Maps. "
                           + "Am Telefon das Ergebnis antippen und „Route“, dann im Auto „Navigation starten“.")
            }
        }
    }
}
