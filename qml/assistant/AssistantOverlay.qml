import QtQuick
import QtQuick.Layouts
import Decenza

// [barista-fork] The proactive barista assistant — a REAL conversation, not a script. When the
// orchestrator activates (Espresso selected → "greeting"; a shot finished → "closeOut") this seeds
// a multi-turn Claude conversation (MainController.aiManager.conversation.ask/followUp) with the
// user's name, bean and best recipe as context, then shows + speaks Claude's replies and sends the
// user's typed replies back. What it says is AI-generated and adapts to the user. Idle-page only.
Item {
    id: root
    anchors.fill: parent

    readonly property var _orch: (typeof Barista !== "undefined") ? Barista.orchestrator : null
    readonly property var _voice: (typeof Barista !== "undefined") ? Barista.voice : null
    readonly property var _settings: (typeof Barista !== "undefined") ? Barista.settings : null
    readonly property string _state: _orch ? _orch.state : "dormant"
    readonly property var _conv: (typeof MainController !== "undefined" && MainController.aiManager)
                                 ? MainController.aiManager.conversation : null

    property string _message: ""       // the assistant's latest line
    property bool _thinking: false
    property bool _showSettings: false
    property bool _awaitingContext: false   // waiting on requestRecentShotContext before ask()
    property bool _closeOutRated: false      // persist the close-out taste to the shot exactly once

    // Who the assistant is talking to: the user's chosen name, else the active barista.
    readonly property string _userName: {
        if (_settings && _settings.userName && _settings.userName.length > 0) return _settings.userName
        if (Settings.dye.dyeBarista && Settings.dye.dyeBarista.length > 0) return Settings.dye.dyeBarista
        return ""
    }
    readonly property string _bean: {
        var s = ((Settings.dye.dyeBeanBrand || "") + " " + (Settings.dye.dyeBeanType || "")).trim()
        return s
    }
    function _partOfDay() {
        var h = new Date().getHours()
        return h < 12 ? "morning" : (h < 18 ? "afternoon" : "evening")
    }

    // Kick off a conversation. First pull the user's REAL dial-in history for this bean so the
    // assistant KNOWS it (and can suggest), instead of asking. ask() fires once the history arrives.
    function _startConversation() {
        if (!root._conv) {
            root._message = TranslationManager.translate("barista.noai",
                "Add an AI key in Settings → AI and I'll be able to chat.")
            return
        }
        root._message = ""
        root._thinking = true
        root._awaitingContext = true
        root._closeOutRated = false
        if (typeof MainController !== "undefined" && MainController.aiManager
                && (Settings.dye.dyeBeanBrand.length > 0 || Settings.dye.dyeBeanType.length > 0)) {
            MainController.aiManager.requestRecentShotContext(
                Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType,
                (typeof ProfileManager !== "undefined") ? ProfileManager.currentProfileName : "", -1)
        } else {
            root._askWithContext("")   // no bean set → converse without history
        }
    }

    // Build the system prompt WITH the real history and open the conversation.
    function _askWithContext(history) {
        root._awaitingContext = false
        if (!root._conv)
            return
        var who = root._settings ? root._settings.assistantName : "Coach"
        var name = root._userName.length > 0 ? root._userName : ""
        var bean = root._bean.length > 0 ? root._bean : "their coffee"
        var hist = (history && history.length > 0) ? history
                 : "(no prior shots recorded on this coffee yet)"
        var sys, seed
        if (root._state === "closeOut") {
            sys = "You are " + who + ", a warm, concise home espresso barista"
                + (name ? " talking to " + name : "") + ".\n"
                + "Their recent history on " + bean + ":\n" + hist + "\n\n"
                + "They just finished a shot. Using the history, comment briefly on how it likely went and offer at most "
                + "one tweak for next time, or ask ONE short question. 1-2 short spoken sentences, conversational."
            seed = "I just pulled a shot."
        } else {
            sys = "You are " + who + ", a warm, concise home espresso barista"
                + (name ? " talking to " + name : "") + ".\n"
                + "Their recent dial-in history on " + bean + ":\n" + hist + "\n\n"
                + "It is the " + root._partOfDay() + " and they are about to pull espresso. Greet them briefly"
                + (name ? " by name" : "") + " and confirm the coffee if useful. You ALREADY KNOW the history above, so do "
                + "NOT ask them to recap past shots — instead proactively suggest ONE specific change for THIS shot based on "
                + "the history (e.g. recent shots ran fast or tasted sour → suggest a finer grind; slow or bitter → coarser). "
                + "Keep every reply to 1-2 short spoken sentences, conversational, and adapt to their answers."
            seed = "Hi — here to make espresso."
        }
        root._conv.ask(sys, seed)
    }

    function _send(text) {
        var t = (text || "").trim()
        if (t.length === 0 || !root._conv || root._thinking)
            return
        // PERSISTENCE: capture the close-out taste feedback onto the shot record, so it's available to
        // the AI on every future call — never starting over or guessing. (Recent-shot context includes it.)
        if (root._state === "closeOut" && !root._closeOutRated && root._orch && root._orch.lastShotId > 0
                && typeof MainController !== "undefined" && MainController.shotHistory) {
            root._closeOutRated = true
            var low = t.toLowerCase()
            var enj = (low.indexOf("sour") >= 0) ? 45
                    : (low.indexOf("bitter") >= 0) ? 55
                    : (low.indexOf("balanc") >= 0 || low.indexOf("good") >= 0 || low.indexOf("great") >= 0
                       || low.indexOf("perfect") >= 0 || low.indexOf("nice") >= 0 || low.indexOf("love") >= 0) ? 82
                    : 0
            var meta = { "espressoNotes": t }
            if (enj > 0)
                meta["enjoyment0to100"] = enj
            MainController.shotHistory.requestUpdateShotMetadata(root._orch.lastShotId, meta)
        }
        root._thinking = true
        root._conv.followUp(t)
    }

    // Activation → start talking (+ bell on the greeting).
    Connections {
        target: root._orch
        ignoreUnknownSignals: true
        function onStateChanged() {
            if (!root._orch) return
            if (root._orch.state === "greeting") {
                if (root._voice) root._voice.playBell()
                root._startConversation()
            } else if (root._orch.state === "closeOut") {
                root._startConversation()
            } else if (root._orch.state === "dormant") {
                root._showSettings = false
            }
        }
    }
    // Claude's replies → show + speak (once each).
    Connections {
        target: root._conv
        ignoreUnknownSignals: true
        function onResponseReceived(response) {
            root._message = response
            root._thinking = false
            if (root._voice) root._voice.speak(response)
        }
    }
    // Rich dial-in history arrived → now open the conversation grounded in it (so the AI KNOWS,
    // rather than asking). Gated on _awaitingContext so we only consume the request we fired.
    Connections {
        target: (typeof MainController !== "undefined") ? MainController.aiManager : null
        ignoreUnknownSignals: true
        function onRecentShotContextReady(context) {
            if (root._awaitingContext)
                root._askWithContext(context)
        }
    }

    // ---- Conversation card (centered, idle page only) --------------------------
    Rectangle {
        id: card
        visible: (root._state === "greeting" || root._state === "closeOut") && !root._showSettings
        anchors.centerIn: parent
        width: Math.min(Theme.scaled(520), parent.width - Theme.spacingLarge * 2)
        height: cardCol.implicitHeight + Theme.spacingLarge * 2
        radius: Theme.cardRadius
        color: Theme.surfaceColor
        border.width: 1
        border.color: Theme.borderColor

        Accessible.role: Accessible.StaticText
        Accessible.name: msgText.text

        ColumnLayout {
            id: cardCol
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: Theme.spacingLarge
            spacing: Theme.spacingMedium

            // Header: name · gear · dismiss
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.scaled(6)
                Text {
                    text: root._settings ? root._settings.assistantName
                                         : TranslationManager.translate("barista.title", "Coach")
                    Layout.fillWidth: true
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
                Item {
                    implicitWidth: Theme.scaled(32); implicitHeight: Theme.scaled(32)
                    Accessible.role: Accessible.Button
                    Accessible.name: TranslationManager.translate("barista.settings.open", "Assistant settings")
                    Accessible.focusable: true
                    Accessible.onPressAction: root._showSettings = true
                    Image {
                        anchors.centerIn: parent
                        source: "qrc:/icons/settings.svg"
                        width: Theme.scaled(20); height: Theme.scaled(20)
                        fillMode: Image.PreserveAspectFit
                        visible: status === Image.Ready
                        Accessible.ignored: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root._showSettings = true
                    }
                }
                AccessibleButton {
                    subtle: true
                    text: "×"
                    accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                    onClicked: if (root._orch) root._orch.dismiss()
                }
            }

            // The assistant's line (or a thinking indicator)
            Text {
                id: msgText
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textColor
                font: Theme.subtitleFont
                text: (root._thinking && root._message.length === 0)
                      ? TranslationManager.translate("barista.thinking", "…")
                      : root._message
            }

            // Reply input (typed; voice input arrives here too in a later phase)
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall
                StyledTextField {
                    id: replyField
                    Layout.fillWidth: true
                    enabled: !root._thinking
                    placeholderText: TranslationManager.translate("barista.reply", "Reply…")
                    onAccepted: { root._send(text); text = "" }
                }
                AccessibleButton {
                    subtle: true
                    enabled: !root._thinking
                    text: TranslationManager.translate("barista.chat.send", "Send")
                    accessibleName: TranslationManager.translate("barista.chat.send", "Send")
                    onClicked: { root._send(replyField.text); replyField.text = "" }
                }
            }
        }
    }

    // Settings panel (name / voice / bell / mute), toggled from the card's gear.
    AssistantSettingsPanel {
        visible: root._showSettings && (root._state === "greeting" || root._state === "closeOut")
        anchors.centerIn: parent
        width: Math.min(Theme.scaled(520), parent.width - Theme.spacingLarge * 2)
        onClosed: root._showSettings = false
    }
}
