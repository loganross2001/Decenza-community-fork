import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import Decenza

// [barista-fork] Proactive coaching card, extracted verbatim from PostShotReviewPage.qml so that
// page's footprint stays small (merge hygiene for the private fork). Behaviour is unchanged.
//
// Coupling is explicit: the host page is passed in as `page` (read-only state + method calls),
// and the "Why?" action is surfaced as a signal the page wires to its own conversationOverlay.
// Loaded by URL from the page via a Loader, so no import of this module's qrc is needed there.
Rectangle {
    id: root

    // The host PostShotReviewPage. Read-only: the card reads its coaching state
    // (coachingAnalyzing / coachingResult / coachingResponded / editShotData) and calls its
    // methods (resetAutoCloseTimer / coachThisShot / applyCoachingRecommendation).
    property var page

    // Emitted when the user taps "Why?" — the page opens its conversation overlay.
    signal requestDiscussion()

    implicitHeight: coachingCardColumn.implicitHeight + Theme.spacingMedium * 2
    radius: Theme.cardRadius
    color: Theme.surfaceColor
    border.width: 1
    border.color: Theme.borderColor

    // State machine:
    //   notConfigured: AI not set up
    //   analyzing:     our request is in flight
    //   result:        structuredNext recommendation present
    //   quiet:         got a response, no concrete change
    //   idle:          nothing fetched yet
    readonly property string cardState: {
        if (!page) return "idle"   // Loader sets `page` in onLoaded; bindings evaluate first
        if (!MainController.aiManager || !MainController.aiManager.isConfigured)
            return "notConfigured"
        if (page.coachingAnalyzing) return "analyzing"
        if (page.coachingResult) return "result"
        if (page.coachingResponded) return "quiet"
        return "idle"
    }

    Accessible.role: Accessible.StaticText
    Accessible.name: coachingHeadline.text

    ColumnLayout {
        id: coachingCardColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: Theme.spacingMedium
        spacing: Theme.spacingSmall

        // Header row: sparkle + title
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.scaled(6)

            Image {
                source: "qrc:/icons/sparkle.svg"
                width: Theme.scaled(16)
                height: Theme.scaled(16)
                visible: status === Image.Ready
                Accessible.ignored: true
                layer.enabled: true
                layer.smooth: true
                layer.effect: MultiEffect {
                    colorization: 1.0
                    colorizationColor: Theme.textSecondaryColor
                }
            }

            Tr {
                key: "postshotreview.coach.title"
                fallback: "Coaching"
                color: Theme.textSecondaryColor
                font: Theme.labelFont
                Layout.fillWidth: true
                Accessible.ignored: true
            }
        }

        // Headline / status line
        Text {
            id: coachingHeadline
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textColor
            font: Theme.bodyFont
            text: {
                switch (root.cardState) {
                case "notConfigured":
                    return TranslationManager.translate("postshotreview.coach.notConfigured",
                        "Set up AI in Settings to get coaching.")
                case "analyzing":
                    return TranslationManager.translate("postshotreview.coach.analyzing",
                        "Analyzing this shot…")
                case "result":
                    return (page.coachingResult.reasoning
                            && String(page.coachingResult.reasoning).length > 0)
                        ? String(page.coachingResult.reasoning)
                        : TranslationManager.translate("postshotreview.coach.haveSuggestion",
                            "Here's a suggestion for your next shot.")
                case "quiet":
                    return TranslationManager.translate("postshotreview.coach.onTrack",
                        "Looks on track — nothing to change.")
                default:
                    return TranslationManager.translate("postshotreview.coach.idlePrompt",
                        "Tap a taste above, or coach this shot.")
                }
            }
        }

        // Concrete change line (result state only)
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textColor
            font: Theme.subtitleFont
            visible: root.cardState === "result" && text.length > 0
            text: {
                if (!page) return ""
                var r = page.coachingResult
                if (!r) return ""
                var parts = []
                // Grind: show CURRENT → TARGET when we know the reviewed shot's
                // current grind, else fall back to the bare target.
                if (r.grinderSetting !== undefined && String(r.grinderSetting).length > 0) {
                    var curGrind = String(page.editShotData.grinderSetting || "")
                    if (curGrind.length > 0)
                        parts.push(TranslationManager.translate("postshotreview.coach.grindLabel", "Grind")
                                   + " " + curGrind + " → " + String(r.grinderSetting))
                    else
                        parts.push(TranslationManager.translate("postshotreview.coach.grindTo", "Grind to")
                                   + " " + String(r.grinderSetting))
                }
                // Dose: CURRENT → TARGET g, falling back to target.
                if (r.doseG !== undefined && Number(r.doseG) > 0) {
                    var curDose = Number(page.editShotData.doseWeightG)
                    if (curDose > 0)
                        parts.push(TranslationManager.translate("postshotreview.coach.doseLabel", "Dose")
                                   + " " + curDose + " → " + Number(r.doseG) + " "
                                   + TranslationManager.translate("postshotreview.coach.gramsUnit", "g"))
                    else
                        parts.push(TranslationManager.translate("postshotreview.coach.doseTo", "Dose to")
                                   + " " + Number(r.doseG) + " "
                                   + TranslationManager.translate("postshotreview.coach.gramsUnit", "g"))
                }
                // Temperature: CURRENT → TARGET °C with signed delta.
                // Effective current temp = shot override if set, else the profile's target.
                if (r.temperatureC !== undefined && Number(r.temperatureC) > 0) {
                    var target = Number(r.temperatureC)
                    var override = Number(page.editShotData.temperatureOverrideC)
                    var curTemp = (override > 0) ? override : Number(ProfileManager.profileTargetTemperature)
                    var delta = target - curTemp
                    var deltaStr = (delta >= 0 ? "+" : "") + (Math.round(delta * 10) / 10)
                    parts.push(TranslationManager.translate("postshotreview.coach.tempLabel", "Temperature")
                               + " " + (Math.round(curTemp * 10) / 10) + " → "
                               + (Math.round(target * 10) / 10) + " °C  ("
                               + deltaStr + "°)")
                }
                if (r.profileTitle !== undefined && String(r.profileTitle).length > 0)
                    parts.push(TranslationManager.translate("postshotreview.coach.tryProfile", "Try the")
                               + " " + String(r.profileTitle) + " "
                               + TranslationManager.translate("postshotreview.coach.profileWord", "profile"))
                return parts.join("  ·  ")
            }
            Accessible.ignored: true
        }

        // Expected duration window (result state, optional)
        Text {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: Theme.textSecondaryColor
            font: Theme.labelFont
            visible: root.cardState === "result" && text.length > 0
            text: {
                if (!page) return ""
                var r = page.coachingResult
                if (!r || !r.expectedDurationSec) return ""
                var d = r.expectedDurationSec
                if (d.length !== 2) return ""
                return TranslationManager.translate("postshotreview.coach.expectDuration", "Expect")
                       + " " + d[0] + "–" + d[1] + "s"
            }
            Accessible.ignored: true
        }

        // Action row
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            // Coach this shot (idle / quiet states)
            AccessibleButton {
                primary: true
                visible: root.cardState === "idle" || root.cardState === "quiet"
                enabled: MainController.aiManager
                         && MainController.aiManager.isConfigured
                         && !(MainController.aiManager.conversation
                              && MainController.aiManager.conversation.busy)
                text: TranslationManager.translate("postshotreview.coach.coachThisShot", "Coach this shot")
                accessibleName: TranslationManager.translate("postshotreview.coach.coachThisShot", "Coach this shot")
                onClicked: {
                    page.resetAutoCloseTimer()
                    page.coachThisShot()
                }
            }

            // Apply (result state)
            AccessibleButton {
                primary: true
                visible: root.cardState === "result"
                text: TranslationManager.translate("postshotreview.coach.apply", "Apply")
                accessibleName: TranslationManager.translate("postshotreview.coach.applyAccessible",
                    "Apply the suggested change to your next shot")
                onClicked: {
                    page.resetAutoCloseTimer()
                    page.applyCoachingRecommendation()
                }
            }

            // Why? (result / quiet states) — opens the full conversation (handled by the page)
            AccessibleButton {
                subtle: true
                visible: root.cardState === "result" || root.cardState === "quiet"
                text: TranslationManager.translate("postshotreview.coach.why", "Why?")
                accessibleName: TranslationManager.translate("postshotreview.coach.whyAccessible",
                    "Open the full coaching conversation")
                onClicked: {
                    page.resetAutoCloseTimer()
                    root.requestDiscussion()
                }
            }

            Item { Layout.fillWidth: true }

            // Busy spinner for our own request
            BusyIndicator {
                running: root.cardState === "analyzing"
                visible: running
                implicitWidth: Theme.scaled(24)
                implicitHeight: Theme.scaled(24)
                Accessible.ignored: true
            }
        }

        // "Coach automatically after each shot" toggle
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacingSmall

            Switch {
                id: autoCoachSwitch
                checked: Settings.app.coachAfterEachShot
                onToggled: Settings.app.coachAfterEachShot = checked
                Accessible.role: Accessible.CheckBox
                Accessible.name: trAutoCoachLabel.text
                Accessible.checked: checked
                Accessible.focusable: true
                Accessible.onToggleAction: toggle()
            }

            Tr {
                id: trAutoCoachLabel
                key: "postshotreview.coach.autoToggle"
                fallback: "Coach automatically after each shot"
                color: Theme.textSecondaryColor
                font: Theme.labelFont
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                Accessible.ignored: true
            }
        }
    }
}
