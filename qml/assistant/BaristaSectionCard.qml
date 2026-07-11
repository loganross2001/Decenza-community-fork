import QtQuick
import QtQuick.Layouts
import Decenza

// [barista-fork] A grouped SECTION CARD for the barista settings panel: an uppercase caption (e.g. "VOICE",
// "SAVED VOICES", "VOLUME & SPEED"), a filled + rounded-bordered surface, and a content column its callers
// fill via `default property alias content`. Related controls sit INSIDE this card instead of a flat list,
// so each tab reads as labelled groups.
//
// Colour note: the panel root is transparent over the host settingsCard's surfaceColor, so a surfaceColor
// card would vanish against it. We fill with backgroundColor + a 1px border — the same "backgroundColor
// content inside a surfaceColor host" convention the saved-voice/maintenance rows already use — so the card
// reads clearly. Width is driven by the parent layout (Layout.fillWidth from the caller).
Rectangle {
    id: root

    // The uppercase caption shown above the grouped controls (already-translated text).
    property string caption: ""

    // Callers place their controls as children; they land in `bodyColumn`.
    default property alias content: bodyColumn.data

    Layout.fillWidth: true
    implicitHeight: cardCol.implicitHeight + Theme.spacingMedium * 2
    radius: Theme.cardRadius
    color: Theme.backgroundColor
    border.width: 1
    border.color: Theme.borderColor

    ColumnLayout {
        id: cardCol
        anchors.fill: parent
        anchors.margins: Theme.spacingMedium
        spacing: Theme.spacingSmall

        // Uppercase section caption — a quiet, letter-spaced label so groups scan at a glance.
        Text {
            visible: root.caption.length > 0
            Layout.fillWidth: true
            text: root.caption.toUpperCase()
            color: Theme.textSecondaryColor
            font.pixelSize: Theme.captionFont.pixelSize
            font.bold: true
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 1
            Accessible.role: Accessible.Heading
            Accessible.name: root.caption
        }

        // The grouped controls. A ColumnLayout so callers use the usual Layout.* attached properties.
        ColumnLayout {
            id: bodyColumn
            Layout.fillWidth: true
            Layout.topMargin: root.caption.length > 0 ? Theme.scaled(4) : 0
            spacing: Theme.spacingMedium
        }
    }
}
