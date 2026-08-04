import QtQuick
import QtQuick.Layouts
import Decenza

// Layout widget: measured dose weight (composable-brew-bar).
// Shows Settings.dye.dyeBeanWeight; "—" when no dose recorded. While the idle
// page's bean auto-capture is weighing a dose it shows the engine's live
// virtual-zero net instead (Theme.doseLiveNetG, -1 when not weighing) in the
// secondary color, and flashes the accent color at capture —
// driven by the same capture engine and flash timing as the espresso panel's
// readout (clamped at 0; reverts to the recorded dose once captured, where the
// panel keeps ticking live).
LayoutWidgetItem {
    id: root

    // Per-instance display mode ("text" default | "icon": beans icon in place
    // of the label) and color override. See WidgetColor for the shared palette.
    readonly property string displayMode: (modelData && modelData.displayMode) ? modelData.displayMode : "text"
    readonly property string colorChoice: (modelData && modelData.color) ? modelData.color : "default"

    readonly property string labelText: TranslationManager.translate("idle.status.beans", "Beans")
    // Live dose state published on Theme by IdlePage's beanCapture engine; -1 = not
    // weighing, so the widget falls back to the recorded dose. This used to be read off
    // the window root through `Window.window` — a QQuickWindow, so neither side of the
    // channel was checked and a one-sided rename could only be caught by a runtime
    // warn-once probe, which is what stood here.
    readonly property real liveNetG: Theme.doseLiveNetG
    readonly property bool captureFlash: Theme.doseCaptureFlash
    readonly property bool isLive: liveNetG >= 0
    readonly property string valueText: root.isLive
                                        ? root.liveNetG.toFixed(1) + " g"
                                        : Settings.dye.dyeBeanWeight > 0
                                          ? Settings.dye.dyeBeanWeight.toFixed(1) + " g"
                                          : "—"

    implicitWidth: root.isCompact ? compactContent.implicitWidth : fullContent.implicitWidth
    implicitHeight: root.isCompact ? compactContent.implicitHeight : fullContent.implicitHeight

    // Value tint: a named override is static; otherwise secondary while a live
    // (unsettled) weight shows, accent flash at capture, else the zone color.
    // Shared by the compact and full renderings.
    readonly property color valueColor: WidgetColor.resolve(root.colorChoice,
                                             root.captureFlash ? Theme.primaryColor
                                           : root.isLive ? Theme.textSecondaryColor
                                           : root.zoneTextColor)

    Accessible.role: Accessible.StaticText
    Accessible.name: root.labelText + ": " + root.valueText
    Accessible.focusable: true

    // --- COMPACT MODE (bar zones) ---
    // A single icon+value row so the number fits a one-row bar, the way
    // ScaleWeightItem renders compact. The stacked label+value column below is
    // two rows tall; a compact bar squeezes the value row out of it, which is
    // why the number went missing on the brew bar. The beans icon identifies it
    // as the dose (there is no room for the text label here).
    Item {
        id: compactContent
        visible: root.isCompact
        anchors.fill: parent
        implicitWidth: compactRow.implicitWidth
        implicitHeight: compactRow.implicitHeight

        Row {
            id: compactRow
            anchors.centerIn: parent
            spacing: Theme.spacingSmall

            ThemedIcon {
                anchors.verticalCenter: parent.verticalCenter
                source: "qrc:/icons/coffeebeans.svg"
                iconSize: Theme.scaled(18)
                color: WidgetColor.resolve(root.colorChoice, root.zoneTextColor)
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.valueText
                color: root.valueColor
                // Bar readouts render bold (the sibling widgets use Theme.valueFont,
                // which is bold); the zone only sets zoneValueBold on accentBar
                // zones, so key the compact value bold directly to match the bar
                // regardless of zone style. Size matches the full rendering.
                font.pixelSize: Theme.scaled(21)
                font.bold: true
            }
        }
    }

    // --- FULL MODE (center zones) ---
    Item {
        id: fullContent
        visible: !root.isCompact
        anchors.fill: parent
        implicitWidth: col.implicitWidth
        implicitHeight: col.implicitHeight

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
                source: "qrc:/icons/coffeebeans.svg"
                iconSize: Theme.scaled(20)
                color: WidgetColor.resolve(root.colorChoice, root.zoneTextColor)
            }
            Text {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                text: root.valueText
                color: root.valueColor
                font.pixelSize: Theme.scaled(21)
                font.bold: root.zoneValueBold
            }
        }
    }
}
