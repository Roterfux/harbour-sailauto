import QtQuick 2.2
import Sailfish.Silica 1.0

// Which phone apps appear on the car's "Apps" page. Tapping one in the car
// starts it on the phone; music/radio apps are then controlled from the
// car's music page.
Page {
    id: page
    allowedOrientations: Orientation.All

    property var apps: controller.installedApps()

    SilicaListView {
        anchors.fill: parent
        model: page.apps

        header: Column {
            width: parent.width
            PageHeader { title: qsTr("Apps fürs Auto") }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeSmall
                text: qsTr("Gewählte Apps erscheinen im Auto unter „Apps“. Antippen startet sie am Telefon; "
                           + "Musik- und Radio-Apps steuerst du dann über die Seite „Musik“ im Auto.")
            }
            Item { width: 1; height: Theme.paddingLarge }
        }

        delegate: ListItem {
            contentHeight: Theme.itemSizeMedium
            onClicked: sw.checked = !sw.checked

            Image {
                id: icon
                x: Theme.horizontalPageMargin
                anchors.verticalCenter: parent.verticalCenter
                width: Theme.iconSizeMedium
                height: width
                sourceSize.width: width
                sourceSize.height: height
                source: modelData.icon
                visible: modelData.icon !== ""
            }
            Label {
                anchors {
                    left: icon.right; leftMargin: Theme.paddingMedium
                    right: sw.left; verticalCenter: parent.verticalCenter
                }
                text: modelData.name + (modelData.media ? "  ♫" : "")
                truncationMode: TruncationMode.Fade
                color: sw.checked ? Theme.highlightColor : Theme.primaryColor
            }
            Switch {
                id: sw
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: modelData.selected
                onCheckedChanged: controller.setCarApp(modelData.id, checked)
            }
        }
        VerticalScrollDecorator {}
    }
}
