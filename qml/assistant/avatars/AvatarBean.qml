import QtQuick
import QtQuick.Shapes
import Decenza

// [barista-fork] Avatar style "bean" — a coffee-bean character with a cute face.
Item {
    id: root
    property string mode: "idle"
    property real mouthOpen: 0
    function greet() { greetAnim.restart() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)
    Accessible.ignored: true

    readonly property color beanColor: "#4a2f1c"
    readonly property color beanEdge: "#2f1d10"
    readonly property real _eyeOpen: (mode === "thinking") ? 0.9 : 1.0
    property real _greetSmile: 0
    property real _pupilX: 0

    Item {
        id: scene
        anchors.fill: parent
        property real bob: 0
        y: bob
        rotation: -8 + bob * 0.4
        SequentialAnimation on bob {
            running: root.visible; loops: Animation.Infinite
            NumberAnimation { from: -1.5; to: 1.5; duration: 1900; easing.type: Easing.InOutSine }
            NumberAnimation { from: 1.5; to: -1.5; duration: 1900; easing.type: Easing.InOutSine }
        }

        // bean body
        Rectangle {
            id: bean
            anchors.centerIn: parent
            width: parent.width * 0.62; height: parent.height * 0.82; radius: width / 2
            color: root.beanColor; border.width: 2; border.color: root.beanEdge
        }
        // center crease
        Shape {
            anchors.fill: parent
            ShapePath {
                strokeWidth: Math.max(3, root.width * 0.03); strokeColor: root.beanEdge
                fillColor: "transparent"; capStyle: ShapePath.RoundCap
                startX: bean.x + bean.width * 0.55; startY: bean.y + bean.height * 0.12
                PathCubic {
                    x: bean.x + bean.width * 0.55; y: bean.y + bean.height * 0.88
                    control1X: bean.x + bean.width * 0.16; control1Y: bean.y + bean.height * 0.38
                    control2X: bean.x + bean.width * 0.90; control2Y: bean.y + bean.height * 0.62
                }
            }
        }
        // eyes
        Repeater {
            id: beanEyes
            model: 2
            Rectangle {
                id: beye
                width: bean.width * 0.26; height: width * 1.1; radius: width / 2; color: "white"
                x: bean.x + bean.width * (index === 0 ? 0.12 : 0.60); y: bean.y + bean.height * 0.30
                property real blinkScale: 1.0
                transform: Scale { origin.x: beye.width/2; origin.y: beye.height/2; yScale: root._eyeOpen * beye.blinkScale
                    Behavior on yScale { NumberAnimation { duration: 90 } } }
                Rectangle {
                    width: parent.width * 0.5; height: width; radius: width / 2; color: root.beanEdge
                    x: parent.width/2 - width/2 + root._pupilX; y: parent.height/2 - height/2
                    Behavior on x { NumberAnimation { duration: 500 } }
                    Rectangle { width: parent.width * 0.35; height: width; radius: width/2; color: "white"; x: parent.width*0.15; y: parent.height*0.15 }
                }
            }
        }
        // smile (yellow)
        Rectangle {
            id: beanMouth
            width: bean.width * (0.36 + root._greetSmile * 0.16)
            height: Math.max(root.width * 0.028, root.width * 0.028 + root.mouthOpen * root.width * 0.11)
            radius: height / 2; color: Theme.highlightColor
            x: bean.x + bean.width/2 - width/2; y: bean.y + bean.height * 0.66
            Behavior on width { NumberAnimation { duration: 180 } }
            Behavior on height { NumberAnimation { duration: 70 } }
            Shape { anchors.fill: parent; opacity: 1.0 - Math.min(1.0, root.mouthOpen * 3)
                ShapePath { strokeWidth: Math.max(2, root.width*0.022); strokeColor: Theme.highlightColor
                    fillColor: "transparent"; capStyle: ShapePath.RoundCap
                    startX: beanMouth.width*0.14; startY: beanMouth.height*0.32
                    PathQuad { x: beanMouth.width*0.86; y: beanMouth.height*0.32; controlX: beanMouth.width*0.5; controlY: beanMouth.height*1.25 } } }
        }
    }

    Timer {
        running: root.visible; interval: 2600 + Math.floor(Math.random() * 3400); repeat: true
        onTriggered: { blink.restart(); interval = 2600 + Math.floor(Math.random() * 3400) }
    }
    SequentialAnimation {
        id: blink
        ScriptAction { script: { if (beanEyes.itemAt(0)) { beanEyes.itemAt(0).blinkScale = 0.1; beanEyes.itemAt(1).blinkScale = 0.1 } } }
        PauseAnimation { duration: 100 }
        ScriptAction { script: { if (beanEyes.itemAt(0)) { beanEyes.itemAt(0).blinkScale = 1.0; beanEyes.itemAt(1).blinkScale = 1.0 } } }
    }
    Timer {   // pupil drift
        running: root.visible; interval: 1500; repeat: true
        onTriggered: root._pupilX = (root.mode === "thinking") ? bean.width * 0.06 : (Math.random() - 0.5) * bean.width * 0.08
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
