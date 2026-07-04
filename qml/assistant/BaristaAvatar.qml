import QtQuick
import QtQuick.Shapes
import Decenza

// [barista-fork] A friendly procedural vector face for the barista assistant — "gives the AI a face" so the
// user can watch + listen instead of reading everything. Built from Theme-coloured primitives (no external
// art, crisp at any DPI, matches light/dark). Micro-motion (blink, breathe, pupil drift, mouth flaps) is
// what actually reads as "alive".
//
// HONEST LIMIT: the mouth motion is a cadence FAKE — randomised flaps while `speaking`. No TTS provider
// here (OpenAI / ElevenLabs / native) returns visemes or timing, so true lip-sync isn't possible; at tablet
// viewing distance the flap reads convincingly as talking. Upgrade path: ElevenLabs /with-timestamps or a
// Qt 6.8 audio-buffer RMS tap could later drive `mouthOpen` through the same property.
Item {
    id: root

    // Driven by the overlay: speaking > thinking > listening > idle.
    property string mode: "idle"        // "idle" | "listening" | "thinking" | "speaking"
    property real mouthOpen: 0          // 0 = resting smile, 1 = wide open (animated while speaking)

    // One-shot warm greeting (brows up + wide smile), fired when the assistant activates.
    function greet() { greetAnim.restart() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)

    readonly property real _eyeOpen: (mode === "listening" || mode === "speaking") ? 1.0
                                     : (mode === "thinking") ? 0.9 : 1.0
    readonly property real _browLift: (mode === "listening") ? -root.height * 0.03 : 0
    property real _greetBrow: 0
    property real _greetSmile: 0

    Accessible.ignored: true   // decorative — the card exposes the assistant's line for screen readers

    // ---- breathing bob (whole face) ----
    Item {
        id: face
        anchors.fill: parent
        property real bob: 0
        y: bob

        SequentialAnimation on bob {
            running: root.visible
            loops: Animation.Infinite
            NumberAnimation { from: 0; to: -root.height * 0.02; duration: 1600; easing.type: Easing.InOutSine }
            NumberAnimation { from: -root.height * 0.02; to: 0; duration: 1600; easing.type: Easing.InOutSine }
        }

        // ---- head ----
        Rectangle {
            id: head
            anchors.centerIn: parent
            width: parent.width * 0.80
            height: parent.height * 0.84
            radius: width * 0.46
            color: Theme.highlightColor
            border.width: Math.max(1, root.width * 0.012)
            border.color: Qt.darker(Theme.highlightColor, 1.3)
        }

        // ---- eyebrows ----
        Repeater {
            model: 2
            Rectangle {
                width: head.width * 0.20
                height: Math.max(2, root.width * 0.022)
                radius: height / 2
                color: Theme.textColor
                x: head.x + head.width * (index === 0 ? 0.24 : 0.56)
                y: head.y + head.height * 0.30 + root._browLift + root._greetBrow
                Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }
            }
        }

        // ---- eyes (white) + pupils, with blink via scaleY ----
        Repeater {
            id: eyes
            model: 2
            Rectangle {
                id: eyeWhite
                width: head.width * 0.24
                height: width
                radius: width / 2
                color: "white"
                border.width: Math.max(1, root.width * 0.008)
                border.color: Qt.darker(Theme.highlightColor, 1.3)
                x: head.x + head.width * (index === 0 ? 0.16 : 0.60)
                y: head.y + head.height * 0.38
                property real blinkScale: 1.0
                // Vertical-only squash for the blink / eye-open state. QML Items have NO scaleY property
                // (only uniform `scale`), so a Scale transform is the correct way to squash on one axis.
                transform: Scale {
                    origin.x: eyeWhite.width / 2
                    origin.y: eyeWhite.height / 2
                    yScale: root._eyeOpen * eyeWhite.blinkScale
                    Behavior on yScale { NumberAnimation { duration: 90 } }
                }

                // pupil — drifts by state (idle wander / thinking up-aside)
                Rectangle {
                    width: parent.width * 0.42
                    height: width
                    radius: width / 2
                    color: Theme.textColor
                    x: parent.width / 2 - width / 2 + root._pupilX
                    y: parent.height / 2 - height / 2 + root._pupilY
                    Behavior on x { NumberAnimation { duration: 500; easing.type: Easing.InOutQuad } }
                    Behavior on y { NumberAnimation { duration: 500; easing.type: Easing.InOutQuad } }
                }
            }
        }

        // ---- mouth: a rounded bar that opens with mouthOpen; resting = a gentle smile ----
        Rectangle {
            id: mouth
            width: head.width * (0.30 + root._greetSmile * 0.14)
            height: Math.max(root.width * 0.03, root.width * 0.03 + root.mouthOpen * root.width * 0.14)
            radius: height / 2
            color: Qt.darker(Theme.highlightColor, 1.6)
            x: head.x + head.width / 2 - width / 2
            y: head.y + head.height * 0.66
            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
            Behavior on height { NumberAnimation { duration: 70 } }

            // resting smile curve (only visible when the mouth is near-closed)
            Shape {
                anchors.fill: parent
                opacity: 1.0 - Math.min(1.0, root.mouthOpen * 3)
                ShapePath {
                    strokeWidth: Math.max(2, root.width * 0.02)
                    strokeColor: Qt.darker(Theme.highlightColor, 1.6)
                    fillColor: "transparent"
                    capStyle: ShapePath.RoundCap
                    startX: mouth.width * 0.12; startY: mouth.height * 0.35
                    PathQuad { x: mouth.width * 0.88; y: mouth.height * 0.35;
                               controlX: mouth.width * 0.5; controlY: mouth.height * 1.1 }
                }
            }
        }
    }

    // ---- idle blink timer ----
    Timer {
        running: root.visible
        interval: 2500 + Math.floor(Math.random() * 3500)   // 2.5-6s
        repeat: true
        onTriggered: { blinkDown.restart(); interval = 2500 + Math.floor(Math.random() * 3500) }
    }
    SequentialAnimation {
        id: blinkDown
        // close then reopen both eyes
        ScriptAction { script: { eyes.itemAt(0).blinkScale = 0.08; eyes.itemAt(1).blinkScale = 0.08 } }
        PauseAnimation { duration: 100 }
        ScriptAction { script: { eyes.itemAt(0).blinkScale = 1.0; eyes.itemAt(1).blinkScale = 1.0 } }
    }

    // ---- pupil drift ----
    property real _pupilX: 0
    property real _pupilY: 0
    Timer {
        running: root.visible
        interval: 1400
        repeat: true
        onTriggered: {
            if (root.mode === "thinking") {                 // look up-and-aside while thinking
                root._pupilX = root.width * 0.04
                root._pupilY = -root.height * 0.03
            } else {                                        // gentle idle wander
                root._pupilX = (Math.random() - 0.5) * root.width * 0.05
                root._pupilY = (Math.random() - 0.5) * root.height * 0.03
            }
        }
    }

    // ---- speaking mouth flaps (cadence fake) ----
    Timer {
        running: root.visible && root.mode === "speaking"
        interval: 90 + Math.floor(Math.random() * 60)       // 90-150ms, jittered per tick
        repeat: true
        onTriggered: { root.mouthOpen = 0.2 + Math.random() * 0.8; interval = 90 + Math.floor(Math.random() * 60) }
    }
    // snap the mouth shut the moment speaking ends
    onModeChanged: if (mode !== "speaking") mouthOpen = 0

    // ---- one-shot greeting (brows up + wider smile, then settle) ----
    SequentialAnimation {
        id: greetAnim
        ParallelAnimation {
            NumberAnimation { target: root; property: "_greetBrow"; to: -root.height * 0.04; duration: 180 }
            NumberAnimation { target: root; property: "_greetSmile"; to: 1.0; duration: 220; easing.type: Easing.OutBack }
        }
        PauseAnimation { duration: 900 }
        ParallelAnimation {
            NumberAnimation { target: root; property: "_greetBrow"; to: 0; duration: 300 }
            NumberAnimation { target: root; property: "_greetSmile"; to: 0; duration: 400; easing.type: Easing.InOutQuad }
        }
    }
}
