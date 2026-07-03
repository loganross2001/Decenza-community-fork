import QtQuick
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

    // ---- Bean-memory plan (local best recipe for this bean + profile + barista) --------
    // P1b: the instant, offline half of the plan. The Claude plan (structuredNext + provenance)
    // lands into the same card asynchronously in a later increment.
    property var _recipe: ({})
    property bool _recipeLoading: false
    property bool _showSettings: false
    // Smarter plan: Claude's reasoned tweak on top of the local best-recipe (opt-in, on demand).
    property bool _coachThinking: false
    property string _coachText: ""

    function _fetchRecipe() {
        root._recipe = ({})
        root._coachText = ""
        root._coachThinking = false
        if (typeof MainController === "undefined" || !MainController.shotHistory) {
            root._recipeLoading = false
            return
        }
        var kbId = ProfileManager.currentProfileKbId()
        if (kbId.length === 0 || (Settings.dye.dyeBeanBrand.length === 0
                                  && Settings.dye.dyeBeanType.length === 0)) {
            root._recipe = { found: false }
            root._recipeLoading = false
            return
        }
        root._recipeLoading = true
        MainController.shotHistory.requestBeanRecipe(
            Settings.dye.dyeBeanBrand, Settings.dye.dyeBeanType, kbId, Settings.dye.dyeBarista)
    }

    // "grind X  ·  18.0→36.0 g  ·  93.0 °C" — omit fields the best shot didn't carry.
    function _planLine() {
        var r = root._recipe
        if (!r || !r.found) return ""
        var parts = []
        if (r.grinderSetting && String(r.grinderSetting).length > 0)
            parts.push(TranslationManager.translate("barista.plan.grind", "grind") + " " + String(r.grinderSetting))
        if (Number(r.doseG) > 0) {
            var d = Number(r.doseG).toFixed(1)
            parts.push(Number(r.yieldG) > 0 ? (d + "→" + Number(r.yieldG).toFixed(1) + " g") : (d + " g"))
        }
        if (Number(r.temperatureC) > 0)
            parts.push(Number(r.temperatureC).toFixed(1) + " °C")
        return parts.join("  ·  ")
    }

    // Apply the remembered recipe to dial memory (mirrors BrewDialog.applyBeanRecipe; use ?? so a
    // legitimate 0 isn't dropped; temperatureOverride is a property WRITE, not a Q_INVOKABLE).
    function _applyRecipe() {
        var r = root._recipe
        if (!r || !r.found) return
        var grind = r.grinderSetting ?? ""
        if (String(grind).length > 0) Settings.dye.dyeGrinderSetting = grind
        var dose = r.doseG ?? 0
        if (dose > 0) Settings.dye.dyeBeanWeight = dose
        var temp = r.temperatureC ?? 0
        if (temp > 0) Settings.brew.temperatureOverride = temp
    }

    // Typed input → the orchestrator's local matcher (drives the state machine).
    function _send() {
        Qt.inputMethod.commit()   // flush the IME's in-progress word before reading (QML gotcha)
        var t = chatInput.text
        chatInput.text = ""
        if (t && t.length > 0 && root._orch)
            root._orch.handleUtterance(t)
    }

    // Speak the current card (greeting/question, or the plan) via the assistant voice. Deferred
    // via Qt.callLater so the headline/subline bindings settle for the new state before we read them.
    function _speakCard() {
        Qt.callLater(function() {
            if (typeof Barista !== "undefined" && Barista.voice)
                Barista.voice.speak(headline.text + ". " + subline.text)
        })
    }

    // Write the one-tap taste rating to the just-finished shot (close-out), then dismiss.
    // sour/balanced/bitter → enjoyment0to100 (mirrors PostShotReviewPage.enjoymentForTaste).
    function _writeTaste(choice) {
        var id = root._orch ? root._orch.lastShotId : -1
        if (id > 0 && typeof MainController !== "undefined" && MainController.shotHistory) {
            var enj = (choice === "sour") ? 45 : (choice === "balanced") ? 82 : (choice === "bitter") ? 55 : 0
            MainController.shotHistory.requestUpdateShotMetadata(id, { "enjoyment0to100": enj })
        }
        if (root._orch) root._orch.dismiss()
    }

    // Ask Claude for a reasoned tweak on top of the local best-recipe (opt-in, on demand → cost-controlled).
    function _askCoach() {
        if (typeof MainController === "undefined" || !MainController.aiManager
                || !MainController.aiManager.isConfigured)
            return
        root._coachText = ""
        root._coachThinking = true
        var who = (typeof Barista !== "undefined" && Barista.settings) ? Barista.settings.assistantName : "Coach"
        var sys = TranslationManager.translate("barista.coach.system",
            "You are %1, a friendly espresso dial-in coach. Suggest ONE change to try on the next shot to improve it, in a single plain-language sentence, and say what your suggestion is based on. Do not use JSON.").arg(who)
        var bean = root._bean.length > 0 ? root._bean : "this coffee"
        var recipe = (root._recipe && root._recipe.found) ? root._planLine() : "no rated history yet"
        var user = "Bean: " + bean + ". Best recipe so far: " + recipe + ". Suggest one improvement for the next shot."
        MainController.aiManager.analyze(sys, user)
    }

    // Fetch when the orchestrator enters ProposePlan; receive the async result.
    Connections {
        target: root._orch
        ignoreUnknownSignals: true
        function onStateChanged() {
            if (root._orch && root._orch.state === "dormant")
                root._showSettings = false
            if (root._orch && root._orch.state === "proposePlan")
                root._fetchRecipe()          // plan is spoken from onBeanRecipeReady once it resolves
            else if (root._orch && (root._orch.state === "confirmBean" || root._orch.state === "closeOut"))
                root._speakCard()            // greeting/"same bean?" or the taste question
        }
        function onApplyRequested() {   // typed/spoken "apply" in ProposePlan
            root._applyRecipe()
            if (root._orch) root._orch.dismiss()
        }
    }
    Connections {
        target: (typeof MainController !== "undefined") ? MainController.shotHistory : null
        ignoreUnknownSignals: true
        function onBeanRecipeReady(recipe) {
            root._recipe = recipe
            root._recipeLoading = false
            root._speakCard()
        }
    }
    Connections {
        target: (typeof MainController !== "undefined") ? MainController.aiManager : null
        ignoreUnknownSignals: true
        function onRecommendationReceived(text) {
            if (!root._coachThinking) return   // only consume the request we fired
            root._coachThinking = false
            root._coachText = text
            if (typeof Barista !== "undefined" && Barista.voice)
                Barista.voice.speak(text)
        }
    }

    // ---- Conversation card (active states) ------------------------------------
    Rectangle {
        id: card
        visible: (root._state === "confirmBean" || root._state === "proposePlan" || root._state === "closeOut") && !root._showSettings
        anchors.centerIn: parent
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
                Text {
                    text: (typeof Barista !== "undefined" && Barista.settings)
                          ? Barista.settings.assistantName
                          : TranslationManager.translate("barista.title", "Coach")
                    Layout.fillWidth: true
                    color: Theme.textSecondaryColor
                    font: Theme.labelFont
                    Accessible.ignored: true
                }
                // Gear → assistant settings (name / voice / mute)
                Item {
                    implicitWidth: Theme.scaled(28); implicitHeight: Theme.scaled(28)
                    Image {
                        anchors.centerIn: parent
                        source: "qrc:/icons/settings.svg"
                        width: Theme.scaled(18); height: Theme.scaled(18)
                        fillMode: Image.PreserveAspectFit
                        visible: status === Image.Ready
                        Accessible.ignored: true
                    }
                    AccessibleMouseArea {
                        anchors.fill: parent
                        accessibleName: TranslationManager.translate("barista.settings.open", "Assistant settings")
                        accessibleRole: Accessible.Button
                        onAccessibleClicked: root._showSettings = true
                    }
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
                text: {
                    if (root._state === "closeOut")
                        return TranslationManager.translate("barista.closeout.q", "How was that shot?")
                    // Time-aware greeting: "Good morning/afternoon/evening[, name]."
                    var h = new Date().getHours()
                    var greet = h < 12 ? TranslationManager.translate("barista.greet.morning", "Good morning")
                              : h < 18 ? TranslationManager.translate("barista.greet.afternoon", "Good afternoon")
                              : TranslationManager.translate("barista.greet.evening", "Good evening")
                    return root._name.length > 0
                        ? TranslationManager.translate("barista.greet.named", "%1, %2.").arg(greet).arg(root._name)
                        : greet + "."
                }
            }

            // Sub-line: the same-bean question, or the (P1b placeholder) plan line
            Text {
                id: subline
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textColor
                font: Theme.bodyFont
                text: {
                    if (root._state === "closeOut")
                        return TranslationManager.translate("barista.closeout.hint",
                            "Sour, balanced, or bitter — I'll remember for next time.")
                    if (root._state === "confirmBean") {
                        return root._bean.length > 0
                            ? TranslationManager.translate("barista.confirmBean.named", "Same %1 as last time?").arg(root._bean)
                            : TranslationManager.translate("barista.confirmBean", "Same beans as last time?")
                    }
                    // proposePlan
                    if (root._recipeLoading)
                        return TranslationManager.translate("barista.plan.loading",
                            "Looking at your best shots on this coffee…")
                    if (root._recipe && root._recipe.found) {
                        var when = root._recipe.whenLabel ? (" (" + root._recipe.whenLabel + ")") : ""
                        return TranslationManager.translate("barista.plan.best", "Your best pull: %1%2.")
                               .arg(root._planLine()).arg(when)
                    }
                    return TranslationManager.translate("barista.plan.none",
                        "No rated shots on this coffee yet — pull one and rate it, and I'll remember your best.")
                }
            }

            // Coach's reasoned take (async Claude, opt-in — the "smarter plan")
            Text {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                color: Theme.textSecondaryColor
                font: Theme.labelFont
                visible: root._state === "proposePlan" && (root._coachThinking || root._coachText.length > 0)
                text: root._coachThinking
                      ? TranslationManager.translate("barista.coach.thinking", "Thinking…")
                      : root._coachText
                Accessible.ignored: true
            }

            // Actions
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall

                // CloseOut taste buttons (sour / balanced / bitter)
                AccessibleButton {
                    primary: true
                    visible: root._state === "closeOut"
                    text: TranslationManager.translate("barista.taste.sour", "Sour")
                    accessibleName: TranslationManager.translate("barista.taste.sour", "Sour")
                    onClicked: root._writeTaste("sour")
                }
                AccessibleButton {
                    primary: true
                    visible: root._state === "closeOut"
                    text: TranslationManager.translate("barista.taste.balanced", "Balanced")
                    accessibleName: TranslationManager.translate("barista.taste.balanced", "Balanced")
                    onClicked: root._writeTaste("balanced")
                }
                AccessibleButton {
                    primary: true
                    visible: root._state === "closeOut"
                    text: TranslationManager.translate("barista.taste.bitter", "Bitter")
                    accessibleName: TranslationManager.translate("barista.taste.bitter", "Bitter")
                    onClicked: root._writeTaste("bitter")
                }

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

                // ProposePlan actions
                AccessibleButton {
                    primary: true
                    visible: root._state === "proposePlan" && root._recipe && root._recipe.found
                    text: TranslationManager.translate("barista.plan.apply", "Apply")
                    accessibleName: TranslationManager.translate("barista.plan.applyAccessible",
                        "Apply the remembered recipe to your next shot")
                    onClicked: { root._applyRecipe(); if (root._orch) root._orch.dismiss() }
                }
                AccessibleButton {
                    subtle: true
                    visible: root._state === "proposePlan" && !root._coachText && !root._coachThinking
                             && typeof MainController !== "undefined" && MainController.aiManager
                             && MainController.aiManager.isConfigured
                    text: TranslationManager.translate("barista.coach.ask", "Ask %1")
                          .arg((typeof Barista !== "undefined" && Barista.settings) ? Barista.settings.assistantName : "Coach")
                    accessibleName: TranslationManager.translate("barista.coach.askAccessible", "Ask the coach for a suggestion")
                    onClicked: root._askCoach()
                }
                AccessibleButton {
                    subtle: true
                    visible: root._state === "proposePlan"
                    text: (root._recipe && root._recipe.found)
                          ? TranslationManager.translate("barista.plan.notNow", "Not now")
                          : TranslationManager.translate("common.button.ok", "OK")
                    accessibleName: TranslationManager.translate("common.accessibility.dismissDialog", "Dismiss")
                    onClicked: if (root._orch) root._orch.dismiss()
                }

                Item { Layout.fillWidth: true }
            }

            // Typed input — drive the flow by typing (P1c). Enter or Send → handleUtterance.
            RowLayout {
                Layout.fillWidth: true
                spacing: Theme.spacingSmall
                StyledTextField {
                    id: chatInput
                    Layout.fillWidth: true
                    placeholderText: TranslationManager.translate("barista.chat.placeholder", "Type a reply…")
                    onAccepted: root._send()
                }
                AccessibleButton {
                    subtle: true
                    text: TranslationManager.translate("barista.chat.send", "Send")
                    accessibleName: TranslationManager.translate("barista.chat.send", "Send")
                    onClicked: root._send()
                }
            }
        }
    }

    // Settings panel (name / voice / mute), toggled from the card's gear.
    AssistantSettingsPanel {
        visible: root._showSettings && (root._state === "confirmBean" || root._state === "proposePlan")
        anchors.centerIn: parent
        width: Math.min(Theme.scaled(520), parent.width - Theme.spacingLarge * 2)
        onClosed: root._showSettings = false
    }
}
