import QtQuick
import QtQuick.Layouts
import Decenza
import "../.."

// Layout widget: unified brew quick-select (composable-brew-bar).
// A single pill that opens a compact picker letting the user scroll and pick
// RATIO, TEMPERATURE and GRIND in one place. Each row writes the SAME underlying
// settings the individual pills/dialogs write:
//   - Ratio: Settings.brew.lastUsedRatio + Settings.brew.brewYieldOverride
//            (yield = dose × ratio), matching RatioPresetDialog.applyRatio.
//   - Temp:  Settings.brew.temperatureOverride (plain property write — the
//            setter is NOT Q_INVOKABLE), matching TempQuickSelectItem.
//   - Grind: Settings.dye.dyeGrinderSetting, matching GrindQuickSelectItem.
// Values are staged in the dialog and applied together on Confirm.
//
// Pure layout widget: no barista / AI / feedback dependencies.
// [fork-index] pill=brewQuickSelect | domain=recipe | change=-
//   what: brew-bar pill — brew quick-select (dose/grind/temp/yield) via BrewQuickSelectDialog
LayoutWidgetItem {
    id: root

    readonly property string labelText: TranslationManager.translate("brewSelect.label", "Brew")

    // Live current values for the pill preview (same sources the individual pills use).
    readonly property double _dose: ProfileManager.brewByRatioDose > 0 ? ProfileManager.brewByRatioDose : 18.0
    readonly property double activeRatio: ProfileManager.targetWeight / _dose
    readonly property double effectiveTempC: Settings.brew.hasTemperatureOverride
        ? Settings.brew.temperatureOverride
        : ProfileManager.profileTargetTemperature

    readonly property string currentGrind: String(Settings.dye.dyeGrinderSetting || "")

    // Compact pill summary: "1:2.0 · 93.0°C · 4.5" (grind omitted when unset).
    readonly property string valueText: {
        var _ = Settings.app.temperatureUnit
        var s = "1:" + activeRatio.toFixed(1)
              + " · " + Theme.formatTemperature(effectiveTempC, 1)
        if (currentGrind.length > 0)
            s += " · " + currentGrind
        return s
    }

    implicitWidth: col.implicitWidth
    implicitHeight: col.implicitHeight

    ColumnLayout {
        id: col
        anchors.centerIn: parent
        width: parent.width
        spacing: Theme.scaled(2)

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.labelText
            color: root.zoneTextColor
            font: Theme.labelFont
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: brewValue.implicitWidth + Theme.spacingMedium * 2
            Layout.preferredHeight: Theme.scaled(32)
            radius: height / 2
            color: brewMa.pressed ? Qt.darker(root.zoneTextColor, 1.15) : root.zoneTextColor

            Accessible.role: Accessible.Button
            Accessible.name: root.labelText + " " + root.valueText + ". "
                             + TranslationManager.translate("brewSelect.tapToChange", "Tap to change ratio, temperature and grind")
            Accessible.focusable: true
            Accessible.onPressAction: brewMa.clicked(null)

            Text {
                id: brewValue
                anchors.centerIn: parent
                text: root.valueText
                color: Theme.primaryColor
                font.pixelSize: Theme.scaled(18)
                font.bold: true
            }
            MouseArea { id: brewMa; anchors.fill: parent; onClicked: brewDialog.open() }
        }
    }

    BrewQuickSelectDialog { id: brewDialog }
}
