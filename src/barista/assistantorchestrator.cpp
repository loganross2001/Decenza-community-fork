#include "assistantorchestrator.h"

#include "assistantsettings.h"
#include "../controllers/maincontroller.h"
#include "../history/shothistorystorage.h"

#include <QDateTime>

AssistantOrchestrator::AssistantOrchestrator(MainController* mainController, MachineState* machineState,
                                             AssistantSettings* settings, QObject* parent)
    : QObject(parent)
    , m_mainController(mainController)
    , m_machineState(machineState)
    , m_settings(settings) {
    if (m_mainController && m_mainController->shotHistory()) {
        connect(m_mainController->shotHistory(), &ShotHistoryStorage::shotSaved,
                this, &AssistantOrchestrator::onShotSaved);
    }
}

QString AssistantOrchestrator::stateString() const {
    switch (m_state) {
    case State::Conversing: return QStringLiteral("conversing");
    case State::Present:
    default:                return QStringLiteral("present");
    }
}

void AssistantOrchestrator::engage() {
    // The ONLY way into a live conversation: the user tapped the barista / started talking. No recency or
    // tap guards — the human initiated, so we never refuse. The overlay primes context and waits for the
    // first utterance; recencyBucket() (read at this moment) decides whether a greeting is folded into it.
    if (!m_settings || !m_settings->enabled())
        return;
    setState(State::Conversing);
}

void AssistantOrchestrator::dismiss() {
    setState(State::Present);
}

void AssistantOrchestrator::requestDismiss() {
    // [barista-fork] The end_conversation tool ran (barista dismissing itself on a goodbye). Just emit the
    // signal — the overlay owns the timing, ending the session only AFTER the sign-off finishes speaking so it's
    // never cut off. Wired to fire on the main thread (BaristaModule connects the AIManager seam to this).
    emit dismissRequested();
}

void AssistantOrchestrator::requestOpenBagCamera() {
    // [barista-fork] The open_bag_camera tool ran. Emit on the main thread; the overlay opens BagCameraCapture.
    emit openBagCameraRequested();
}

void AssistantOrchestrator::noteEspressoSelected() {
    // Espresso selected → CONTEXT only. The barista does not speak or change state; a later user-initiated
    // chat will assemble fresh context (current bean/profile) at engage-time. Intentionally a no-op today;
    // it exists so the IdlePage hook has a stable, silent target (and a place for future context warming).
}

void AssistantOrchestrator::onShotSaved(qlonglong shotId) {
    // A shot finished → pure BOOKKEEPING (no conversation, no close-out prompt). Record it as the last shot
    // and mark it UNDISCUSSED, so: (a) the overlay's sessionContext can carry justPulledShot into the next
    // user-initiated chat, and (b) P3 can raise a non-verbal cue (a pulse on the tab) until it's discussed.
    if (shotId <= 0 || !m_settings || !m_settings->enabled())
        return;
    m_lastShotId = shotId;
    m_lastShotAtMs = QDateTime::currentMSecsSinceEpoch();
    emit lastShotChanged();
    if (m_shotDiscussed) {
        m_shotDiscussed = false;
        emit shotDiscussedChanged();
    } else {
        // Already had an undiscussed shot; a second one supersedes it — still undiscussed, just newer.
        emit shotDiscussedChanged();
    }
}

void AssistantOrchestrator::markShotDiscussed() {
    if (m_shotDiscussed)
        return;
    m_shotDiscussed = true;
    emit shotDiscussedChanged();
}

QString AssistantOrchestrator::recencyBucket() const {
    if (!m_settings)
        return QStringLiteral("firstEver");
    const qint64 mins = m_settings->minutesSinceLastExchange();
    if (mins < 0)
        return QStringLiteral("firstEver");
    if (mins < 60)
        return QStringLiteral("ongoing");
    // Same calendar day as the last exchange?
    const QDateTime last = QDateTime::fromString(m_settings->lastExchangeAt(), Qt::ISODate);
    const bool sameDay = last.isValid() && last.date() == QDate::currentDate();
    if (mins < 8 * 60 && sameDay)
        return QStringLiteral("earlierToday");
    return QStringLiteral("firstOfDay");
}

qlonglong AssistantOrchestrator::minutesSinceLastExchange() const {
    return m_settings ? m_settings->minutesSinceLastExchange() : -1;
}

void AssistantOrchestrator::markExchangeCompleted() {
    // Single write path for recency: a real user↔barista exchange completed. The overlay routes every
    // completed reply through here (rather than writing lastExchangeAt itself) so there's one source.
    if (m_settings)
        m_settings->markExchangeCompleted();
}

void AssistantOrchestrator::setState(State s) {
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}
