import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Decenza

// [barista-fork] The barista assistant's single global overlay (loaded by URL from main.qml).
// Non-modal: only the pill / card capture input, everything else passes through.
//
// P1a: renders the orchestrator state machine's first beat — a summon pill when dormant, and a
// greeting + "same bean?" card when woken. The state lives in C++ (Barista.orchestrator); this
// just renders it and calls the Q_INVOKABLE transitions. Name/bean come from existing settings.
// P1b adds the plan card; P1c adds the typed chat sheet + taste close-out.
Item {
    id: root
    anchors.fill: parent

    readonly property var _orch: (typeof Barista !== "undefined") ? Barista.orchestrator : null
    readonly property string _state: _orch ? _orch.state : "dormant"

    readonly property string _name: (Settings.dye.dyeBarista && Settings.dye.dyeBarista.length > 0)
                                    ? Settings.dye.dyeBarista : ""
    readonly property string _bean: {
        var brand = (Settings.dye.dyeBeanBrand || "")
        var type = (Settings.dye.dyeBeanType || "")
        var s = (brand + " " + type).trim()
        return s
    }

    // ---- Summon pill (dormant) -------------------------------------------------
    Rectangle {
        id: pill
        visible: root._state === "dormant" && typeof Barista !== "undefined" && Barista.enabled
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Theme.spacingLarge
        width: pillRow.implicitWidth + Theme.spacingMedium * 2
        height: Theme.touchTargetMin
        radius: height / 2
        color: pillArea.pressed ? Qt.darker(Theme.primaryColor, 1.15) : Theme.primaryColor

        Accessible.role: Accessible.Button
        Accessible.name: TranslationManager.translate("barista.summon", "Coach")
        Accessible.focusable: true
        Accessible.onPressAction: root._orch && root._orch.wake()

        Row {
            id: pillRow
            anchors.centerIn: parent
            spacing: Theme.scaled(6)
            Image {
                source: "qrc:/icons/sparkle.svg"
                width: Theme.scaled(16); height: Theme.scaled(16)
                anchors.verticalCenter: parent.verticalCenter
                visible: status === Image.Ready
                Accessible.ignored: true
            }
            Tr {
                key: "barista.summon"; fallback: "Coach"
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.primaryContrastColor
                font: Theme.bodyFont
                Accessible.ignored: true
            }
        }

        AccessibleMouseArea {
            id: pillArea
            anchors.fill: parent
            accessibleName: TranslationManager.translate("barista.summon", "Coach")
            accessibleItem: pill
            accessibleRole: Accessible.Button
            onAccessibleClicked: if (root._orch) root._orch.wake()
        }
    }

    // ---- Conversation card (active states) ------------------------------------
    Rectangle {
        id: card
        visible: root._state === "confirmBean" || root._state === "proposePlan"
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: Theme.spacingLarge
        width: Math.min(Theme.scaled(520), parent.width - Theme.spacingLarge * 2)
        height: cardColumn.implicitHeight + Theme.spacingLarge * 2
        radius: Theme.cardRadius
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        Accessible.role: Accessible.StaticText
        Accessible.name: headline.text + ". " + subline.text

        ColumnLayout {
            id: cardColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spacingLarge
            spacing: Theme.spacingMedium

            // Header: sparkle + dismiss
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.scaled(6)
                Image {
                    source: "qrc:/icons/sparkle.svg"
                    width: Theme.scaled(16); height: Theme.scaled(16)
                    visible: status === Image.Ready
                    Accessible.ignored: true
                }
                Tr {
                    key: "barista.title"; fallback: "Coach"
                    Layout.fillWidth: true
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
                AccessibleButton {
                    subtle: true
                    text: "×"
                    accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                    onClicked: if (root._orch) root._orch.dismiss()
                }
            }

            // Greeting headline
            Text {
                id: headline
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textColor
                font: Theme.subtitleFont
                text: root._name.length > 0
                      ? TranslationManager.translate("barista.greet.named", "Morning, %1.").arg(root._name)
                      : TranslationManager.translate("barista.greet", "Morning.")
            }

            // Sub-line: the same-bean question, or the (P1b placeholder) plan line
            Text {
                id: subline
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textColor
                font: Theme.bodyFont
                text: {
                    if (root._state === "confirmBean") {
                        return root._bean.length > 0
                            ? TranslationManager.translate("barista.confirmBean.named", "Same %1 as last time?").arg(root._bean)
                            : TranslationManager.translate("barista.confirmBean", "Same beans as last time?")
                    }
                    // proposePlan — real plan lands in P1b
                    return TranslationManager.translate("barista.plan.pending",
                        "Let me look at your recent shots and suggest a plan…")
                }
            }

            // Actions
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                // ConfirmBean actions
                AccessibleButton {
                    primary: true
                    visible: root._state === "confirmBean"
                    text: TranslationManager.translate("barista.sameBean", "Yes, same")
                    accessibleName: TranslationManager.translate("barista.sameBean", "Yes, same")
                    onClicked: if (root._orch) root._orch.confirmSameBean()
                }
                AccessibleButton {
                    subtle: true
                    visible: root._state === "confirmBean"
                    text: TranslationManager.translate("barista.newBean", "New coffee")
                    accessibleName: TranslationManager.translate("barista.newBean", "New coffee")
                    onClicked: if (root._orch) root._orch.chooseNewBean()
                }

                // ProposePlan actions (placeholder until P1b)
                AccessibleButton {
                    primary: true
                    visible: root._state === "proposePlan"
                    text: TranslationManager.translate("common.button.ok", "OK")
                    accessibleName: TranslationManager.translate("common.button.ok", "OK")
                    onClicked: if (root._orch) root._orch.dismiss()
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}
