import QtQuick

// [barista-fork] The barista assistant's single global overlay. Loaded by URL from main.qml
// (qrc:/qml/assistant/AssistantOverlay.qml) via a Loader gated on Barista.enabled, so the
// upstream QML module list is never touched.
//
// P0: intentionally inert — this is the mount point. P1 wires the conversation orchestrator
// (greeting card, plan card, chat sheet) in here; page-awareness lives inside via MachineState,
// never by editing the individual pages.
Item {
    id: root
    anchors.fill: parent
    visible: false
}
