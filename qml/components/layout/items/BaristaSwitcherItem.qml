import QtQuick
import Decenza

// Layout widget: the barista roster switcher (avatar chips + "+"), placeable in
// any zone via the layout editor. It replaces the old hardcoded top-of-idle row.
//
// BaristaChipRow.alwaysShow keeps it rendered even for 0/1-person rosters — a
// widget the user deliberately placed should never silently vanish. Its size is
// bound directly to the chip row's implicit size (not gated on visibility), so
// the layout cell never collapses to zero and then fails to re-expand.
//
// isCompact (true in bar zones) drives the chip row's compact layout: the name
// sits beside the avatar in a short row that fits the bar height, instead of the
// taller avatar-over-name stack used in center zones.
LayoutWidgetItem {
    id: root

    implicitWidth: chipRow.implicitWidth
    implicitHeight: chipRow.implicitHeight

    BaristaChipRow {
        id: chipRow
        anchors.centerIn: parent
        alwaysShow: true
        compact: root.isCompact
        showAdd: false   // switch-between-existing-people only; add a person from the menu
    }
}
