import QtQuick
import QtQuick.Layouts
import Decenza

// Layout widget: measured milk weight (composable-brew-bar).
// Shows the live in-session milk while steaming (sessionMeasuredMilkG on the
// window root) and falls back to the last committed session weight
// (Settings.brew.lastSteamMilkG); "—" when neither is available.
Item {
    id: root
    property bool isCompact: false
    property string itemId: ""
    property var modelData: ({})
    property color zoneTextColor: Theme.textColor
    property bool zoneValueBold: false

    // Per-instance display mode ("text" default | "icon": the shared steam icon
    // in place of the label) and color override. See WidgetColor for the palette.
    readonly property string displayMode: (modelData && modelData.displayMode) ? modelData.displayMode : "text"
    readonly property string colorChoice: (modelData && modelData.color) ? modelData.color : "default"

    readonly property string labelText: TranslationManager.translate("idle.status.milk", "Milk")

    property bool scaleConnected: ScaleDevice && ScaleDevice.connected

    // Pitcher tare for the selected steam preset (0 when none saved/disabled).
    function _pitcherWeight() {
        var p = Settings.brew.getSteamPitcherPreset(Settings.brew.selectedSteamPitcher)
        return (p && !p.disabled) ? (p.pitcherWeightG ?? 0) : 0
    }

    // [barista-fork] Captured/committed milk for this session — set at capture, reset to 0 at session end /
    // pitcher change. Upstream #1680 migrated sessionMeasuredMilkG off the window root onto the AppShell
    // singleton, so read it there (the old window-root read is gone).
    readonly property double sessionMilkG:
        AppShell.sessionMeasuredMilkG > 0 ? AppShell.sessionMeasuredMilkG : 0
    // Live net milk on the scale right now (pitcher tare subtracted). Needs a saved
    // pitcher weight to be meaningful; 0 otherwise.
    readonly property double liveNetMilkG: {
        var p = root._pitcherWeight()
        return (root.scaleConnected && p > 0) ? Math.max(0, MachineState.scaleWeight - p) : 0
    }
    // Show the captured value once it lands (held), else the live net milk while
    // the pitcher is on the scale, else the last committed session weight.
    readonly property double milkG: root.sessionMilkG > 0 ? root.sessionMilkG
                                    : (root.liveNetMilkG > 0.3 ? root.liveNetMilkG
                                    : Settings.brew.lastSteamMilkG)
    readonly property string valueText: root.milkG > 0 ? root.milkG.toFixed(1) + " g" : "—"

    implicitWidth: col.implicitWidth
    implicitHeight: col.implicitHeight

    Accessible.role: Accessible.StaticText
    Accessible.name: root.labelText + ": " + root.valueText
    Accessible.focusable: true

    ColumnLayout {
        id: col
        anchors.centerIn: parent
        width: parent.width
        spacing: 0
        Text {
            visible: root.displayMode !== "icon"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.labelText
            color: root.zoneTextColor
            font: Theme.labelFont
        }
        ThemedIcon {
            visible: root.displayMode === "icon"
            Layout.alignment: Qt.AlignHCenter
            source: "qrc:/icons/steam.svg"
            iconSize: Theme.scaled(20)
            color: WidgetColor.resolve(root.colorChoice, root.zoneTextColor)
        }
        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.valueText
            color: WidgetColor.resolve(root.colorChoice, root.zoneTextColor)
            font.pixelSize: Theme.scaled(21)
            font.bold: root.zoneValueBold
        }
    }
}
