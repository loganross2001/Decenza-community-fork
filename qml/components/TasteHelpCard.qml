import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] "How to describe a shot" cheat sheet. A dismissible card that pops over the main screen after a
// shot you haven't described yet, so you know the words to SPEAK to the barista. It's guidance only — no taps
// record anything here; saying these words to the barista is what logs the shot's taste. The two structured
// axes (extraction + body) reuse the SAME i18n keys as TastePicker so the vocabulary never drifts from the tap
// UI; the richer examples below are free description the barista also understands.
Popup {
    id: root

    modal: true
    dim: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, Overlay.overlay ? Overlay.overlay.width - 2 * Theme.spacingLarge : 560)
    padding: Theme.spacingLarge

    // Small helper so every visible string reacts to a language change (binding over translate()).
    function tr(key, fallback) { return TranslationManager.translate(key, fallback) }

    background: Rectangle {
        color: Theme.dialogBackgroundColor
        radius: Theme.cardRadius
        border.color: Theme.borderColor
        border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: Theme.spacingMedium

        Text {
            Layout.fillWidth: true
            text: root.tr("barista.tasteHelp.title", "Describing your shot")
            color: Theme.textColor
            font.family: Theme.subtitleFont.family
            font.pixelSize: Theme.subtitleFont.pixelSize
            font.bold: true
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            text: root.tr("barista.tasteHelp.intro",
                          "Just tell me how it tasted — no need to tap, and I'll note it. For example:")
            color: Theme.textSecondaryColor
            font.family: Theme.bodyFont.family
            font.pixelSize: Theme.bodyFont.pixelSize
            wrapMode: Text.WordWrap
        }

        // The two structured axes (these map to a saved rating). Labels come from the TastePicker i18n keys.
        Text {
            Layout.fillWidth: true
            text: root.tr("tasteIntake.sour", "Sour") + "  ·  "
                  + root.tr("tasteIntake.balanced", "Balanced") + "  ·  "
                  + root.tr("tasteIntake.bitter", "Bitter")
            color: Theme.textColor
            font.family: Theme.bodyFont.family
            font.pixelSize: Theme.bodyFont.pixelSize
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Text {
            Layout.fillWidth: true
            text: root.tr("tasteIntake.thin", "Thin") + "  ·  "
                  + root.tr("tasteIntake.medium", "Medium") + "  ·  "
                  + root.tr("tasteIntake.heavy", "Heavy")
            color: Theme.textColor
            font.family: Theme.bodyFont.family
            font.pixelSize: Theme.bodyFont.pixelSize
            font.bold: true
            wrapMode: Text.WordWrap
        }

        // Richer spoken vocabulary — free description the barista understands (not tap axes).
        Text {
            Layout.fillWidth: true
            text: root.tr("barista.tasteHelp.richer",
                          "…or describe it your own way — bright, fruity, chocolatey, nutty, watery, bold, "
                          + "harsh, sweet, syrupy, astringent — and whether you liked it.")
            color: Theme.textSecondaryColor
            font.family: Theme.bodyFont.family
            font.pixelSize: Theme.bodyFont.pixelSize
            wrapMode: Text.WordWrap
        }

        AccessibleButton {
            Layout.alignment: Qt.AlignRight
            Layout.topMargin: Theme.spacingSmall
            text: root.tr("common.button.gotIt", "Got it")
            onClicked: root.close()
        }
    }
}
