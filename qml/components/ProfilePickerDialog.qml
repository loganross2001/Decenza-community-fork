// The row delegate accesses the outer `root` id (a nested component), which needs
// this pragma. Safe: the delegate declares its injected role `required property var
// modelData`, so role injection is unaffected.
pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// Profile picker opened from the profileQuickSelect layout widget. Lists the
// user's FAVORITE profiles (Settings.app.favoriteProfiles) and applies the tapped
// one by emitting its filename, which the item hands to ProfileManager.loadProfile.
// A plain list picker — mirrors TempPickerDialog's shape.
//
// Pure UI: no barista / AI dependencies. The list (`rows`) is computed by the
// item, each row: { label: <display name>, filename: <profile filename>,
// isCurrent: bool }.
//
// Roots at DecenzaDialog (the shared dialog base every dialog in the app uses),
// not QtQuick.Controls Dialog: the app base keeps AOT compilation and the app's
// dialog theming (dim, enter/exit transitions). See DecenzaDialog.qml.
DecenzaDialog {
    id: root
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(Theme.scaled(360), parent ? parent.width * 0.95 : Theme.scaled(360))
    height: Math.min(contentCol.implicitHeight + Theme.scaled(40), parent ? parent.height * 0.92 : Theme.scaled(640))
    modal: true
    closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
    padding: 0

    // [{ label: string, filename: string, isCurrent: bool }].
    property var rows: []

    // Emits the picked profile's FILENAME (the stable id loadProfile resolves).
    signal profilePicked(string filename)

    background: Rectangle {
        color: Theme.dialogBackgroundColor
        radius: Theme.cardRadius
        border.width: 1
        border.color: Theme.borderColor
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
            spacing: Theme.spacingSmall

            // --- Header ---
            ColumnLayout {
                Layout.fillWidth: true
                Layout.topMargin: Theme.spacingLarge
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                spacing: Theme.scaled(2)
                Text {
                    text: TranslationManager.translate("profile.picker.title", "Favorite Profiles")
                    color: Theme.textColor
                    font.pixelSize: Theme.scaled(24); font.bold: true
                }
                Text {
                    text: TranslationManager.translate("profile.picker.subtitle", "Tap a profile to load it")
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                }
            }

            // --- Profile rows ---
            Repeater {
                model: root.rows
                delegate: Rectangle {
                    id: rowRect
                    required property var modelData
                    readonly property bool isCurrent: modelData.isCurrent === true
                    Layout.fillWidth: true
                    Layout.leftMargin: Theme.spacingLarge
                    Layout.rightMargin: Theme.spacingLarge
                    Layout.preferredHeight: Theme.scaled(48)
                    radius: Theme.buttonRadius
                    color: rowMa.pressed ? Qt.darker(Theme.backgroundColor, 1.1) : Theme.backgroundColor
                    border.width: isCurrent ? 2 : 1
                    border.color: isCurrent ? Theme.primaryColor : Theme.borderColor

                    Accessible.role: Accessible.Button
                    Accessible.name: String(modelData.label)
                                     + (isCurrent ? ", " + TranslationManager.translate("profile.picker.current", "current") : "")
                    Accessible.focusable: true
                    Accessible.onPressAction: rowMa.clicked(null)

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: Theme.spacingMedium
                        anchors.rightMargin: Theme.spacingMedium
                        spacing: Theme.spacingSmall
                        Text {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: String(rowRect.modelData.label)
                            color: rowRect.isCurrent ? Theme.primaryColor : Theme.textColor
                            font.pixelSize: Theme.scaled(20)
                            font.bold: rowRect.isCurrent
                            Accessible.ignored: true   // the delegate Rectangle carries Accessible.name
                        }
                        Text {
                            visible: rowRect.isCurrent
                            text: TranslationManager.translate("profile.picker.current", "current").toUpperCase()
                            color: Theme.primaryColor
                            font: Theme.captionFont
                            Accessible.ignored: true   // the delegate Rectangle carries Accessible.name
                        }
                    }

                    MouseArea {
                        id: rowMa
                        anchors.fill: parent
                        onClicked: {
                            root.profilePicked(String(rowRect.modelData.filename))
                            root.close()
                        }
                    }
                }
            }

            // --- Empty state (no favorites yet) ---
            Text {
                visible: root.rows.length === 0
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: TranslationManager.translate("profile.picker.empty",
                        "No favorite profiles yet. Star a profile to add it here.")
                color: Theme.textSecondaryColor
                font: Theme.labelFont
            }

            // --- Close ---
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.spacingLarge
                Layout.rightMargin: Theme.spacingLarge
                Layout.topMargin: Theme.spacingSmall
                Layout.bottomMargin: Theme.spacingLarge
                Layout.preferredHeight: Theme.scaled(48)
                radius: Theme.buttonRadius
                color: closeMa.pressed ? Qt.darker(Theme.primaryColor, 1.15) : Theme.primaryColor
                Accessible.role: Accessible.Button
                Accessible.name: TranslationManager.translate("common.button.close", "Close")
                Accessible.focusable: true
                Accessible.onPressAction: closeMa.clicked(null)
                Text {
                    anchors.centerIn: parent
                    text: TranslationManager.translate("common.button.close", "Close")
                    color: Theme.primaryContrastColor
                    font: Theme.bodyFont
                    Accessible.ignored: true   // the Close Rectangle carries Accessible.name
                }
                MouseArea { id: closeMa; anchors.fill: parent; onClicked: root.close() }
            }
        }
    }
}
