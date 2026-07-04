#pragma once

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

class AIManager;
class BeanBaseClient;
class ProfileManager;
class Settings;

// [barista-fork] Coordinates the barista's session context from several sources and emits ONE
// combined block, so the overlay awaits a single ready signal.
//
//   (a) core dial-in context — AIManager::requestBaristaContext (the user's own shots + advice)
//   (b) bean profile — for an UNLINKED bean, a best-effort lookup in the community bean database
//       (Visualizer canonical search via the app's shared BeanBaseClient); a linked bean already
//       carries its blob inside (a), so we skip it there.
//   (c) profile guidance — the app's curated "respected sources" corpus for the current profile.
//
// (b) is async + best-effort; a short timeout guarantees the greeting never hangs on the network —
// we emit with whatever is ready. Everything stays out of the high-churn upstream files.
class BaristaContextBuilder : public QObject {
    Q_OBJECT
public:
    BaristaContextBuilder(AIManager* aiManager, BeanBaseClient* beanBase,
                          ProfileManager* profileManager, Settings* settings,
                          QObject* parent = nullptr);

    // Assemble context for this bean+profile; emits contextReady() exactly once per call.
    Q_INVOKABLE void build(const QString& beanBrand, const QString& beanType, const QString& profileName);

signals:
    void contextReady(const QString& fullBlock);

private:
    void onCoreReady(const QString& coreBlock);
    void onBeanResults(const QString& query, const QVariantList& entries);
    void onBeanFailed(const QString& query, const QString& status);
    QString buildBeanBlock(const QVariantList& entries) const;
    void maybeEmit(bool force = false);

    AIManager* m_aiManager = nullptr;
    BeanBaseClient* m_beanBase = nullptr;
    ProfileManager* m_profileManager = nullptr;
    Settings* m_settings = nullptr;

    bool m_building = false;
    bool m_coreReady = false;
    bool m_beanDone = false;      // bean lookup finished (result, failure, skipped, or timed out)
    QString m_coreBlock;
    QString m_beanBlock;
    QString m_profileBlock;
    QString m_beanQuery;          // the in-flight bean search query (for stale-result discard)
    QTimer m_timeout;
};
