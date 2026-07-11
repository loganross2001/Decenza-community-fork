import QtQuick
import Decenza

// [barista-fork] Avatar SELECTOR — loads one of four animated character styles chosen in Settings
// (Barista.settings.avatarStyle: "face" | "cup" | "orb" | "bean"). Keeps the public API the overlay
// relies on: a settable `mode` ("idle"|"listening"|"thinking"|"speaking") and `greet()`. The overlay
// needs no changes when the style switches — the Loader swaps the variant and re-forwards mode.
Item {
    id: root

    property string mode: "idle"
    // [barista-fork] "Don't leave the user in silence" Part B: a NON-VERBAL silence-breaker. When a slow op has
    // run ~5s with nothing spoken yet, the overlay sets this true and the avatar does a gentle, more pronounced
    // "still thinking" pulse (a soft breathing scale) ON TOP of whichever variant's thinking animation is
    // showing — so the user sees the barista is actively working. Implemented here at the wrapper so all four
    // avatar styles get it for free (no per-variant changes). Cleared the moment anything is spoken.
    property bool thinkingCue: false
    function greet() { if (loader.item) loader.item.greet() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)
    Accessible.ignored: true

    // [barista-fork] The pronounced thinking beat: a slow, subtle breathing scale of the whole avatar. Only
    // runs while thinkingCue is set (Part B fired) — otherwise the avatar sits at its normal scale (1.0).
    property real _cueScale: 1.0
    transform: Scale {
        origin.x: root.width / 2; origin.y: root.height / 2
        xScale: root._cueScale; yScale: root._cueScale
    }
    SequentialAnimation on _cueScale {
        running: root.thinkingCue
        loops: Animation.Infinite
        alwaysRunToEnd: true   // finish the current beat before stopping → returns cleanly to 1.0
        NumberAnimation { from: 1.0; to: 1.06; duration: 700; easing.type: Easing.InOutSine }
        NumberAnimation { from: 1.06; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
    }
    // Snap back to rest when the cue clears mid-beat (belt-and-suspenders alongside alwaysRunToEnd).
    onThinkingCueChanged: if (!thinkingCue) _cueScale = 1.0

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
