import QtQuick
import QtQuick.Layouts
import Decenza

// [barista-fork] Lets the Repeater delegate reference outer ids (root, editDialog) from its nested
// scope. Safe here: the delegate already declares `required property var modelData`, so it does not
// rely on injected model roles (the case QML_GOTCHAS warns the pragma would break).
pragma ComponentBehavior: Bound

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

    // When true, always render (existing chips + the "+" add chip) regardless of
    // roster size. Set by the placeable BaristaSwitcherItem: a widget the user
    // deliberately added should never silently vanish. Default false preserves the
    // legacy self-hide for any other caller.
    property bool alwaysShow: false

    // Compact (bar) layout: name sits BESIDE the avatar in a short row that fits a
    // bar's height, instead of a taller avatar-over-name stack. Set from the
    // widget's isCompact (true in top/bottom/status bars). Large stacked style is
    // used in center zones and by the legacy hardcoded placement.
    property bool compact: false

    // Show the trailing "+" chip that creates a new person. Off for the idle-screen
    // switcher widget: it is a switch-between-existing-people control; adding a
    // person is a rare action done from the menu, not something to keep on the home
    // screen. Default true preserves the full picker for any other caller.
    property bool showAdd: true

    // Avatar sizing keyed off compact so the whole row stays within the bar height.
    readonly property real avatarOuter: compact ? Theme.scaled(34) : Theme.scaled(56)
    readonly property real avatarInner: compact ? Theme.scaled(30) : Theme.scaled(46)

    readonly property var baristaStorage: MainController.baristaStorage

    // Hide entirely for single-user homes — the picker only earns its space
    // once there is more than one person to switch between (unless alwaysShow).
    visible: alwaysShow || roster.length > 1
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

            // GridLayout so the avatar/name arrangement flips with `compact`:
            // columns:2 → avatar beside name (short, fits a bar); columns:1 →
            // avatar above name (taller, for center zones / legacy placement).
            delegate: GridLayout {
                id: chip
                required property var modelData
                readonly property bool active: root.isActive(modelData)

                Layout.alignment: Qt.AlignVCenter
                columns: root.compact ? 2 : 1
                rowSpacing: root.compact ? 0 : Theme.scaled(4)
                columnSpacing: root.compact ? Theme.scaled(6) : 0

                // Circular avatar with selection ring on the active chip.
                Item {
                    Layout.alignment: root.compact ? Qt.AlignVCenter : Qt.AlignHCenter
                    implicitWidth: root.avatarOuter
                    implicitHeight: root.avatarOuter

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
                        width: root.avatarInner
                        height: root.avatarInner
                        radius: width / 2
                        color: root.chipColorFor(chip.modelData)
                        opacity: avatarTap.isPressed ? 0.7 : 1.0

                        Image {
                            anchors.centerIn: parent
                            visible: root.hasEmojiAvatar(chip.modelData)
                            source: visible ? Theme.emojiToImage(chip.modelData.avatar) : ""
                            sourceSize.width: Math.round(root.avatarInner * 0.57)
                            sourceSize.height: Math.round(root.avatarInner * 0.57)
                            Accessible.ignored: true
                        }

                        Text {
                            anchors.centerIn: parent
                            visible: !root.hasEmojiAvatar(chip.modelData)
                            text: root.initialFor(chip.modelData)
                            color: Theme.primaryContrastColor
                            font.pixelSize: Math.round(root.avatarInner * 0.48)
                            font.bold: true
                            Accessible.ignored: true
                        }

                        Behavior on opacity { NumberAnimation { duration: 100 } }
                    }
                }

                Text {
                    Layout.alignment: root.compact ? Qt.AlignVCenter : Qt.AlignHCenter
                    Layout.maximumWidth: Theme.scaled(80)
                    text: chip.modelData.name || ""
                    color: chip.active ? Theme.primaryColor : Theme.textColor
                    font: Theme.captionFont
                    elide: Text.ElideRight
                    horizontalAlignment: root.compact ? Text.AlignLeft : Text.AlignHCenter
                    Accessible.ignored: true
                }

                AccessibleTapHandler {
                    id: avatarTap
                    // qmllint disable Quick.layout-positioning
                    anchors.fill: parent   // intentional tap overlay covering the whole chip, NOT a layout cell
                    // qmllint enable Quick.layout-positioning
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
        GridLayout {
            visible: root.showAdd
            Layout.alignment: Qt.AlignVCenter
            columns: root.compact ? 2 : 1
            rowSpacing: root.compact ? 0 : Theme.scaled(4)
            columnSpacing: root.compact ? Theme.scaled(6) : 0

            Rectangle {
                id: addCircle
                Layout.alignment: root.compact ? Qt.AlignVCenter : Qt.AlignHCenter
                implicitWidth: root.avatarInner
                implicitHeight: root.avatarInner
                radius: width / 2
                color: "transparent"
                border.width: 1
                border.color: Theme.borderColor
                opacity: addTap.isPressed ? 0.7 : 1.0

                Text {
                    anchors.centerIn: parent
                    text: "+"
                    color: Theme.textSecondaryColor
                    font.pixelSize: Math.round(root.avatarInner * 0.57)
                    Accessible.ignored: true
                }

                Behavior on opacity { NumberAnimation { duration: 100 } }
            }

            Text {
                Layout.alignment: root.compact ? Qt.AlignVCenter : Qt.AlignHCenter
                text: TranslationManager.translate("barista.add", "Add")
                color: Theme.textSecondaryColor
                font: Theme.captionFont
                horizontalAlignment: root.compact ? Text.AlignLeft : Text.AlignHCenter
                Accessible.ignored: true
            }

            AccessibleTapHandler {
                id: addTap
                // qmllint disable Quick.layout-positioning
                anchors.fill: parent   // intentional tap overlay covering the whole add-chip, NOT a layout cell
                // qmllint enable Quick.layout-positioning
                accessibleName: TranslationManager.translate("barista.addBarista", "Add barista")
                accessibleItem: addCircle
                onAccessibleClicked: editDialog.openForCreate()
            }
        }
    }
}
