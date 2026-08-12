import QtQuick
import QtQuick.Layouts
import Decenza
import "../.."

// Layout widget: brew-temperature quick-select (composable-brew-bar).
// Shows the effective brew temperature as a pill; tapping opens a value picker
// of temperatures at the current ±5 steps (11 entries, current centered).
// Tapping a value arms it as the brew-temperature override AND pushes it to the
// machine via ProfileManager.applyTemperatureOverride() — the canonical writer,
// which does a set-or-clear against the profile baseline and re-uploads the
// profile, so the same override the shipped temperature UI (TemperatureItem /
// BrewDialog) reads takes effect on the DE1 immediately (a bare override write
// only lands on the next profile upload).
//
// The step is a global preference (Settings.brew.temperatureQuickSelectStep,
// default 0.5 °C, edited in Settings -> Machine and shared with BrewQuickSelectDialog)
// — a fork-only control. (The upstream PR of this widget drops the setting and fixes
// the step at 0.5 °C.) Values are stepped in Celsius (the internal/stored unit); the
// pill and picker DISPLAY them in the user's unit via Theme.formatTemperature.
//
// Pure layout widget: no barista / AI / feedback dependencies.
LayoutWidgetItem {
    id: root

    readonly property string labelText: TranslationManager.translate("temp.quickSelect.label", "Temp")

    // The effective brew temperature (Celsius): the override when set, otherwise
    // the active profile's target temperature — the same source TemperatureItem
    // and BrewDialog use.
    readonly property double effectiveTempC: Settings.brew.hasTemperatureOverride
        ? Settings.brew.temperatureOverride
        : ProfileManager.profileTargetTemperature
    readonly property string valueText: Theme.formatTemperature(effectiveTempC, 1)

    // Global configurable step (°C); the C++ setter clamps it to 0.1–5 °C. A
    // non-positive stored value falls back to 0.5.
    readonly property real tempStepC: (Settings.brew.temperatureQuickSelectStep > 0)
        ? Settings.brew.temperatureQuickSelectStep : 0.5

    implicitWidth: col.implicitWidth
    implicitHeight: col.implicitHeight

    // Up to 11 rows for n = -5..+5 around the current temperature, clamped to the
    // brew range (70–100 °C, the same window the BrewDialog override editor uses)
    // and de-duplicated. If the current temperature is itself out of range (a tea
    // or calibration profile) no row is isCurrent and the ladder may be empty —
    // canQuickSelect gates the pill on that. Each row:
    //   { value: <Celsius double>, label: <display string>, isCurrent: bool }.
    readonly property var rows: {
        // Reference the unit so the labels re-evaluate on a unit change
        // (Theme.formatTemperature reads it in JS).
        var __ = Settings.app.temperatureUnit
        var cur = root.effectiveTempC
        var step = root.tempStepC

        var out = []
        var seen = ({})
        for (var n = -5; n <= 5; n++) {
            var v = cur + n * step
            if (!(v >= 70 && v <= 100)) continue   // clamp to the brew range (NaN-safe)
            // Round to 2 decimals to fold float dirt and de-duplicate.
            var key = v.toFixed(2)
            if (seen[key]) continue
            seen[key] = true
            out.push({ value: v, label: Theme.formatTemperature(v, 1), isCurrent: n === 0 })
        }
        return out
    }
    // The quick-select only makes sense when the current temperature is in range,
    // i.e. the ladder contains it. Otherwise the pill is a plain read-out so it
    // never opens an empty/currentless picker. Both guards key off rows so they
    // can't drift.
    readonly property bool canQuickSelect: root.rows.some(function(r) { return r.isCurrent })

    function applyValueC(v) {
        // The canonical writer: set-or-clear the override against the profile
        // baseline (tapping the profile's own temperature disarms it) AND push it
        // to the machine. A bare Settings.brew.temperatureOverride write would not
        // reach the DE1 until the next profile upload.
        ProfileManager.applyTemperatureOverride(v)
    }

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
            // Brew-bar symmetry: never narrower than a "1:X.X" Ratio Quick-Select pill (same font +
            // padding formula), so the Temp, Grind and Ratio pills line up at equal width side by side.
            Layout.preferredWidth: Math.max(tempValue.implicitWidth, tempPillRef.implicitWidth)
                                   + Theme.spacingMedium * 2
            Layout.preferredHeight: Theme.scaled(32)
            radius: height / 2
            color: (tempMa.pressed && root.canQuickSelect) ? Qt.darker(root.zoneTextColor, 1.15) : root.zoneTextColor
            // Dim to a plain read-out when the current temperature is out of range.
            opacity: root.canQuickSelect ? 1.0 : 0.55

            Accessible.role: root.canQuickSelect ? Accessible.Button : Accessible.StaticText
            Accessible.name: root.canQuickSelect
                ? root.labelText + " " + root.valueText + ". "
                  + TranslationManager.translate("temp.quickSelect.tapToChange", "Tap to change")
                : root.labelText + " " + root.valueText
            Accessible.focusable: true
            Accessible.onPressAction: { if (root.canQuickSelect) tempMa.clicked(null) }

            // Hidden width reference: a representative ratio value in the identical font, measured only
            // (never drawn), so the temp pill is at least as wide as a ratio pill for brew-bar parity.
            Text {
                id: tempPillRef
                visible: false
                text: "1:2.0"
                font.pixelSize: Theme.scaled(20)
                font.bold: true
            }
            Text {
                id: tempValue
                anchors.centerIn: parent
                text: root.valueText
                Accessible.ignored: true   // the pill Rectangle carries Accessible.name
                color: Theme.primaryColor
                font.pixelSize: Theme.scaled(20)
                font.bold: true
            }
            MouseArea {
                id: tempMa
                anchors.fill: parent
                enabled: root.canQuickSelect   // inert read-out when the temp is out of range
                onClicked: tempDialog.open()
            }
        }
    }

    TempPickerDialog {
        id: tempDialog
        rows: root.rows
        onValuePicked: function(v) { root.applyValueC(v) }
    }
}
