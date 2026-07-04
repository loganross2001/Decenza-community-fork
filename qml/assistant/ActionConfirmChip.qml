import QtQuick
import QtQuick.Layouts
import Decenza

// [barista-fork] Compact confirm chip shown under the assistant's message. Two modes:
//  • apply mode  — summarises a structuredNext recommendation (dose/temp/yield/grind) with Apply/Skip.
//  • grind mode  — "Set grinder to X?" with Yes/No (the off-machine pending reminder).
// Emits applied()/skipped(); the overlay does the actual Barista.actions call.
Rectangle {
    id: chip

    property var next: ({})            // structuredNext map (apply mode)
    property bool grindMode: false     // true → grinder confirm (Yes/No)
    property string grindValue: ""

    signal applied()
    signal skipped()

    readonly property bool _hasAny: next && (Number(next.temperatureC) > 0 || Number(next.doseG) > 0
                                    || Number(next.targetWeightG) > 0
                                    || (next.grinderSetting && String(next.grinderSetting).length > 0))
    visible: grindMode || _hasAny

    readonly property string _summary: {
        if (grindMode)
            return TranslationManager.translate("barista.act.grindQ", "Set grinder to %1?").arg(grindValue)
        var parts = []
        if (next && Number(next.temperatureC) > 0)
            parts.push(Number(next.temperatureC).toFixed(1) + "°C")
        if (next && Number(next.doseG) > 0)
            parts.push(Number(next.doseG).toFixed(1) + "g " + TranslationManager.translate("barista.act.dose", "dose"))
        if (next && Number(next.targetWeightG) > 0)
            parts.push(Number(next.targetWeightG).toFixed(1) + "g " + TranslationManager.translate("barista.act.yield", "yield"))
        if (next && next.grinderSetting && String(next.grinderSetting).length > 0)
            parts.push(TranslationManager.translate("barista.act.grind", "grind") + " " + next.grinderSetting)
        return TranslationManager.translate("barista.act.applyLabel", "Apply:") + " " + parts.join(", ")
    }

    implicitHeight: row.implicitHeight + Theme.spacingMedium
    radius: Theme.cardRadius
    color: Theme.surfaceColor
    border.width: 1
    border.color: Theme.borderColor

    RowLayout {
        id: row
        anchors.fill: parent
        anchors.margins: Theme.spacingSmall
        spacing: Theme.spacingSmall

        Text {
            Layout.fillWidth: true
            text: chip._summary
            wrapMode: Text.WordWrap
            color: Theme.textColor
            font: Theme.labelFont
            Accessible.ignored: true
        }
        AccessibleButton {
            text: chip.grindMode ? TranslationManager.translate("common.button.yes", "Yes")
                                 : TranslationManager.translate("barista.act.applyBtn", "Apply")
            accessibleName: text
            onClicked: chip.applied()
        }
        AccessibleButton {
            subtle: true
            text: chip.grindMode ? TranslationManager.translate("common.button.no", "No")
                                 : TranslationManager.translate("barista.act.skip", "Skip")
            accessibleName: text
            onClicked: chip.skipped()
        }
    }
}
