#include "baristacontextbuilder.h"

#include "../ai/aimanager.h"
#include "../network/beanbaseclient.h"
#include "../controllers/profilemanager.h"
#include "../core/settings.h"
#include "../core/settings_dye.h"

#include <QVariantMap>

BaristaContextBuilder::BaristaContextBuilder(AIManager* aiManager, BeanBaseClient* beanBase,
                                             ProfileManager* profileManager, Settings* settings,
                                             QObject* parent)
    : QObject(parent)
    , m_aiManager(aiManager)
    , m_beanBase(beanBase)
    , m_profileManager(profileManager)
    , m_settings(settings) {
    m_timeout.setSingleShot(true);
    m_timeout.setInterval(4000);   // never let a slow/absent network hang the greeting
    connect(&m_timeout, &QTimer::timeout, this, [this] { maybeEmit(true); });

    if (m_aiManager)
        connect(m_aiManager, &AIManager::baristaContextReady, this, &BaristaContextBuilder::onCoreReady);
    if (m_beanBase) {
        connect(m_beanBase, &BeanBaseClient::searchResults, this, &BaristaContextBuilder::onBeanResults);
        connect(m_beanBase, &BeanBaseClient::searchFailed, this, &BaristaContextBuilder::onBeanFailed);
    }
}

void BaristaContextBuilder::build(const QString& beanBrand, const QString& beanType, const QString& profileName) {
    m_building = true;
    m_coreReady = false;
    m_beanDone = false;
    m_coreBlock.clear();
    m_beanBlock.clear();
    m_beanQuery.clear();

    // (c) profile guidance — synchronous, from the curated corpus.
    m_profileBlock.clear();
    if (m_profileManager) {
        const QString title = !profileName.isEmpty() ? profileName : m_profileManager->currentProfileName();
        const QString kb = m_profileManager->profileKnowledgeContent(title).trimmed();
        if (!kb.isEmpty())
            m_profileBlock = QStringLiteral("\n\n## Profile guidance (respected sources)\n") + kb;
    }

    // (a) core dial-in context.
    if (m_aiManager)
        m_aiManager->requestBaristaContext(beanBrand, beanType, profileName);
    else
        m_coreReady = true;

    // (b) bean profile — only when the bean is UNLINKED (a linked bean's blob is already in (a)).
    bool doBean = false;
    if (m_beanBase && m_settings && m_settings->dye()) {
        const bool linked = !m_settings->dye()->dyeBeanBaseId().isEmpty()
                         || !m_settings->dye()->dyeBeanBaseData().isEmpty();
        const QString name = (beanBrand + QLatin1Char(' ') + beanType).trimmed();
        if (!linked && !name.isEmpty()) {
            m_beanQuery = name;
            doBean = true;
            m_beanBase->search(name);
        }
    }
    if (!doBean)
        m_beanDone = true;

    m_timeout.start();
    maybeEmit();   // in case everything resolved synchronously
}

void BaristaContextBuilder::onCoreReady(const QString& coreBlock) {
    if (!m_building)
        return;
    m_coreBlock = coreBlock;
    m_coreReady = true;
    maybeEmit();
}

void BaristaContextBuilder::onBeanResults(const QString& query, const QVariantList& entries) {
    if (!m_building || query != m_beanQuery)
        return;   // not our search (the client is shared with the Beans page)
    m_beanBlock = buildBeanBlock(entries);
    m_beanDone = true;
    maybeEmit();
}

void BaristaContextBuilder::onBeanFailed(const QString& query, const QString&) {
    if (!m_building || query != m_beanQuery)
        return;
    m_beanDone = true;   // best-effort — proceed without the bean block
    maybeEmit();
}

QString BaristaContextBuilder::buildBeanBlock(const QVariantList& entries) const {
    if (entries.isEmpty())
        return QString();
    const QVariantMap e = entries.first().toMap();
    QString body;
    const auto add = [&](const char* label, const char* key) {
        const QString v = e.value(QString::fromLatin1(key)).toString().trimmed();
        if (!v.isEmpty())
            body += QStringLiteral("- ") + QString::fromLatin1(label) + QStringLiteral(": ") + v + QLatin1Char('\n');
    };
    add("Roaster", "roasterName");
    add("Roast", "roastName");
    add("Origin", "origin");
    add("Region", "region");
    add("Producer", "producer");
    add("Variety", "variety");
    add("Process", "process");
    add("Roast degree", "degree");
    add("Elevation", "elevation");
    add("Harvest", "harvest");
    add("Tasting notes", "tastingNotes");
    if (body.isEmpty())
        return QString();
    return QStringLiteral("\n\n## Bean profile (community database — unconfirmed match)\n")
         + QStringLiteral("Closest match in the community bean database for this bean; treat as helpful "
                          "background, not confirmed fact:\n")
         + body;
}

void BaristaContextBuilder::maybeEmit(bool force) {
    if (!m_building)
        return;
    if (!force && !(m_coreReady && m_beanDone))
        return;
    m_building = false;
    m_timeout.stop();
    QString full = m_coreBlock;
    full += m_beanBlock;     // empty when no match / skipped
    full += m_profileBlock;  // empty when no curated guidance
    emit contextReady(full);
}
