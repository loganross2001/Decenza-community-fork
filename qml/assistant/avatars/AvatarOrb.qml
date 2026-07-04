import QtQuick
import Decenza

// [barista-fork] Avatar style "orb" — an abstract glowing orb with rings + a waveform core that react
// to the voice. No face. mouthOpen is unused; `mode` drives everything.
Item {
    id: root
    property string mode: "idle"
    property real mouthOpen: 0
    function greet() { greetRing.restart() }

    implicitWidth: Theme.scaled(150)
    implicitHeight: Theme.scaled(150)
    Accessible.ignored: true

    readonly property bool _active: mode === "speaking" || mode === "listening"
    readonly property int _ringDur: mode === "speaking" ? 1100 : 2200

    Item {
        anchors.centerIn: parent
        width: Math.min(root.width, root.height) * 0.92
        height: width

        // continuously pulsing rings (bigger/faster when the voice is active)
        Repeater {
            model: 3
            Rectangle {
                anchors.centerIn: parent
                width: parent.width * 0.5; height: width; radius: width / 2
                color: "transparent"
                border.width: Math.max(2, root.width * 0.02); border.color: Theme.highlightColor
                property real t: 0
                scale: 1.0 + t * (root._active ? 1.4 : 0.9)
                opacity: (1.0 - t) * 0.55
                NumberAnimation on t { running: root.visible; loops: Animation.Infinite; from: 0; to: 1; duration: root._ringDur }
                Component.onCompleted: t = index / 3
            }
        }

        // one-shot greeting ring
        Rectangle {
            id: greetRingItem
            anchors.centerIn: parent
            width: parent.width * 0.5; height: width; radius: width / 2
            color: "transparent"; border.width: Math.max(2, root.width * 0.025); border.color: Theme.highlightColor
            visible: greetRing.running; opacity: 0
            NumberAnimation { id: dummy }
        }

        // glowing gradient core (soft scale-pulse when listening)
        Rectangle {
            id: core
            anchors.centerIn: parent
            width: parent.width * 0.44; height: width; radius: width / 2
            gradient: Gradient {
                GradientStop { position: 0.0; color: Qt.lighter(Theme.highlightColor, 1.25) }
                GradientStop { position: 1.0; color: Qt.darker(Theme.highlightColor, 1.25) }
            }
            SequentialAnimation on scale {
                running: root.visible && root.mode === "listening"; loops: Animation.Infinite
                NumberAnimation { from: 1.0; to: 1.08; duration: 700; easing.type: Easing.InOutSine }
                NumberAnimation { from: 1.08; to: 1.0; duration: 700; easing.type: Easing.InOutSine }
            }

            // waveform bars — animate height while speaking, flat/short otherwise
            Row {
                anchors.centerIn: parent
                spacing: core.width * 0.07
                Repeater {
                    model: 4
                    Rectangle {
                        width: core.width * 0.08; radius: width / 2; color: Theme.textColor
                        anchors.verticalCenter: parent.verticalCenter
                        property real amp: 0.3
                        height: core.height * (0.12 + amp * 0.42)
                        Behavior on height { NumberAnimation { duration: 110; easing.type: Easing.OutQuad } }
                        Timer {
                            running: root.visible && root.mode === "speaking"
                            interval: 110 + index * 25; repeat: true
                            onTriggered: parent.amp = Math.random()
                            onRunningChanged: if (!running) parent.amp = 0.3
                        }
                    }
                }
            }
        }
    }

    // greeting: one expanding ring
    SequentialAnimation {
        id: greetRing
        ScriptAction { script: greetRingItem.opacity = 0.7 }
        ParallelAnimation {
            NumberAnimation { target: greetRingItem; property: "scale"; from: 0.6; to: 2.0; duration: 650; easing.type: Easing.OutQuad }
            NumberAnimation { target: greetRingItem; property: "opacity"; from: 0.7; to: 0; duration: 650 }
        }
    }
}
