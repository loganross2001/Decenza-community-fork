#include "maintenancedocsync.h"
#include "tasksstorage.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkCookieJar>
#include <QCryptographicHash>
#include <QDateTime>
#include <QRegularExpression>
#include <QUrl>
#include <QDebug>

// The cleaning-schedule section of Decent's DE1 Quickstart — the exact source kDefaultMaintenance
// (tasksstorage.cpp) was seeded from.
const QString MaintenanceDocSync::kDocUrl = QStringLiteral("https://decentespresso.com/doc/quickstart/");

MaintenanceDocSync::MaintenanceDocSync(TasksStorage* tasks, QObject* parent)
    : QObject(parent)
    , m_tasks(tasks)
    , m_network(new QNetworkAccessManager(this))
{
    // Empty, non-persistent cookie jar so the claim "nothing about the user is sent" is literal — a
    // fresh QNAM would otherwise get a default jar; we install our own so no cookie is ever stored or
    // returned across checks. The GET carries only a static UA header, no query/body.
    m_network->setCookieJar(new QNetworkCookieJar(m_network));
    if (m_tasks) {
        connect(m_tasks, &TasksStorage::maintenanceDocStateReady,
                this, [this](const QVariantMap& state) {
            const bool wasEnabled = m_enabled;
            const qint64 wasChecked = m_lastCheckedAt;
            m_enabled = state.value(QStringLiteral("enabled"), true).toBool();
            m_lastCheckedAt = state.value(QStringLiteral("lastCheckedAt")).toLongLong();
            if (m_enabled != wasEnabled || m_lastCheckedAt != wasChecked)
                emit stateChanged();

            // If a startup check was requested before we knew the state, run it now (once), rate-limited.
            if (m_startupCheckPending) {
                m_startupCheckPending = false;
                if (m_enabled && !m_checking) {
                    const qint64 now = QDateTime::currentSecsSinceEpoch();
                    const qint64 windowSecs = qint64(kMinCheckIntervalDays) * 86400;
                    if (m_lastCheckedAt <= 0 || (now - m_lastCheckedAt) >= windowSecs)
                        issueGet();
                }
            }
        });
    }
}

MaintenanceDocSync::~MaintenanceDocSync()
{
    if (m_reply) {
        m_reply->abort();
        m_reply->deleteLater();
        m_reply = nullptr;
    }
}

void MaintenanceDocSync::refreshStateFromStore()
{
    if (m_tasks)
        m_tasks->requestMaintenanceDocState();
}

void MaintenanceDocSync::maybeCheckOnStartup()
{
    if (!m_tasks)
        return;
    // Defer the decision to when the persisted state arrives (the rate-limit gate reads last_checked_at).
    m_startupCheckPending = true;
    refreshStateFromStore();
}

void MaintenanceDocSync::checkNow()
{
    if (!m_tasks || m_checking)
        return;
    // Owner-initiated: bypass the rate-limit but still respect the toggle (Check-now is disabled in the
    // UI when off; this is a defensive backstop).
    if (!m_enabled)
        return;
    issueGet();
}

void MaintenanceDocSync::setEnabled(bool enabled)
{
    if (!m_tasks)
        return;
    // Persist; the local mirror updates from the maintenanceDocStateReady echo.
    m_tasks->setMaintenanceDocSyncEnabled(enabled);
}

void MaintenanceDocSync::issueGet()
{
    if (m_checking || m_reply)
        return;
    m_checking = true;
    emit checkingChanged();

    const QUrl url(kDocUrl);
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Decenza"));
    // decentespresso.com may redirect (e.g. trailing-slash / https) — follow only no-less-safe hops.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // A stalled connection must not hang the reply forever (inactivity timeout, resets per chunk).
    request.setTransferTimeout(30000);

    m_reply = m_network->get(request);
    connect(m_reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply* r = m_reply;
        onReplyFinished(r);
    });
}

void MaintenanceDocSync::onReplyFinished(QNetworkReply* reply)
{
    if (!reply)
        return;
    m_reply = nullptr;
    m_checking = false;
    emit checkingChanged();

    // Fail silently on any error — we simply try again in the next window (no crash, no user-facing error).
    if (reply->error() != QNetworkReply::NoError) {
        qDebug() << "MaintenanceDocSync: fetch failed (will retry next window):" << reply->errorString();
        reply->deleteLater();
        return;
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();
    if (body.isEmpty()) {
        qDebug() << "MaintenanceDocSync: empty doc body — skipping";
        return;
    }

    const QString normalized = normalizeHtml(QString::fromUtf8(body));
    if (normalized.isEmpty()) {
        qDebug() << "MaintenanceDocSync: normalized doc text empty — skipping";
        return;
    }
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(normalized.toUtf8(), QCryptographicHash::Sha256).toHex());

    // Persist off the main thread via TasksStorage; on completion the state echo refreshes our mirror
    // (last-checked date) AND flips reviewed=0 in the DB when the hash changed, which the AIManager
    // context builder reads to let the barista OFFER the update on its next first-reply.
    if (m_tasks)
        m_tasks->recordFetchedMaintenanceDoc(normalized, hash);
}

QString MaintenanceDocSync::normalizeHtml(const QString& html)
{
    QString s = html;
    // Drop volatile non-content blocks first (scripts/styles/analytics nonces would otherwise churn the
    // hash on every fetch and nag the owner forever). DOTALL so blocks spanning newlines are removed.
    static const QRegularExpression scriptRe(
        QStringLiteral("<script\\b[^>]*>.*?</script>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression styleRe(
        QStringLiteral("<style\\b[^>]*>.*?</style>"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression commentRe(
        QStringLiteral("<!--.*?-->"), QRegularExpression::DotMatchesEverythingOption);
    s.remove(scriptRe);
    s.remove(styleRe);
    s.remove(commentRe);

    // Strip all remaining tags.
    static const QRegularExpression tagRe(QStringLiteral("<[^>]+>"));
    s.replace(tagRe, QStringLiteral(" "));

    // Decode the handful of entities that survive on a plain content page.
    s.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "));
    s.replace(QStringLiteral("&amp;"),  QStringLiteral("&"));
    s.replace(QStringLiteral("&lt;"),   QStringLiteral("<"));
    s.replace(QStringLiteral("&gt;"),   QStringLiteral(">"));
    s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
    s.replace(QStringLiteral("&#39;"),  QStringLiteral("'"));

    // Collapse all whitespace runs to single spaces so trivial reflow doesn't change the hash.
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    s.replace(wsRe, QStringLiteral(" "));
    s = s.trimmed();

    if (s.size() > kMaxDocTextChars)
        s = s.left(kMaxDocTextChars);
    return s;
}
