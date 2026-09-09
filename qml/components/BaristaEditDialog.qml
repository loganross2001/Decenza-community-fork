import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] Lets the color/avatar Repeater delegates reference outer ids (root) from their
// nested scopes. Safe: both delegates already declare `required property var modelData`, so they do
// not rely on injected model roles (the case QML_GOTCHAS warns the pragma would break).
pragma ComponentBehavior: Bound

// Create / rename / delete a barista (pr/barista-identity). Opened by
// BaristaChipRow's "+" chip (create) and a chip long-press (edit). Writes go
// through MainController.baristaStorage; the caller refreshes its roster on the
// saved / deleted signals.
Dialog {
    id: root
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(Theme.scaled(460), parent ? parent.width * 0.95 : Theme.scaled(460))
    modal: true
    closePolicy: Dialog.CloseOnEscape | Dialog.CloseOnPressOutside
    padding: 0

    readonly property var baristaStorage: MainController.baristaStorage

    // Editing state. id <= 0 = create mode.
    property int baristaId: 0
    property string pickedColor: ""
    property string pickedAvatar: ""

    // Async-write state: which write we dispatched and are waiting on
    // ("create" | "update" | "delete" | ""), and the last error to surface. We
    // close ONLY when the storage confirms success, so a duplicate name hitting
    // the UNIQUE constraint (or any DB failure) keeps the dialog open with
    // feedback instead of silently dropping the edit.
    property string _pendingOp: ""
    property string _error: ""

    signal saved()
    signal deleted()

    // Theme-derived swatch palette — accent + neutral surface/border tokens so the
    // picker tracks the active theme rather than hardcoding hex colors.
    readonly property var swatches: [
        Theme.primaryColor,
        Theme.successColor,
        Theme.warningColor,
        Theme.errorColor,
        Theme.accentColor,
        Theme.textSecondaryColor
    ]

    // A small set of avatar emoji plus the "initial" fallback (empty = use the
    // name's first letter at render time).
    readonly property var avatarChoices: ["", "☕", "🫖", "🌱", "⭐", "🔥", "🍫", "🥛"]

    function openForCreate() {
        baristaId = 0
        nameField.text = ""
        pickedColor = swatches.length > 0 ? swatches[0] : ""
        pickedAvatar = ""
        _pendingOp = ""
        _error = ""
        open()
    }

    function openForEdit(barista) {
        baristaId = barista.id || 0
        nameField.text = barista.name || ""
        pickedColor = (barista.color && barista.color.length > 0)
                          ? barista.color
                          : (swatches.length > 0 ? swatches[0] : "")
        pickedAvatar = barista.avatar || ""
        _pendingOp = ""
        _error = ""
        open()
    }

    function commit() {
        Keyboard.commit()  // flush in-progress IME word before reading text (compile-time singleton; Qt.inputMethod is typed as bare QObject and its .commit() is unresolvable — see CLAUDE.md QML gotchas)
        var name = nameField.text.trim()
        if (name.length === 0 || !baristaStorage || _pendingOp.length > 0)
            return
        _error = ""
        var fields = { "name": name, "color": pickedColor, "avatar": pickedAvatar }
        if (baristaId > 0) {
            _pendingOp = "update"
            baristaStorage.requestUpdateBarista(baristaId, fields)
        } else {
            _pendingOp = "create"
            baristaStorage.requestCreateBarista(fields)
        }
        // Do NOT close here — wait for the storage result (Connections below).
    }

    function removeBarista() {
        if (!(baristaId > 0) || !baristaStorage || _pendingOp.length > 0)
            return
        _error = ""
        _pendingOp = "delete"
        baristaStorage.requestDeleteBarista(baristaId)
    }

    // Close only on a confirmed success; on failure (most often a duplicate name
    // hitting the UNIQUE constraint) stay open and show why.
    Connections {
        target: root.baristaStorage
        function onBaristaCreated(id, barista) {
            if (root._pendingOp !== "create")
                return
            root._pendingOp = ""
            if (id > 0) {
                root.saved()
                root.close()
            } else {
                root._error = TranslationManager.translate("barista.error.save",
                    "Couldn't save — that name may already be in use.")
            }
        }
        function onBaristaUpdated(id, success) {
            if (root._pendingOp !== "update" || id !== root.baristaId)
                return
            root._pendingOp = ""
            if (success) {
                root.saved()
                root.close()
            } else {
                root._error = TranslationManager.translate("barista.error.save",
                    "Couldn't save — that name may already be in use.")
            }
        }
        function onBaristaDeleted(id, success) {
            if (root._pendingOp !== "delete" || id !== root.baristaId)
                return
            root._pendingOp = ""
            if (success) {
                root.deleted()
                root.close()
            } else {
                root._error = TranslationManager.translate("barista.error.delete",
                    "Couldn't delete this barista.")
            }
        }
    }

    background: Rectangle {
        color: Theme.surfaceColor
        radius: Theme.cardRadius
        border.width: 1
        border.color: Theme.borderColor
    }

    contentItem: ColumnLayout {
        id: contentCol
        spacing: Theme.spacingMedium

        // --- Title ---
        Text {
            Layout.fillWidth: true
            Layout.topMargin: Theme.spacingLarge
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            text: root.baristaId > 0
                      ? TranslationManager.translate("barista.edit.title", "Edit Barista")
                      : TranslationManager.translate("barista.add.title", "Add Barista")
            color: Theme.textColor
            font.pixelSize: Theme.scaled(22)
            font.bold: true
        }

        // --- Name field ---
        StyledTextField {
            id: nameField
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            placeholder: TranslationManager.translate("barista.name.placeholder", "Name")
            accessibleName: TranslationManager.translate("barista.name.label", "Barista name")
            onTextChanged: root._error = ""  // clear stale error once the user edits
        }

        // Failure feedback (e.g. duplicate name) — only shown when a write fails.
        Text {
            visible: root._error.length > 0
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            text: root._error
            color: Theme.errorColor
            font: Theme.labelFont
            wrapMode: Text.WordWrap
        }

        // --- Color picker ---
        Text {
            Layout.leftMargin: Theme.spacingLarge
            text: TranslationManager.translate("barista.color.label", "Color")
            color: Theme.textSecondaryColor
            font: Theme.labelFont
        }
        RowLayout {
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            spacing: Theme.spacingSmall

            Repeater {
                model: root.swatches
                delegate: Item {
                    id: swatch
                    required property var modelData
                    readonly property bool picked: root.pickedColor === modelData
                    implicitWidth: Theme.scaled(40)
                    implicitHeight: Theme.scaled(40)

                    Rectangle {
                        anchors.fill: parent
                        radius: width / 2
                        color: "transparent"
                        visible: swatch.picked
                        border.width: Theme.focusBorderWidth
                        border.color: Theme.textColor
                    }
                    Rectangle {
                        anchors.centerIn: parent
                        width: Theme.scaled(32)
                        height: Theme.scaled(32)
                        radius: width / 2
                        color: swatch.modelData
                    }

                    AccessibleMouseArea {
                        anchors.fill: parent
                        accessibleName: TranslationManager.translate("barista.color.choose", "Choose color")
                                        + (swatch.picked
                                            ? ", " + TranslationManager.translate("barista.selected", "selected")
                                            : "")
                        onAccessibleClicked: root.pickedColor = swatch.modelData
                    }
                }
            }
        }

        // --- Avatar picker ---
        Text {
            Layout.leftMargin: Theme.spacingLarge
            text: TranslationManager.translate("barista.avatar.label", "Avatar")
            color: Theme.textSecondaryColor
            font: Theme.labelFont
        }
        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            spacing: Theme.spacingSmall

            Repeater {
                model: root.avatarChoices
                delegate: Rectangle {
                    id: avatarOption
                    required property var modelData
                    readonly property bool isInitial: modelData === ""
                    readonly property bool picked: root.pickedAvatar === modelData
                    width: Theme.scaled(40)
                    height: Theme.scaled(40)
                    radius: width / 2
                    color: Theme.backgroundColor
                    border.width: picked ? Theme.focusBorderWidth : 1
                    border.color: picked ? Theme.primaryColor : Theme.borderColor

                    Text {
                        anchors.centerIn: parent
                        visible: avatarOption.isInitial
                        text: TranslationManager.translate("barista.avatar.initial", "Aa")
                        color: Theme.textColor
                        font.pixelSize: Theme.scaled(18)
                        Accessible.ignored: true
                    }
                    Image {
                        anchors.centerIn: parent
                        visible: !avatarOption.isInitial
                        source: visible ? Theme.emojiToImage(avatarOption.modelData) : ""
                        sourceSize.width: Theme.scaled(22)
                        sourceSize.height: Theme.scaled(22)
                        Accessible.ignored: true
                    }

                    AccessibleMouseArea {
                        anchors.fill: parent
                        accessibleName: avatarOption.isInitial
                                  ? TranslationManager.translate("barista.avatar.useInitial", "Use initial")
                                  : TranslationManager.translate("barista.avatar.choose", "Choose avatar")
                        onAccessibleClicked: root.pickedAvatar = avatarOption.modelData
                    }
                }
            }
        }

        // --- Actions ---
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Theme.spacingLarge
            Layout.rightMargin: Theme.spacingLarge
            Layout.bottomMargin: Theme.spacingLarge
            Layout.topMargin: Theme.spacingSmall
            spacing: Theme.spacingMedium

            AccessibleButton {
                visible: root.baristaId > 0
                enabled: root._pendingOp.length === 0
                destructive: true
                text: TranslationManager.translate("common.button.delete", "Delete")
                accessibleName: TranslationManager.translate("barista.delete", "Delete barista")
                onClicked: root.removeBarista()
            }

            Item { Layout.fillWidth: true }

            AccessibleButton {
                text: TranslationManager.translate("common.button.cancel", "Cancel")
                accessibleName: TranslationManager.translate("common.button.cancel", "Cancel")
                onClicked: root.close()
            }

            AccessibleButton {
                primary: true
                enabled: nameField.text.trim().length > 0 && root._pendingOp.length === 0
                text: TranslationManager.translate("common.button.save", "Save")
                accessibleName: TranslationManager.translate("common.button.save", "Save")
                onClicked: root.commit()
            }
        }
    }
}
