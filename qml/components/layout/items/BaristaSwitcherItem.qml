import QtQuick
import Decenza

// Layout widget: the barista roster switcher — the one-tap active-user picker
// that used to be hardcoded at the top of IdlePage. Wrapping BaristaChipRow as a
// catalog widget lets the layout editor place it in any zone (bar or center),
// like every other idle-screen element, instead of pinning it to a fixed spot.
//
// BaristaChipRow self-hides when the roster has 0 or 1 people, so a single-user
// home still sees nothing here (the item collapses to zero size and the zone
// simply renders empty) — the zero-friction behavior is preserved.
Item {
    id: root

    // Standard layout-item interface (set by LayoutItemDelegate.onLoaded).
    property bool isCompact: false
    property string itemId: ""
    property var modelData: ({})
    property color zoneTextColor: Theme.textColor
    property bool zoneValueBold: false

    // Forward BaristaChipRow's self-hide (roster <= 1) up to the layout item, so a
    // single-user home collapses this widget to nothing wherever it is placed —
    // otherwise the wrapper would stay visible and reserve space in its zone.
    visible: chipRow.visible
    implicitWidth: chipRow.visible ? chipRow.implicitWidth : 0
    implicitHeight: chipRow.visible ? chipRow.implicitHeight : 0

    BaristaChipRow {
        id: chipRow
        anchors.centerIn: parent
    }
}
