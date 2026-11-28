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
                text: qsTr("Karte entlang der Route vorladen")
                description: qsTr("Bei Routenstart die Karte für die ganze Strecke laden und am Telefon behalten (für Funklöcher, "
                                  + "ohne OSM Scout Server). Etwa 10–20 MB pro 100 km, höchstens 400 MB.")
                checked: controller.setting("map/prefetch", true)
                onCheckedChanged: controller.setSetting("map/prefetch", checked)
            }
            TextSwitch {
                visible: controller.mapEnabled
                text: qsTr("Ferne Karte scharf (anisotrop 16x)")
                description: qsTr("Nur beim Zeichnen mit der GPU: Mipmaps und anisotrope Filterung, kein Flimmern am Horizont.")
                checked: controller.setting("map/aniso", true)
                onCheckedChanged: controller.setSetting("map/aniso", checked)
            }
            TextSwitch {
                visible: controller.mapEnabled
                text: qsTr("Karte flüssig")
                description: qsTr("So viele Bilder, wie das Auto nimmt (meist 30/s). Aus: etwa 15/s, braucht weniger Strom.")
                checked: controller.setting("map/smooth", true)
                onCheckedChanged: controller.setSetting("map/smooth", checked)
            }

            SectionHeader { text: qsTr("Stau und Unfälle (TomTom)") }
            PasswordField {
                id: trafficKey
                width: parent.width
                label: qsTr("TomTom-Schlüssel (developer.tomtom.com, kostenlos)")
                placeholderText: qsTr("TomTom-Schlüssel")
                text: controller.setting("traffic/key", "")
                EnterKey.iconSource: "image://theme/icon-m-enter-close"
                EnterKey.onClicked: focus = false
                onActiveFocusChanged: if (!activeFocus) controller.setSetting("traffic/key", text.trim())
            }
            Label {
                x: Theme.horizontalPageMargin
                width: parent.width - 2 * Theme.horizontalPageMargin
                wrapMode: Text.Wrap
                color: Theme.secondaryHighlightColor
                font.pixelSize: Theme.fontSizeExtraSmall
                text: qsTr("Nur während der eigenen Navigation, alle 5 min für die nächsten 60 km (~12 Abfragen pro Stunde). "
                           + "Diesen Monat: %1 Abfragen.").arg(controller.trafficRequests())
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
            TextSwitch {
                text: qsTr("Hardware-Videokodierer")
                description: qsTr("Bild fürs Auto mit dem Video-Chip des Telefons kodieren (droidmedia): spart viel Akku. "
                                  + "Klappt es nicht, nimmt SailAuto Software und versucht es nicht noch einmal (Protokoll).")
                checked: controller.setting("video/hw", true)
                onCheckedChanged: controller.setSetting("video/hw", checked)
            }
            TextSwitch {
                text: qsTr("Mit der GPU zeichnen")
                description: qsTr("Bildschirm und Karte über OpenGL: weniger Last für den Prozessor. Geht es nicht, "
                                  + "nimmt SailAuto von selbst die CPU (steht im Protokoll).")
                checked: controller.setting("ui/gpu", true)
                onCheckedChanged: controller.setSetting("ui/gpu", checked)
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
