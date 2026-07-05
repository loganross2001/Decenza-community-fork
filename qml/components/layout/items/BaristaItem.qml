import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import Decenza
import "../.."

// [barista-fork] Home-screen action widget: tap to summon the barista assistant (wakes the greeting/chat
// card), the same call IdlePage makes on Espresso. Modelled on DiscussItem. Only shown when the barista
// is enabled. Placeable anywhere in the layout editor (e.g. the bottom-left where Discuss lives).
Item {
    id: root
    property bool isCompact: false
    property string itemId: ""
    visible: typeof Barista !== "undefined" && Barista.enabled

    implicitWidth: isCompact ? compactContent.implicitWidth : fullContent.implicitWidth
    implicitHeight: isCompact ? compactContent.implicitHeight : fullContent.implicitHeight

    function openBarista() {
        if (typeof Barista !== "undefined" && Barista.orchestrator)
            Barista.orchestrator.wake()
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
    Item {
        id: fullContent
        visible: !root.isCompact
        anchors.fill: parent
        implicitWidth: Theme.scaled(150)
        implicitHeight: Theme.scaled(120)

        ActionButton {
            anchors.fill: parent
            translationKey: "idle.button.barista"
            translationFallback: "Barista"
            iconSource: "qrc:/icons/barista.svg"
            iconSize: Theme.scaled(43)
            backgroundColor: Theme.primaryColor
            onClicked: root.openBarista()
        }
    }
}
