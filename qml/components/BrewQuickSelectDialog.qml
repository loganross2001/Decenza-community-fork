import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// Unified brew quick-select dialog, opened from the brewQuickSelect layout
// widget. Three stepper rows — Ratio, Temperature, Grind — let the user pick all
// three in one place. Values are STAGED (initialized from the current settings
// on open) and applied together on Confirm, so a partial edit never leaks out.
//
// First cut: compact stepper-per-row (± buttons) rather than scrolling wheels —
// simpler and robust, and it reuses the exact write paths of the individual
// pills/dialogs. Each write matches its single-purpose counterpart:
//   - Ratio: lastUsedRatio + brewYieldOverride (yield = dose × ratio)
//   - Temp:  temperatureOverride (plain property write — setter not Q_INVOKABLE)
//   - Grind: dyeGrinderSetting (string stepping: numeric / number-in-text / letters)
//
// Pure UI: no barista / AI dependencies.
Dialog {
    id: root
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(Theme.scaled(460), parent ? parent.width * 0.95 : Theme.scaled(460))
    height: Math.min(contentCol.implicitHeight + Theme.scaled(40), parent ? parent.height * 0.92 : Theme.scaled(640))
    modal: true
    closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
    padding: 0

    // --- Steps (global preferences, shared with the individual pills) ---
    readonly property double tempStepC: (Settings.brew.temperatureQuickSelectStep > 0)
        ? Settings.brew.temperatureQuickSelectStep : 0.5
    // [barista-fork] Upstream #1540 retired the global grindQuickSelectStep for a HISTORY-derived step. Mirror
    // GrindQuickSelectItem: derive from the full cross-grinder history (grindStepForGrinder("")), fall back to 1.0.
    readonly property double grindStep: {
        var s = (MainController.shotHistory && MainController.shotHistory.grindStepForGrinder)
            ? MainController.shotHistory.grindStepForGrinder("") : 0
        return s > 0 ? s : 1.0
    }
    readonly property double ratioStep: 0.1   // ratio picks are in 0.1 increments

    // --- Live current values (staged from these on open) ---
    readonly property double _dose: ProfileManager.brewByRatioDose > 0 ? ProfileManager.brewByRatioDose : 18.0
    readonly property double currentRatio: ProfileManager.targetWeight / _dose
    readonly property double currentTempC: Settings.brew.hasTemperatureOverride
        ? Settings.brew.temperatureOverride
        : ProfileManager.profileTargetTemperature
    readonly property string currentGrind: String(Settings.dye.dyeGrinderSetting || "")

    // --- Staged values (edited by the steppers, written on Confirm) ---
    property double pendingRatio: 2.0
    property double pendingTempC: 93.0
    property string pendingGrind: ""

    function reset() {
        pendingRatio = Math.max(0.5, Math.min(5.0, Math.round(root.currentRatio * 10) / 10))
        pendingTempC = root.currentTempC
        pendingGrind = root.currentGrind
    }
    onAboutToShow: reset()

    // --- Grind string stepping (numeric / number-in-text / pure letters),
    //     mirroring GrindQuickSelectItem.stepGrind. Returns "" to skip. ---
    function _stepDecimals(step) {
        var s = Number(step).toFixed(3).replace(/0+$/, "").replace(/\.$/, "")
        var dot = s.indexOf(".")
        return dot < 0 ? 0 : (s.length - dot - 1)
    }
    function stepGrind(currentString, n, step) {
        var s = String(currentString == null ? "" : currentString).trim()
        if (s.length === 0)
            return ""
        if (/^-?\d+(\.\d+)?$/.test(s)) {
            var v = parseFloat(s) + n * step
            if (v < 0) return ""
            return Number(v).toFixed(_stepDecimals(step))
        }
        var m = s.match(/^(\D*)(\d+(?:\.\d+)?)(\D*)$/)
        if (m) {
            var nv = parseFloat(m[2]) + n * step
            if (nv < 0) nv = 0
            return m[1] + Number(nv).toFixed(_stepDecimals(step)) + m[3]
        }
        if (/^[A-Za-z]{1,3}$/.test(s)) {
            var last = s.charAt(s.length - 1)
            var isUpper = last === last.toUpperCase()
            var base = isUpper ? 65 : 97
            var ord = last.charCodeAt(0) - base + n
            if (ord < 0) ord = 0
            if (ord > 25) ord = 25
            return s.substring(0, s.length - 1) + String.fromCharCode(base + ord)
        }
        return ""   // unparseable: grind row disables its steppers
    }
    readonly property bool grindStepsUsable: stepGrind(root.pendingGrind, 1, root.grindStep) !== ""
                                             || stepGrind(root.pendingGrind, -1, root.grindStep) !== ""

    function applyAll() {
        // Ratio: arm a RATIO ANCHOR (yield tracks dose x ratio live), matching RatioPresetDialog.applyRatio —
        // record lastUsedRatio (preset memory) then setBrewRatioAnchor (the session anchor). The anchor derives
        // the target from the live dose; nothing here flattens it to a stale absolute.
        Settings.brew.lastUsedRatio = root.pendingRatio
        Settings.brew.setBrewRatioAnchor(root.pendingRatio)
        // Temp: plain property write (setter is NOT Q_INVOKABLE).
        Settings.brew.temperatureOverride = root.pendingTempC
        // Grind: only write a real, non-empty value.
        if (root.pendingGrind && root.pendingGrind.length > 0)
            Settings.dye.dyeGrinderSetting = root.pendingGrind
        root.close()
    }

    background: Rectangle {
        color: Theme.surfaceColor
        radius: Theme.cardRadius
        border.width: 1
        border.color: Theme.borderColor
    }

    // Reusable stepper row: label + [–] value [+], with accessible buttons.
    component StepperRow: RowLayout {
        id: srow
        property string rowLabel: ""
        property string valueLabel: ""
        property bool minusEnabled: true
        property bool plusEnabled: true
        signal decrement()
        signal increment()

        Layout.fillWidth: true
        Layout.leftMargin: Theme.spacingLarge
        Layout.rightMargin: Theme.spacingLarge
        spacing: Theme.spacingMedium

        Text {
            Layout.preferredWidth: Theme.scaled(90)
            text: srow.rowLabel
            color: Theme.textColor
            font.pixelSize: Theme.scaled(17); font.bold: true
            Accessible.ignored: true
        }

        Rectangle {
            Layout.preferredWidth: Theme.scaled(46)
            Layout.preferredHeight: Theme.scaled(46)
            radius: Theme.buttonRadius
            opacity: srow.minusEnabled ? 1.0 : 0.4
            color: minusMa.pressed ? Qt.darker(Theme.primaryColor, 1.15) : Theme.primaryColor
            Accessible.role: Accessible.Button
            Accessible.name: TranslationManager.translate("brewSelect.decrease", "Decrease %1").arg(srow.rowLabel)
            Accessible.focusable: srow.minusEnabled
            Accessible.onPressAction: if (srow.minusEnabled) srow.decrement()
            Text { anchors.centerIn: parent; text: "–"; color: Theme.primaryContrastColor
                   font.pixelSize: Theme.scaled(24); font.bold: true }
            MouseArea { id: minusMa; anchors.fill: parent; enabled: srow.minusEnabled
                onClicked: srow.decrement() }
        }

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            text: srow.valueLabel
            color: Theme.textColor
            font.pixelSize: Theme.scaled(20); font.bold: true
            Accessible.role: Accessible.StaticText
            Accessible.name: srow.rowLabel + " " + srow.valueLabel
        }

        Rectangle {
            Layout.preferredWidth: Theme.scaled(46)
            Layout.preferredHeight: Theme.scaled(46)
            radius: Theme.buttonRadius
            opacity: srow.plusEnabled ? 1.0 : 0.4
            color: plusMa.pressed ? Qt.darker(Theme.primaryColor, 1.15) : Theme.primaryColor
            Accessible.role: Accessible.Button
            Accessible.name: TranslationManager.translate("brewSelect.increase", "Increase %1").arg(srow.rowLabel)
            Accessible.focusable: srow.plusEnabled
            Accessible.onPressAction: if (srow.plusEnabled) srow.increment()
            Text { anchors.centerIn: parent; text: "+"; color: Theme.primaryContrastColor
                   font.pixelSize: Theme.scaled(24); font.bold: true }
            MouseArea { id: plusMa; anchors.fill: parent; enabled: srow.plusEnabled
                onClicked: srow.increment() }
        }
    }

    contentItem: Flickable {
        contentWidth: width
        contentHeight: contentCol.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        ColumnLayout {
            id: contentCol
            width: parent.width
            spacing: Theme.spacingMedium

            // --- Header ---
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingLarge
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                spacing: Theme.scaled(2)
                Text {
                    text: TranslationManager.translate("brewSelect.dialog.title", "Ratio · Temp · Grind")
                    color: Theme.textColor
                    font.pixelSize: Theme.scaled(24); font.bold: true
                }
                Text {
                    text: TranslationManager.translate("brewSelect.dialog.subtitle", "Adjust each, then Confirm to apply")
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                }
            }

            // --- Ratio row (0.1 increments, clamped 0.5–5.0) ---
            StepperRow {
                rowLabel: TranslationManager.translate("idle.status.ratio", "Ratio")
                valueLabel: "1:" + root.pendingRatio.toFixed(1)
                minusEnabled: root.pendingRatio > 0.5 + 0.001
                plusEnabled: root.pendingRatio < 5.0 - 0.001
                onDecrement: root.pendingRatio = Math.max(0.5, Math.round((root.pendingRatio - root.ratioStep) * 10) / 10)
                onIncrement: root.pendingRatio = Math.min(5.0, Math.round((root.pendingRatio + root.ratioStep) * 10) / 10)
            }

            // --- Temperature row (step °C, clamped 70–100 °C, shown in user unit) ---
            StepperRow {
                rowLabel: TranslationManager.translate("temp.quickSelect.label", "Temp")
                valueLabel: Theme.formatTemperature(root.pendingTempC, 1)
                minusEnabled: root.pendingTempC > 70 + 0.001
                plusEnabled: root.pendingTempC < 100 - 0.001
                onDecrement: root.pendingTempC = Math.max(70, root.pendingTempC - root.tempStepC)
                onIncrement: root.pendingTempC = Math.min(100, root.pendingTempC + root.tempStepC)
            }

            // --- Grind row (string stepping; disabled if unparseable/empty) ---
            StepperRow {
                rowLabel: TranslationManager.translate("grind.quickSelect.label", "Grind")
                valueLabel: root.pendingGrind.length > 0
                    ? root.pendingGrind
                    : TranslationManager.translate("grind.quickSelect.unset", "—")
                minusEnabled: root.grindStepsUsable
                              && root.stepGrind(root.pendingGrind, -1, root.grindStep) !== ""
                plusEnabled: root.grindStepsUsable
                             && root.stepGrind(root.pendingGrind, 1, root.grindStep) !== ""
                onDecrement: {
                    var v = root.stepGrind(root.pendingGrind, -1, root.grindStep)
                    if (v !== "") root.pendingGrind = v
                }
                onIncrement: {
                    var v = root.stepGrind(root.pendingGrind, 1, root.grindStep)
                    if (v !== "") root.pendingGrind = v
                }
            }

            // --- Note when grind can't be stepped (letters exhausted / unparseable) ---
            Text {
                visible: !root.grindStepsUsable
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                text: TranslationManager.translate("brewSelect.grindUnsteppable",
                    "This grinder's setting can't be stepped here — use the Grind pill.")
                color: Theme.textSecondaryColor
                font: Theme.captionFont
                wrapMode: Text.WordWrap
            }

            // --- Buttons: Cancel + Confirm ---
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                Layout.topMargin: Theme.spacingSmall
                Layout.bottomMargin: Theme.spacingLarge
                spacing: Theme.spacingMedium

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.scaled(48)
                    radius: Theme.buttonRadius
                    color: cancelMa.pressed ? Qt.darker(Theme.backgroundColor, 1.1) : "transparent"
                    border.width: 1
                    border.color: Theme.borderColor
                    Accessible.role: Accessible.Button
                    Accessible.name: TranslationManager.translate("common.button.cancel", "Cancel")
                    Accessible.focusable: true
                    Accessible.onPressAction: cancelMa.clicked(null)
                    Text {
                        anchors.centerIn: parent
                        text: TranslationManager.translate("common.button.cancel", "Cancel")
                        color: Theme.primaryColor
                        font: Theme.bodyFont
                    }
                    MouseArea { id: cancelMa; anchors.fill: parent; onClicked: root.close() }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Theme.scaled(48)
                    radius: Theme.buttonRadius
                    color: confirmMa.pressed ? Qt.darker(Theme.primaryColor, 1.15) : Theme.primaryColor
                    Accessible.role: Accessible.Button
                    Accessible.name: TranslationManager.translate("common.button.confirm", "Confirm")
                    Accessible.focusable: true
                    Accessible.onPressAction: confirmMa.clicked(null)
                    Text {
                        anchors.centerIn: parent
                        text: TranslationManager.translate("common.button.confirm", "Confirm")
                        color: Theme.primaryContrastColor
                        font: Theme.bodyFont
                    }
                    MouseArea { id: confirmMa; anchors.fill: parent; onClicked: root.applyAll() }
                }
            }
        }
    }
}
