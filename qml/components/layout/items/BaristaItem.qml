import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import Decenza
import "../.."

// [barista-fork] Home-screen action widget: tap to ENGAGE the barista assistant (opens the conversation
// panel + mic — user-initiated model). Modelled on DiscussItem. Only shown when the barista is enabled.
// Placeable anywhere in the layout editor (e.g. the bottom-left where Discuss lives).
Item {
    id: root
    property bool isCompact: false
    property string itemId: ""
    visible: typeof Barista !== "undefined" && Barista.enabled

    implicitWidth: isCompact ? compactContent.implicitWidth : fullContent.implicitWidth
    implicitHeight: isCompact ? compactContent.implicitHeight : fullContent.implicitHeight

    function openBarista() {
        // [barista-fork] User-initiated: engage() opens the conversation (→ Conversing); the overlay
        // observes the state change, expands, and opens the mic (tap-chat-and-talk).
        if (typeof Barista !== "undefined" && Barista.orchestrator)
            Barista.orchestrator.engage()
    }

    // The assistant's own name (e.g. "Coach") for the spoken/accessible label; falls back to "Barista".
    readonly property string _name: (typeof Barista !== "undefined" && Barista.settings
                                     && Barista.settings.assistantName.length > 0)
                                    ? Barista.settings.assistantName
                                    : TranslationManager.translate("idle.button.barista", "Barista")

    // --- COMPACT MODE ---
    Item {
        id: compactContent
        visible: root.isCompact
        anchors.fill: parent
        implicitWidth: compactRow.implicitWidth + Theme.scaled(16)
        implicitHeight: Theme.bottomBarHeight

        RowLayout {
            id: compactRow
            anchors.centerIn: parent
            spacing: Theme.spacingSmall

            Image {
                source: "qrc:/icons/barista.svg"
                sourceSize.height: Theme.scaled(20)
                fillMode: Image.PreserveAspectFit
                Accessible.ignored: true

                layer.enabled: true
                layer.smooth: true
                layer.effect: MultiEffect {
                    colorization: 1.0
                    colorizationColor: Theme.textColor
                }
            }
            Tr {
                key: "idle.button.barista"
                fallback: "Barista"
                font: Theme.bodyFont
                color: Theme.textColor
                Accessible.ignored: true
            }
        }

        AccessibleTapHandler {
            anchors.fill: parent
            accessibleName: TranslationManager.translate("idle.accessible.barista.description",
                "Open %1 to chat about your coffee").arg(root._name)
            onAccessibleClicked: root.openBarista()
        }
    }

    // --- FULL MODE ---
    // A big tile has room for the LIVE animated character (the user's chosen avatar style, idling) as the
    // button face — a "tap me to chat" preview. If the character is turned off, fall back to the cup icon.
    Item {
        id: fullContent
        visible: !root.isCompact
        anchors.fill: parent
        implicitWidth: Theme.scaled(150)
        implicitHeight: Theme.scaled(120)

        readonly property bool _live: typeof Barista !== "undefined" && Barista.settings
                                      && Barista.settings.avatarEnabled

        // Static-icon button (character off).
        ActionButton {
            anchors.fill: parent
            visible: !fullContent._live
            translationKey: "idle.button.barista"
            translationFallback: "Barista"
            iconSource: "qrc:/icons/barista.svg"
            iconSize: Theme.scaled(43)
            backgroundColor: Theme.primaryColor
            onClicked: root.openBarista()
        }

        // Live-character button.
        Rectangle {
            anchors.fill: parent
            visible: fullContent._live
            radius: Theme.cardRadius
            color: Theme.primaryColor

            Column {
                anchors.centerIn: parent
                spacing: Theme.spacingSmall

                Loader {
                    anchors.horizontalCenter: parent.horizontalCenter
                    // Scale the character to the tile; a floor keeps it legible in a modest tile.
                    property real _sz: Math.max(Theme.scaled(48),
                                                Math.min(fullContent.width, fullContent.height) * 0.55)
                    width: _sz; height: _sz
                    active: fullContent._live && fullContent.visible
                    source: "qrc:/qml/assistant/BaristaAvatar.qml"
                    onLoaded: if (item) item.mode = "idle"   // idle animation as a preview
                }
                Tr {
                    anchors.horizontalCenter: parent.horizontalCenter
                    key: "idle.button.barista"; fallback: "Barista"
                    font: Theme.bodyFont
                    color: Theme.primaryContrastColor
                    Accessible.ignored: true
                }
            }

            AccessibleTapHandler {
                anchors.fill: parent
                accessibleName: TranslationManager.translate("idle.accessible.barista.description",
                    "Open %1 to chat about your coffee").arg(root._name)
                onAccessibleClicked: root.openBarista()
            }
        }
    }
}
