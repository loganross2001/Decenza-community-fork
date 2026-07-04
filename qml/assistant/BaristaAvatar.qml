import QtQuick
import Decenza

// [barista-fork] Avatar SELECTOR — loads one of four animated character styles chosen in Settings
// (Barista.settings.avatarStyle: "face" | "cup" | "orb" | "bean"). Keeps the public API the overlay
// relies on: a settable `mode` ("idle"|"listening"|"thinking"|"speaking") and `greet()`. The overlay
// needs no changes when the style switches — the Loader swaps the variant and re-forwards mode.
Item {
    id: root

    property string mode: "idle"
    function greet() { if (loader.item) loader.item.greet() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)
    Accessible.ignored: true

    readonly property string _style: (typeof Barista !== "undefined" && Barista.settings)
                                     ? Barista.settings.avatarStyle : "face"

    Loader {
        id: loader
        anchors.fill: parent
        source: {
            switch (root._style) {
                case "cup":  return "qrc:/qml/assistant/avatars/AvatarCup.qml"
                case "orb":  return "qrc:/qml/assistant/avatars/AvatarOrb.qml"
                case "bean": return "qrc:/qml/assistant/avatars/AvatarBean.qml"
                default:     return "qrc:/qml/assistant/avatars/AvatarFace.qml"
            }
        }
        // Forward the live state into whichever variant is loaded (re-runs on every style switch).
        onLoaded: if (item) item.mode = Qt.binding(function() { return root.mode })
    }
}
