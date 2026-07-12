import QtQuick
import QtQuick.Layouts
import Decenza

// Horizontal row of barista chips for the idle screen (pr/barista-identity).
// Each chip is a circular avatar (the person's `avatar` string, else the first
// letter of their name) filled in their `color` (Theme fallback when empty) with
// the name beneath. The active barista (name === Settings.dye.dyeBarista) gets a
// selection ring. Tapping a chip switches the active barista and bumps its MRU;
// long-pressing opens the edit dialog. A trailing "+" chip creates a new person.
//
// The row hides itself when the roster has 0 or 1 people: a single-user home
// never sees the picker (zero friction). The roster is mirrored into a local
// model, refreshed from MainController.baristaStorage on completion and whenever
// baristasChanged/rosterReady fires.
FocusScope {
    id: root

    // Local mirror of the roster (QVariantList of barista variant maps).
    property var roster: []

    readonly property var baristaStorage: MainController.baristaStorage

    // Hide entirely for single-user homes — the picker only earns its space
    // once there is more than one person to switch between.
    visible: roster.length > 1
    implicitHeight: visible ? chipRow.implicitHeight : 0
    implicitWidth: chipRow.implicitWidth

    function refresh() {
        if (baristaStorage)
            baristaStorage.requestRoster()
    }

    // Avatars are either an emoji (rendered as an SVG Image) or empty, in which
    // case we fall back to the name's first initial as text.
    function hasEmojiAvatar(barista) {
        return barista.avatar && barista.avatar.length > 0
    }

    function initialFor(barista) {
        var name = barista.name || ""
        return name.length > 0 ? name.charAt(0).toUpperCase() : "?"
    }

    function chipColorFor(barista) {
        return (barista.color && barista.color.length > 0) ? barista.color : Theme.primaryColor
    }

    function isActive(barista) {
        return (barista.name || "") === Settings.dye.dyeBarista
    }

    function selectBarista(barista) {
        Settings.dye.dyeBarista = barista.name || ""   // [barista-fork] property assign; setDyeBarista() isn't Q_INVOKABLE (silent TypeError)
        if (baristaStorage && barista.id > 0)
            baristaStorage.requestTouchLastUsed(barista.id)
    }

    Component.onCompleted: refresh()

    Connections {
        target: root.baristaStorage
        function onRosterReady(baristas) { root.roster = baristas }
        function onBaristasChanged() { root.refresh() }
    }

    BaristaEditDialog {
        id: editDialog
        onSaved: root.refresh()
        onDeleted: root.refresh()
    }

    RowLayout {
        id: chipRow
        anchors.centerIn: parent
        spacing: Theme.spacingMedium

        Repeater {
            model: root.roster

            delegate: ColumnLayout {
                id: chip
                required property var modelData
                readonly property bool active: root.isActive(modelData)

                Layout.alignment: Qt.AlignVCenter
                spacing: Theme.scaled(4)

                // Circular avatar with selection ring on the active chip.
                Item {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: Theme.scaled(56)
                    implicitHeight: Theme.scaled(56)

                    // Selection ring — mirrors the PresetPillRow / Theme focus look.
                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: "transparent"
                        visible: chip.active
                        border.width: Theme.focusBorderWidth
                        border.color: Theme.primaryColor
                    }

                    Rectangle {
                        id: avatarCircle
                        anchors.centerIn: parent
                        width: Theme.scaled(46)
                        height: Theme.scaled(46)
                        radius: width / 2
                        color: root.chipColorFor(chip.modelData)
                        opacity: avatarTap.isPressed ? 0.7 : 1.0

                        Image {
                            anchors.centerIn: parent
                            visible: root.hasEmojiAvatar(chip.modelData)
                            source: visible ? Theme.emojiToImage(chip.modelData.avatar) : ""
                            sourceSize.width: Theme.scaled(26)
                            sourceSize.height: Theme.scaled(26)
                            Accessible.ignored: true
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: !root.hasEmojiAvatar(chip.modelData)
                            text: root.initialFor(chip.modelData)
                            color: Theme.primaryContrastColor
                            font.pixelSize: Theme.scaled(22)
                            font.bold: true
                            Accessible.ignored: true
                        }

                        Behavior on opacity { NumberAnimation { duration: 100 } }
                    }
                }

                Text {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: Theme.scaled(80)
                    text: chip.modelData.name || ""
                    color: chip.active ? Theme.primaryColor : Theme.textColor
                    font: Theme.captionFont
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    Accessible.ignored: true
                }

                AccessibleTapHandler {
                    id: avatarTap
                    anchors.fill: parent
                    supportLongPress: true
                    accessibleName: TranslationManager.translate("barista.brewAs", "Brew as %1")
                                        .arg(chip.modelData.name || "")
                                    + (chip.active
                                        ? ", " + TranslationManager.translate("barista.selected", "selected")
                                        : "")
                    accessibleDescription: TranslationManager.translate(
                        "barista.chipHint", "Long-press to edit.")
                    accessibleItem: chip
                    onAccessibleClicked: root.selectBarista(chip.modelData)
                    onAccessibleLongPressed: editDialog.openForEdit(chip.modelData)
                }
            }
        }

        // Trailing "+" chip — opens the edit dialog to create a new person.
        ColumnLayout {
            Layout.alignment: Qt.AlignVCenter
            spacing: Theme.scaled(4)

            Rectangle {
                id: addCircle
                Layout.alignment: Qt.AlignHCenter
                width: Theme.scaled(46)
                height: Theme.scaled(46)
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: Theme.borderColor
                opacity: addTap.isPressed ? 0.7 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    color: Theme.textSecondaryColor
                    font.pixelSize: Theme.scaled(26)
                    Accessible.ignored: true
                }

                Behavior on opacity { NumberAnimation { duration: 100 } }
            }

            Text {
                Layout.alignment: Qt.AlignHCenter
                text: TranslationManager.translate("barista.add", "Add")
                color: Theme.textSecondaryColor
                font: Theme.captionFont
                horizontalAlignment: Text.AlignHCenter
                Accessible.ignored: true
            }

            AccessibleTapHandler {
                id: addTap
                anchors.fill: parent
                accessibleName: TranslationManager.translate("barista.addBarista", "Add barista")
                accessibleItem: addCircle
                onAccessibleClicked: editDialog.openForCreate()
            }
        }
    }
}
