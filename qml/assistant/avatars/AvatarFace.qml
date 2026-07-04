import QtQuick
import QtQuick.Shapes
import Decenza

// [barista-fork] Avatar style "face" — a refined friendly face (catch-light eyes, brows, cheeks, warm smile).
// Same animated-state contract as every avatar variant: `mode` + `greet()` (+ mouthOpen for the mouth flap).
Item {
    id: root

    property string mode: "idle"        // "idle" | "listening" | "thinking" | "speaking"
    property real mouthOpen: 0
    function greet() { greetAnim.restart() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)
    Accessible.ignored: true

    readonly property real _eyeOpen: (mode === "thinking") ? 0.9 : 1.0
    readonly property real _browLift: (mode === "listening") ? -root.height * 0.03 : 0
    property real _greetBrow: 0
    property real _greetSmile: 0
    property real _pupilX: 0
    property real _pupilY: 0

    Item {
        id: face
        anchors.fill: parent
        property real bob: 0
        y: bob
        SequentialAnimation on bob {
            running: root.visible; loops: Animation.Infinite
            NumberAnimation { from: 0; to: -root.height * 0.02; duration: 1600; easing.type: Easing.InOutSine }
            NumberAnimation { from: -root.height * 0.02; to: 0; duration: 1600; easing.type: Easing.InOutSine }
        }

        Rectangle {
            id: head
            anchors.centerIn: parent
            width: parent.width * 0.80; height: parent.height * 0.84; radius: width * 0.46
            color: Theme.highlightColor
            border.width: Math.max(1, root.width * 0.012); border.color: Qt.darker(Theme.highlightColor, 1.3)
        }

        // cheeks
        Repeater {
            model: 2
            Rectangle {
                width: head.width * 0.17; height: head.height * 0.09; radius: height / 2
                color: "#ff9d6b"; opacity: 0.5
                x: head.x + head.width * (index === 0 ? 0.10 : 0.73); y: head.y + head.height * 0.60
            }
        }

        // eyebrows
        Repeater {
            model: 2
            Rectangle {
                width: head.width * 0.22; height: Math.max(2, root.width * 0.024); radius: height / 2
                color: Theme.textColor; rotation: index === 0 ? -8 : 8
                x: head.x + head.width * (index === 0 ? 0.22 : 0.56)
                y: head.y + head.height * 0.27 + root._browLift + root._greetBrow
                Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutQuad } }
            }
        }

        // eyes (white) + pupils with a catch-light highlight, blink via Scale transform
        Repeater {
            id: eyes
            model: 2
            Rectangle {
                id: eyeWhite
                width: head.width * 0.25; height: width * 1.05; radius: width / 2
                color: "white"
                border.width: Math.max(1, root.width * 0.008); border.color: Qt.darker(Theme.highlightColor, 1.3)
                x: head.x + head.width * (index === 0 ? 0.15 : 0.60); y: head.y + head.height * 0.36
                property real blinkScale: 1.0
                transform: Scale {
                    origin.x: eyeWhite.width / 2; origin.y: eyeWhite.height / 2
                    yScale: root._eyeOpen * eyeWhite.blinkScale
                    Behavior on yScale { NumberAnimation { duration: 90 } }
                }
                Rectangle {
                    id: pupil
                    width: parent.width * 0.46; height: width; radius: width / 2; color: Theme.textColor
                    x: parent.width / 2 - width / 2 + root._pupilX
                    y: parent.height / 2 - height / 2 + root._pupilY
                    Behavior on x { NumberAnimation { duration: 500; easing.type: Easing.InOutQuad } }
                    Behavior on y { NumberAnimation { duration: 500; easing.type: Easing.InOutQuad } }
                    Rectangle {   // catch-light
                        width: parent.width * 0.34; height: width; radius: width / 2; color: "white"
                        x: parent.width * 0.18; y: parent.height * 0.16
                    }
                }
            }
        }

        // mouth: opens with mouthOpen; resting = a warm smile
        Rectangle {
            id: mouth
            width: head.width * (0.32 + root._greetSmile * 0.14)
            height: Math.max(root.width * 0.03, root.width * 0.03 + root.mouthOpen * root.width * 0.14)
            radius: height / 2; color: Qt.darker(Theme.highlightColor, 1.7)
            x: head.x + head.width / 2 - width / 2; y: head.y + head.height * 0.66
            Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutQuad } }
            Behavior on height { NumberAnimation { duration: 70 } }
            Shape {
                anchors.fill: parent
                opacity: 1.0 - Math.min(1.0, root.mouthOpen * 3)
                ShapePath {
                    strokeWidth: Math.max(2, root.width * 0.022); strokeColor: Qt.darker(Theme.highlightColor, 1.7)
                    fillColor: "transparent"; capStyle: ShapePath.RoundCap
                    startX: mouth.width * 0.12; startY: mouth.height * 0.32
                    PathQuad { x: mouth.width * 0.88; y: mouth.height * 0.32; controlX: mouth.width * 0.5; controlY: mouth.height * 1.2 }
                }
            }
        }
    }

    // blink
    Timer {
        running: root.visible; interval: 2500 + Math.floor(Math.random() * 3500); repeat: true
        onTriggered: { blinkDown.restart(); interval = 2500 + Math.floor(Math.random() * 3500) }
    }
    SequentialAnimation {
        id: blinkDown
        ScriptAction { script: { if (eyes.itemAt(0)) { eyes.itemAt(0).blinkScale = 0.08; eyes.itemAt(1).blinkScale = 0.08 } } }
        PauseAnimation { duration: 100 }
        ScriptAction { script: { if (eyes.itemAt(0)) { eyes.itemAt(0).blinkScale = 1.0; eyes.itemAt(1).blinkScale = 1.0 } } }
    }

    // pupil drift
    Timer {
        running: root.visible; interval: 1400; repeat: true
        onTriggered: {
            if (root.mode === "thinking") { root._pupilX = root.width * 0.04; root._pupilY = -root.height * 0.03 }
            else { root._pupilX = (Math.random() - 0.5) * root.width * 0.05; root._pupilY = (Math.random() - 0.5) * root.height * 0.03 }
        }
    }

    // speaking mouth flaps
    Timer {
        running: root.visible && root.mode === "speaking"
        interval: 90 + Math.floor(Math.random() * 60); repeat: true
        onTriggered: { root.mouthOpen = 0.2 + Math.random() * 0.8; interval = 90 + Math.floor(Math.random() * 60) }
    }
    onModeChanged: if (mode !== "speaking") mouthOpen = 0

    // greeting
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
