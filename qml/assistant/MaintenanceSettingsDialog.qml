import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] Maintenance schedule editor for the Decent DE1. Lists each recurring maintenance
// task with its interval, enabled state, and last-done date; lets the owner EDIT the interval,
// enable/disable, and mark a task done. The owner's edit becomes their OVERRIDE (persisted; the
// seeded default is only the fallback).
//
// The seeded intervals follow Decent's official DE1 Quickstart cleaning guide
// (decentespresso.com/doc/quickstart/); each row still on its seed is tagged "default". They are
// Decent's general guidance — the owner adjusts for their own usage, and MUST set the descale
// interval for their water (it's TDS-dependent; the banner spells out the thresholds).
//
// Reachability: opened from the "Maintenance & reminders" button at the bottom of
// AssistantSettingsPanel (itself reached via the gear in the conversation-card header). Writes go
// through Barista.tasks (TasksStorage); the list refreshes on the storage confirmation signals.
Dialog {
    id: root
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(Theme.scaled(560), parent ? parent.width * 0.95 : Theme.scaled(560))
    height: Math.min(Theme.scaled(640), parent ? parent.height * 0.9 : Theme.scaled(640))
    modal: true
    closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
    padding: 0

    readonly property var _tasks: (typeof Barista !== "undefined") ? Barista.tasks : null

    // The live task list (array of maps: taskKey, label, intervalDays, enabled, lastDoneAt, isDefault, note).
    property var _rows: []

    function refresh() {
        if (root._tasks)
            root._tasks.requestMaintenanceTasks()
    }

    function _lastDoneText(secs) {
        if (!secs || secs <= 0)
            return TranslationManager.translate("barista.maint.neverDone", "never done")
        var d = new Date(secs * 1000)
        var pad = function(n) { return (n < 10 ? "0" : "") + n }
        return TranslationManager.translate("barista.maint.lastDone", "last done")
               + " " + d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate())
    }

    onOpened: refresh()

    Connections {
        target: root._tasks
        ignoreUnknownSignals: true
        function onMaintenanceTasksReady(rows) { root._rows = rows }
        // Any edit / mark-done re-reads the list so the UI reflects the persisted state (last-done date,
        // the "default" tag clearing once an interval is edited).
        function onMaintenanceTaskUpdated(taskKey) { root.refresh() }
        function onMaintenanceLogged(taskKey) { root.refresh() }
    }

    background: Rectangle {
        color: Theme.surfaceColor
        radius: Theme.cardRadius
        border.width: 1
        border.color: Theme.borderColor
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        // --- Title + close ---
        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingLarge
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            Text {
                Layout.fillWidth: true
                text: TranslationManager.translate("barista.maint.title", "Maintenance schedule")
                color: Theme.textColor
                font.pixelSize: Theme.scaled(22)
                font.bold: true
                Accessible.ignored: true
            }
            AccessibleButton {
                subtle: true
                text: "×"
                accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                onClicked: root.close()
            }
        }

        // --- No-fabrication banner (hard requirement) ---
        Rectangle {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            radius: Theme.cardRadius
            color: Theme.backgroundColor
            border.width: 1
            border.color: Theme.warningColor
            implicitHeight: bannerText.implicitHeight + Theme.spacingMedium * 2
            Text {
                id: bannerText
                anchors.fill: parent
                anchors.margins: Theme.spacingMedium
                wrapMode: Text.WordWrap
                text: TranslationManager.translate("barista.maint.defaultsBanner",
                    "These default intervals follow Decent's DE1 Quickstart cleaning guide. Adjust any of them "
                    + "for your own usage — and especially set DESCALE for your water (none under ~30ppm TDS, "
                    + "~yearly at 30-120ppm, every 4-8 weeks if harder). Your edits override the defaults.")
                color: Theme.textSecondaryColor
                font: Theme.labelFont
            }
        }

        // --- Task list ---
        ScrollView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            clip: true

            ColumnLayout {
                width: root.width - Theme.spacingLarge * 2
                spacing: Theme.spacingSmall

                Text {
                    visible: root._rows.length === 0
                    Layout.fillWidth: true
                    text: TranslationManager.translate("barista.maint.empty", "No maintenance tasks yet.")
                    color: Theme.textSecondaryColor
                    font: Theme.bodyFont
                    Accessible.ignored: true
                }

                Repeater {
                    model: root._rows
                    delegate: Rectangle {
                        id: taskRow
                        required property var modelData
                        readonly property string taskKey: modelData.taskKey || ""
                        readonly property string label: modelData.label || taskKey
                        readonly property int intervalDays: modelData.intervalDays || 0
                        readonly property bool taskEnabled: modelData.enabled === true
                        readonly property double lastDoneAt: modelData.lastDoneAt || 0
                        readonly property bool isDefault: modelData.isDefault === true
                        readonly property string note: modelData.note || ""

                        Layout.fillWidth: true
                        implicitHeight: rowCol.implicitHeight + Theme.spacingMedium * 2
                        radius: Theme.cardRadius
                        color: Theme.backgroundColor
                        border.width: 1
                        border.color: Theme.borderColor
                        opacity: taskEnabled ? 1.0 : 0.6

                        ColumnLayout {
                            id: rowCol
                            anchors.fill: parent
                            anchors.margins: Theme.spacingMedium
                            spacing: Theme.spacingSmall

                            // Label + enable toggle
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacingSmall
                                Text {
                                    Layout.fillWidth: true
                                    text: taskRow.label
                                    color: Theme.textColor
                                    font: Theme.bodyFont
                                    wrapMode: Text.WordWrap
                                    Accessible.ignored: true
                                }
                                // "default" tag — this row is still on its seeded (unconfirmed) default interval.
                                Rectangle {
                                    visible: taskRow.isDefault
                                    radius: Theme.scaled(4)
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Theme.warningColor
                                    implicitWidth: defaultTag.implicitWidth + Theme.spacingSmall * 2
                                    implicitHeight: defaultTag.implicitHeight + Theme.spacingSmall
                                    Text {
                                        id: defaultTag
                                        anchors.centerIn: parent
                                        text: TranslationManager.translate("barista.maint.defaultTag", "default")
                                        color: Theme.warningColor
                                        font: Theme.labelFont
                                        Accessible.ignored: true
                                    }
                                }
                                Switch {
                                    id: enableSwitch
                                    checked: taskRow.taskEnabled
                                    onToggled: if (root._tasks)
                                        root._tasks.requestUpdateMaintenanceTask({ "taskKey": taskRow.taskKey,
                                                                                   "enabled": checked })
                                    Accessible.role: Accessible.CheckBox
                                    Accessible.name: taskRow.label + " "
                                        + TranslationManager.translate("barista.maint.enabled", "enabled")
                                    Accessible.checked: checked
                                    Accessible.focusable: true
                                    Accessible.onToggleAction: toggle()
                                }
                            }

                            // Optional per-task note (the "confirm against Decent" hint carried in the seed).
                            Text {
                                visible: taskRow.note.length > 0
                                Layout.fillWidth: true
                                text: taskRow.note
                                color: Theme.textSecondaryColor
                                font: Theme.labelFont
                                wrapMode: Text.WordWrap
                                Accessible.ignored: true
                            }

                            // Interval editor + last-done + mark-done
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacingSmall

                                Text {
                                    text: TranslationManager.translate("barista.maint.everyLabel", "Every")
                                    color: Theme.textSecondaryColor
                                    font: Theme.labelFont
                                    Accessible.ignored: true
                                }
                                StyledTextField {
                                    id: intervalField
                                    Layout.preferredWidth: Theme.scaled(70)
                                    inputMethodHints: Qt.ImhDigitsOnly
                                    validator: IntValidator { bottom: 0; top: 3650 }
                                    Component.onCompleted: text = taskRow.intervalDays > 0
                                                                  ? String(taskRow.intervalDays) : ""
                                    accessibleName: taskRow.label + " "
                                        + TranslationManager.translate("barista.maint.intervalDaysLabel", "interval in days")
                                    onEditingFinished: {
                                        Qt.inputMethod.commit()
                                        if (!root._tasks)
                                            return
                                        var v = parseInt(text, 10)
                                        if (isNaN(v) || v < 0)
                                            v = 0
                                        if (v !== taskRow.intervalDays)
                                            root._tasks.requestUpdateMaintenanceTask({ "taskKey": taskRow.taskKey,
                                                                                       "intervalDays": v })
                                    }
                                }
                                Text {
                                    text: TranslationManager.translate("barista.maint.daysUnit", "days")
                                    color: Theme.textSecondaryColor
                                    font: Theme.labelFont
                                    Accessible.ignored: true
                                }

                                Item { Layout.fillWidth: true }

                                Text {
                                    text: root._lastDoneText(taskRow.lastDoneAt)
                                    color: Theme.textSecondaryColor
                                    font: Theme.labelFont
                                    Accessible.ignored: true
                                }
                                AccessibleButton {
                                    subtle: true
                                    text: TranslationManager.translate("barista.maint.markDone", "Mark done")
                                    accessibleName: TranslationManager.translate("barista.maint.markDone", "Mark done")
                                        + ": " + taskRow.label
                                    onClicked: if (root._tasks)
                                        root._tasks.requestLogMaintenance(taskRow.taskKey, 0)
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Close ---
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            Layout.bottomMargin: Theme.spacingLarge
            Item { Layout.fillWidth: true }
            AccessibleButton {
                primary: true
                text: TranslationManager.translate("common.button.done", "Done")
                accessibleName: TranslationManager.translate("common.button.done", "Done")
                onClicked: root.close()
            }
        }
    }
}
