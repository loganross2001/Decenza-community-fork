import QtQuick
import QtQuick.Layouts
import Decenza

// Layout widget: favorite-profiles quick-select (composable-brew-bar). A sibling
// of the ratio/grind/temp quick-select pills. Shows the current profile name as a
// pill; tapping opens a picker of the user's FAVORITE profiles
// (Settings.app.favoriteProfiles) and loads the tapped one via
// ProfileManager.loadProfile(filename) — the same load the profile selector uses.
//
// When there are no favorites the pill is a plain read-out (see canQuickSelect):
// nothing to pick, so it doesn't open an empty picker.
//
// Pure layout widget: no barista / AI dependencies.
// [fork-index] pill=profileQuickSelect | domain=recipe | change=-
//   what: brew-bar pill — pick from favorite profiles; ProfileManager.loadProfile
LayoutWidgetItem {
    id: root

    readonly property string labelText: TranslationManager.translate("profile.quickSelect.label", "Profile")

    // The active profile's display name — what the pill shows.
    readonly property string currentName: ProfileManager.currentProfileName
    readonly property string valueText: root.currentName !== "" ? root.currentName : "—"

    // The favorites list mapped to picker rows. Reads favoriteProfiles (the stored
    // list) and currentProfileName so the highlight tracks the active profile.
    //   { label: <display name>, filename: <profile filename>, isCurrent: bool }.
    readonly property var rows: {
        var favs = Settings.app.favoriteProfiles
        var cur = ProfileManager.currentProfileName
        var out = []
        for (var i = 0; i < favs.length; i++) {
            var f = favs[i]
            out.push({ label: f.name, filename: f.filename, isCurrent: f.name === cur })
        }
        return out
    }

    // Interactive only when there is at least one favorite to pick; otherwise the
    // pill is a plain read-out so it never opens an empty picker.
    readonly property bool canQuickSelect: root.rows.length > 0

    function applyFilename(filename) {
        ProfileManager.loadProfile(filename)
    }

    implicitWidth: col.implicitWidth
    implicitHeight: col.implicitHeight

    ColumnLayout {
        id: col
        anchors.centerIn: parent
        width: parent.width
        spacing: Theme.scaled(2)

        Text {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            text: root.labelText
            color: root.zoneTextColor
            font: Theme.labelFont
        }

        Rectangle {
            Layout.alignment: Qt.AlignHCenter
            // Cap the width so a long profile name can't blow up the brew bar; the
            // name elides inside the pill.
            Layout.preferredWidth: Math.min(profileName.implicitWidth, Theme.scaled(150))
                                   + Theme.spacingMedium * 2
            Layout.preferredHeight: Theme.scaled(32)
            radius: height / 2
            color: (profileMa.pressed && root.canQuickSelect) ? Qt.darker(root.zoneTextColor, 1.15) : root.zoneTextColor
            // Dim to a plain read-out when there are no favorites to pick.
            opacity: root.canQuickSelect ? 1.0 : 0.55

            Accessible.role: root.canQuickSelect ? Accessible.Button : Accessible.StaticText
            Accessible.name: root.canQuickSelect
                ? root.labelText + " " + root.valueText + ". "
                  + TranslationManager.translate("profile.quickSelect.tapToChange", "Tap to switch profile")
                : root.labelText + " " + root.valueText
            Accessible.focusable: true
            Accessible.onPressAction: { if (root.canQuickSelect) profileMa.clicked(null) }

            Text {
                id: profileName
                anchors.centerIn: parent
                width: Math.min(implicitWidth, Theme.scaled(150))
                elide: Text.ElideRight
                horizontalAlignment: Text.AlignHCenter
                text: root.valueText
                Accessible.ignored: true   // the pill Rectangle carries Accessible.name
                color: Theme.primaryColor
                font.pixelSize: Theme.scaled(20)
                font.bold: true
            }
            MouseArea {
                id: profileMa
                anchors.fill: parent
                enabled: root.canQuickSelect   // inert read-out when there are no favorites
                onClicked: profileDialog.open()
            }
        }
    }

    ProfilePickerDialog {
        id: profileDialog
        rows: root.rows
        onProfilePicked: function(filename) { root.applyFilename(filename) }
    }
}
