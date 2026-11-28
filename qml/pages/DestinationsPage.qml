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
            SectionHeader { text: qsTr("Navigation") }
            TextSwitch {
                text: qsTr("SailAuto navigiert selbst")
                description: qsTr("Route offline über OSM Scout Server, sonst online (OpenStreetMap-Routenserver). "
                                  + "Aus: Pure Maps übernimmt (Ziel dort bestätigen).")
                checked: controller.ownNavigation
                onCheckedChanged: controller.ownNavigation = checked
            }

            SectionHeader { text: qsTr("Karte im Auto") }
            TextSwitch {
                text: qsTr("Live-Karte")
                description: qsTr("Karte auf der Seite Navigation, geneigt, Fahrtrichtung oben. Offline von OSM Scout Server, "
                                  + "sonst online (OpenFreeMap). Position vom GPS des Autos, sonst vom Telefon.")
                checked: controller.mapEnabled
                onCheckedChanged: controller.mapEnabled = checked
            }
            TextField {
                width: parent.width
                visible: controller.mapEnabled
                label: qsTr("Eigene Kartenadresse, z. B. mit Schlüssel ({z} {x} {y})")
                text: controller.mapUrl
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoAutoUppercase
                EnterKey.onClicked: focus = false
                onActiveFocusChanged: if (!activeFocus) controller.mapUrl = text
            }
            ComboBox {
                visible: controller.mapEnabled
                label: qsTr("Eigene Position")
                currentIndex: controller.setting("map/marker", 0)
                menu: ContextMenu {
                    MenuItem { text: qsTr("Auto (3D)") }
                    MenuItem { text: qsTr("Pfeil") }
                }
                onCurrentIndexChanged: controller.setSetting("map/marker", currentIndex)
            }
            ComboBox {
                visible: controller.mapEnabled
                label: qsTr("Kartenausschnitt")
                description: qsTr("Weit: mehr Straßen sichtbar, alles kleiner")
                currentIndex: controller.setting("map/range", 0) + 1
                menu: ContextMenu {
                    MenuItem { text: qsTr("Nah") }
                    MenuItem { text: qsTr("Normal") }
                    MenuItem { text: qsTr("Weit") }
                }
                onCurrentIndexChanged: controller.setSetting("map/range", currentIndex - 1)
            }
            TextSwitch {
                visible: controller.mapEnabled
                text: qsTr("Karte flüssig")
                description: qsTr("So viele Bilder, wie das Auto nimmt (meist 30/s). Aus: etwa 15/s, braucht weniger Strom.")
                checked: controller.setting("map/smooth", true)
                onCheckedChanged: controller.setSetting("map/smooth", checked)
            }

            SectionHeader { text: qsTr("Anzeige im Auto") }
            ComboBox {
                label: qsTr("Aussehen")
                description: qsTr("Automatisch: hell am Tag, dunkel bei Nacht (Lichtsensor des Autos)")
                currentIndex: controller.setting("ui/theme", 0)
                menu: ContextMenu {
                    MenuItem { text: qsTr("Automatisch") }
                    MenuItem { text: qsTr("Immer hell") }
                    MenuItem { text: qsTr("Immer dunkel") }
                }
                onCurrentIndexChanged: controller.setSetting("ui/theme", currentIndex)
            }
            TextSwitch {
                text: qsTr("Stromsparen")
                description: qsTr("Ruhige Seiten nur 1 Bild/s statt 30, volle Rate sobald sich etwas bewegt oder du tippst.")
                checked: controller.setting("power/save", true)
                onCheckedChanged: controller.setSetting("power/save", checked)
            }
            SectionHeader { text: qsTr("Hinweis") }
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
