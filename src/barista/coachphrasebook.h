#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QHash>

class AIManager;

// [barista-fork] Model-generated VARIED phrasing for the live coaches (pull + steam). The live coaches fire
// cues by id at reflex speed; the owner's no-canned-strings rule means those spoken lines must be
// model-generated + varied, yet a cloud round-trip mid-shot is far too slow. Resolution: ONE bracketing AI
// call (AIManager::requestCoachPhrasebook) returns 4-5 phrasings per cue id + a pre-shot gameplan; the result
// is cached + persisted (assistant.db, rides the KB backup) so live lookup (lineFor) is instant + offline
// after the first refill. Until the first successful refill, lineFor returns the coach's deterministic
// fallback line (the only path a hard-coded string is ever spoken — a one-time cold start).
class CoachPhrasebook : public QObject {
    Q_OBJECT
public:
    explicit CoachPhrasebook(AIManager* ai, QObject* parent = nullptr);

    void initialize(const QString& assistantDbPath);   // create table + load persisted pools

    // Instant, live-safe: a varied line for cueId (rotating, never repeats consecutively), or `fallback`
    // (the coach's deterministic line) when no pool exists yet. Records a diagnostic with the source.
    QString lineFor(const QString& cueId, const QString& fallback);

    QString gameplan() const { return m_gameplan; }
    bool hasGameplan() const { return !m_gameplan.isEmpty(); }
    bool isFresh(const QString& beanId) const;          // pool < 7 days old, same bean, non-empty

    // ONE bracketing AI call to (re)generate pools + gameplan for this bean/context. No-op if already fresh
    // for this bean (unless force) or a request is already in flight. NEVER call during a shot.
    void refresh(const QString& contextBlock, const QString& beanId, bool force = false);

private slots:
    void onReady(const QString& token, const QString& json);
    void onFailed(const QString& token, const QString& error);

private:
    void load();
    void save();

    AIManager* m_ai = nullptr;
    QString m_dbPath;
    QString m_token;                       // in-flight request token ("" = idle)
    QHash<QString, QStringList> m_pools;    // cueId -> variants
    QHash<QString, int> m_lastIdx;          // cueId -> last-picked index (rotate)
    QString m_gameplan;
    qint64 m_generatedAt = 0;               // secs since epoch
    QString m_bean;                         // bean id the current pools were generated for
};
