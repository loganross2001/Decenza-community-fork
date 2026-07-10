#pragma once

#include <QObject>
#include <QString>
#include <QPointer>

class QNetworkAccessManager;
class QNetworkReply;
class TasksStorage;

// [barista-fork] "Periodic Decent maintenance-docs check" — the owner asked the app to occasionally
// re-read Decent's DE1 Quickstart cleaning section (the source the seeded maintenance schedule came
// from) and PROPOSE default-interval updates, never silently change anything (approve-then-apply).
//
// This service is ONLY the network + rate-limit + hash side. The persisted state (toggle,
// last-checked, baseline/current hash, fetched text) lives in TasksStorage's assistant.db
// `maintenance_doc_state` row, and the approve-then-apply write path is a barista client tool
// (update_maintenance_default, guarded to is_default=1 rows). The barista's OFFER of a detected
// change is folded into the AIManager context block (maintenanceDocChanged); this class merely keeps
// that state fresh.
//
// PRIVACY: the only network egress is a single GET to decentespresso.com. Nothing about the user is
// ever sent — no query string, no body, no cookies (this service owns a private QNetworkAccessManager
// with no cookie jar). The periodic check is behind an owner toggle (default ON, in maintenance_doc_state)
// and can be run on demand via checkNow(). On any network error it fails silently and simply retries in
// the next ~30-day window. It never blocks the UI: the GET is async and the DB writes go to
// TasksStorage's background worker.
//
// Rate-limit: on maybeCheckOnStartup() we read the stored last-checked ISO/epoch and skip the fetch if
// it was within kMinCheckIntervalDays. No QTimer — the check is event-driven off the doc-state read
// (CLAUDE.md: timers only for genuinely periodic tasks; a once-per-launch gated check is not one).
class MaintenanceDocSync : public QObject {
    Q_OBJECT
    // Live mirror of the persisted state, so the settings dialog can bind lastChecked / enabled / a
    // "checking now" spinner without a round-trip. Kept in sync from TasksStorage::maintenanceDocStateReady.
    Q_PROPERTY(bool enabled READ enabled NOTIFY stateChanged)
    Q_PROPERTY(qint64 lastCheckedAt READ lastCheckedAt NOTIFY stateChanged)
    Q_PROPERTY(bool checking READ checking NOTIFY checkingChanged)

public:
    // `tasks` is the shared assistant.db store (owned by BaristaModule); borrowed, must outlive this.
    explicit MaintenanceDocSync(TasksStorage* tasks, QObject* parent = nullptr);
    ~MaintenanceDocSync() override;

    bool enabled() const { return m_enabled; }
    qint64 lastCheckedAt() const { return m_lastCheckedAt; }
    bool checking() const { return m_checking; }

    // Kick the once-per-launch, rate-limited check: reads the doc-sync state, and if enabled AND the
    // last check was more than ~30 days ago (or never), issues the GET. Safe to call before the DB is
    // initialized (it no-ops if TasksStorage has no path yet). Non-blocking.
    Q_INVOKABLE void maybeCheckOnStartup();

    // Owner-initiated "Check now" from the settings dialog: fetches regardless of the rate-limit, as
    // long as the toggle is on. Ignored while a check is already in flight.
    Q_INVOKABLE void checkNow();

    // Flip the owner's toggle (persisted via TasksStorage). Mirrors the state locally on confirmation.
    Q_INVOKABLE void setEnabled(bool enabled);

    // The canonical Decent cleaning/quickstart source the seeded schedule came from.
    static const QString kDocUrl;
    // Rate-limit window for the automatic startup check (owner said "I doubt it will be very often").
    static constexpr int kMinCheckIntervalDays = 30;
    // Bound the stored/reasoned-over doc text so a large page can't bloat assistant.db or the context block.
    static constexpr int kMaxDocTextChars = 8000;

    // Reduce raw HTML to normalized visible text: strip <script>/<style> blocks, drop all tags, decode a
    // few common entities, collapse whitespace, cap length. Exposed for unit testing the hash-stability.
    static QString normalizeHtml(const QString& html);

signals:
    void stateChanged();
    void checkingChanged();

private:
    void issueGet();
    void onReplyFinished(QNetworkReply* reply);
    void refreshStateFromStore();

    QPointer<TasksStorage> m_tasks;
    QNetworkAccessManager* m_network = nullptr;   // private QNAM (no cookie jar); child of this
    QNetworkReply* m_reply = nullptr;

    bool m_enabled = true;
    qint64 m_lastCheckedAt = 0;
    bool m_checking = false;
    // Set when maybeCheckOnStartup ran before the state arrived, so the first state read triggers it once.
    bool m_startupCheckPending = false;
};
