import QtQuick
import QtQuick.Shapes
import Decenza

// [barista-fork] Avatar style "cup" — a coffee-cup mascot with a face and animated steam.
Item {
    id: root
    property string mode: "idle"
    property real mouthOpen: 0
    function greet() { greetAnim.restart() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)
    Accessible.ignored: true

    readonly property color cream: "#f3ead6"
    readonly property color creamEdge: "#cbb98f"
    readonly property int _steamDur: mode === "speaking" ? 900 : 1700   // steam rises faster while talking
    property real _greetSmile: 0

    Item {
        id: scene
        anchors.fill: parent
        property real bob: 0
        y: bob
        SequentialAnimation on bob {
            running: root.visible; loops: Animation.Infinite
            NumberAnimation { from: 0; to: -root.height * 0.015; duration: 1800; easing.type: Easing.InOutSine }
            NumberAnimation { from: -root.height * 0.015; to: 0; duration: 1800; easing.type: Easing.InOutSine }
        }

        // steam wisps (rise + fade, looping)
        Repeater {
            model: 2
            Shape {
                id: wisp
                anchors.fill: parent
                property real t: 0
                opacity: (1.0 - wisp.t) * 0.75
                transform: Translate { y: -wisp.t * root.height * 0.14 }
                ShapePath {
                    strokeWidth: Math.max(3, root.width * 0.035); strokeColor: "#cfd8e0"
                    fillColor: "transparent"; capStyle: ShapePath.RoundCap
                    startX: root.width * (index === 0 ? 0.40 : 0.56); startY: root.height * 0.28
                    PathCubic {
                        x: root.width * (index === 0 ? 0.40 : 0.56); y: root.height * 0.46
                        control1X: root.width * (index === 0 ? 0.31 : 0.47); control1Y: root.height * 0.35
                        control2X: root.width * (index === 0 ? 0.49 : 0.65); control2Y: root.height * 0.40
                    }
                }
                NumberAnimation on t {
                    running: root.visible; loops: Animation.Infinite
                    from: 0; to: 1; duration: root._steamDur
                    // stagger the two wisps
                }
                Component.onCompleted: t = index * 0.5
            }
        }

        // saucer
        Rectangle {
            width: parent.width * 0.62; height: root.height * 0.075; radius: height / 2
            color: root.cream; border.width: 2; border.color: root.creamEdge
            x: parent.width * 0.19; y: parent.height * 0.84
        }
        // handle
        Rectangle {
            width: root.width * 0.17; height: root.height * 0.22; radius: width / 2
            color: "transparent"; border.width: Math.max(5, root.width * 0.05); border.color: root.cream
            x: parent.width * 0.74; y: parent.height * 0.52
        }
        // cup body
        Rectangle {
            id: cup
            width: parent.width * 0.46; height: parent.height * 0.40; radius: root.width * 0.07
            color: root.cream; border.width: 2; border.color: root.creamEdge
            x: parent.width * 0.27; y: parent.height * 0.46
            Rectangle { anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right
                height: parent.height * 0.16; radius: root.width * 0.03; color: Theme.highlightColor }  // rim band

            // eyes
            Repeater {
                id: cupEyes
                model: 2
                Rectangle {
                    id: ceye
                    width: cup.width * 0.16; height: width * 1.1; radius: width / 2; color: Theme.textColor
                    x: cup.width * (index === 0 ? 0.24 : 0.60); y: cup.height * 0.36
                    property real blinkScale: 1.0
                    transform: Scale { origin.x: ceye.width/2; origin.y: ceye.height/2; yScale: ceye.blinkScale
                        Behavior on yScale { NumberAnimation { duration: 90 } } }
                    Rectangle { width: parent.width * 0.4; height: width; radius: width/2; color: "white"; x: parent.width*0.1; y: parent.height*0.12 }
                }
            }
            // mouth
            Rectangle {
                id: cupMouth
                width: cup.width * (0.34 + root._greetSmile * 0.14)
                height: Math.max(root.width * 0.025, root.width * 0.025 + root.mouthOpen * root.width * 0.10)
                radius: height / 2; color: Qt.darker(Theme.highlightColor, 1.7)
                x: cup.width/2 - width/2; y: cup.height * 0.66
                Behavior on width { NumberAnimation { duration: 180 } }
                Behavior on height { NumberAnimation { duration: 70 } }
                Shape { anchors.fill: parent; opacity: 1.0 - Math.min(1.0, root.mouthOpen * 3)
                    ShapePath { strokeWidth: Math.max(2, root.width*0.02); strokeColor: Qt.darker(Theme.highlightColor, 1.7)
                        fillColor: "transparent"; capStyle: ShapePath.RoundCap
                        startX: cupMouth.width*0.15; startY: cupMouth.height*0.35
                        PathQuad { x: cupMouth.width*0.85; y: cupMouth.height*0.35; controlX: cupMouth.width*0.5; controlY: cupMouth.height*1.2 } } }
            }
        }
    }

    Timer {
        running: root.visible; interval: 2800 + Math.floor(Math.random() * 3200); repeat: true
        onTriggered: { blink.restart(); interval = 2800 + Math.floor(Math.random() * 3200) }
    }
    SequentialAnimation {
        id: blink
        ScriptAction { script: { if (cupEyes.itemAt(0)) { cupEyes.itemAt(0).blinkScale = 0.1; cupEyes.itemAt(1).blinkScale = 0.1 } } }
        PauseAnimation { duration: 100 }
        ScriptAction { script: { if (cupEyes.itemAt(0)) { cupEyes.itemAt(0).blinkScale = 1.0; cupEyes.itemAt(1).blinkScale = 1.0 } } }
    }
    Timer {
        running: root.visible && root.mode === "speaking"
        interval: 90 + Math.floor(Math.random() * 60); repeat: true
        onTriggered: { root.mouthOpen = 0.2 + Math.random() * 0.8; interval = 90 + Math.floor(Math.random() * 60) }
    }
    onModeChanged: if (mode !== "speaking") mouthOpen = 0

    SequentialAnimation {
        id: greetAnim
        NumberAnimation { target: root; property: "_greetSmile"; to: 1.0; duration: 220; easing.type: Easing.OutBack }
        PauseAnimation { duration: 900 }
        NumberAnimation { target: root; property: "_greetSmile"; to: 0; duration: 400; easing.type: Easing.InOutQuad }
    }
}
