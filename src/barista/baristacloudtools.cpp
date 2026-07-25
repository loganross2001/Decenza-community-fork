#include "baristacloudtools.h"

#include "../core/settings.h"
#include "../core/settings_visualizer.h"
#include "../network/beanbaseclient.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QVariant>
#include <memory>

namespace {
const QString kBase = QStringLiteral("https://visualizer.coffee");
QJsonObject errObj(const QString& msg) { return QJsonObject{ {QStringLiteral("error"), msg} }; }
}  // namespace

BaristaCloudTools::BaristaCloudTools(QNetworkAccessManager* net, Settings* appSettings, QObject* parent)
    : QObject(parent),
      m_net(net),
      m_appSettings(appSettings),
      // Own, dedicated BeanBaseClient so a barista lookup never supersedes (or is superseded by) the Beans-page
      // search bar sharing MainController's instance. Keyless; reuses the module's QNAM.
      m_beanBase(new BeanBaseClient(net, appSettings, this))
{}

QString BaristaCloudTools::authHeader() const
{
    if (!m_appSettings || !m_appSettings->visualizer())
        return QString();
    const QString u = m_appSettings->visualizer()->visualizerUsername();
    const QString p = m_appSettings->visualizer()->visualizerPassword();
    if (u.isEmpty() || p.isEmpty())
        return QString();
    return QStringLiteral("Basic ") + QString::fromLatin1((u + QLatin1Char(':') + p).toUtf8().toBase64());
}

QString BaristaCloudTools::shotIdFrom(const QString& idOrUrl)
{
    QString s = idOrUrl.trimmed();
    // A full link like https://visualizer.coffee/shots/<id> — take the last non-empty path segment.
    const qsizetype slash = s.lastIndexOf(QLatin1Char('/'));
    if (slash >= 0)
        s = s.mid(slash + 1);
    return s.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('#'), 0, 0).trimmed();
}

void BaristaCloudTools::getVisualizerShot(const QString& shotIdOrUrl, Done done)
{
    const QString id = shotIdFrom(shotIdOrUrl);
    if (id.isEmpty()) {
        done(errObj(QStringLiteral("no shot id or Visualizer link given")));
        return;
    }
    QUrl url(kBase + QStringLiteral("/api/shots/") + id + QStringLiteral("/download"));
    QNetworkRequest req(url);
    // Auth if we have it (so the user's OWN private shots resolve too); a public/visible shot needs none.
    if (const QString auth = authHeader(); !auth.isEmpty())
        req.setRawHeader("Authorization", auth.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(15000);

    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, id, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int sc = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            done(errObj(QStringLiteral("couldn't fetch Visualizer shot %1 (HTTP %2): %3")
                            .arg(id).arg(sc).arg(reply->errorString())));
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonObject meta = root.value(QStringLiteral("meta")).toObject();

        QJsonObject o;
        o[QStringLiteral("source")] = QStringLiteral("visualizer");
        o[QStringLiteral("visualizerId")] = id;
        o[QStringLiteral("url")] = kBase + QStringLiteral("/shots/") + id;

        const auto copyStr = [&](const char* outKey, const char* inKey) {
            const QString v = meta.value(QLatin1String(inKey)).toString().trimmed();
            if (!v.isEmpty()) o[QLatin1String(outKey)] = v;
        };
        copyStr("beanBrand", "bean_brand");
        copyStr("beanType", "bean_type");
        copyStr("roast", "roast");
        copyStr("grind", "grinder_setting");
        copyStr("barista", "barista");

        const double dose  = meta.value(QStringLiteral("dose")).toVariant().toDouble();
        const double yield = meta.value(QStringLiteral("drink_weight")).toVariant().toDouble();
        if (dose > 0)  o[QStringLiteral("doseG")]  = dose;
        if (yield > 0) o[QStringLiteral("yieldG")] = yield;
        if (dose > 0 && yield > 0)
            o[QStringLiteral("ratio")] = QString::number(yield / dose, 'f', 1).toDouble();

        const QJsonArray elapsed = root.value(QStringLiteral("elapsed")).toArray();
        if (!elapsed.isEmpty())
            o[QStringLiteral("durationSec")] =
                QString::number(elapsed.at(elapsed.size() - 1).toDouble(), 'f', 1).toDouble();

        const QString profileTitle = root.value(QStringLiteral("profile")).toObject()
                                          .value(QStringLiteral("title")).toString().trimmed();
        if (!profileTitle.isEmpty())
            o[QStringLiteral("profile")] = profileTitle;

        const double enj = meta.value(QStringLiteral("espresso_enjoyment")).toVariant().toDouble();
        if (enj > 0) o[QStringLiteral("enjoyment0to100")] = enj;
        // Measured extraction only if THIS shot's owner actually took a refractometer reading (never fabricated).
        const double tds = meta.value(QStringLiteral("drink_tds")).toVariant().toDouble();
        const double ey  = meta.value(QStringLiteral("drink_ey")).toVariant().toDouble();
        if (tds > 0) o[QStringLiteral("measuredTdsPct")] = tds;
        if (ey > 0)  o[QStringLiteral("measuredEyPct")]  = ey;

        o[QStringLiteral("note")] = QStringLiteral(
            "Summary of a Visualizer shot (its dial-in + bean). It may be the user's own or someone else's public "
            "shot; the full trace lives at the url.");
        done(o);
    });
}

void BaristaCloudTools::searchVisualizerShots(const QJsonObject& input, Done done)
{
    const bool wantPublic =
        input.value(QStringLiteral("scope")).toString().trimmed().compare(
            QLatin1String("public"), Qt::CaseInsensitive) == 0;
    int page = input.value(QStringLiteral("page")).toInt(1);
    if (page < 1) page = 1;

    const QString auth = authHeader();
    if (!wantPublic && auth.isEmpty()) {
        done(errObj(QStringLiteral("No Visualizer login is set, so I can't reach your uploaded shots. "
                                   "You can still browse public shots with scope \"public\".")));
        return;
    }

    QUrl url(kBase + QStringLiteral("/api/shots"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("page"), QString::number(page));
    q.addQueryItem(QStringLiteral("items"), QStringLiteral("50"));
    url.setQuery(q);

    QNetworkRequest req(url);
    // Authenticated → the user's own shots; unauthenticated → the public Shot.visible feed.
    if (!wantPublic && !auth.isEmpty())
        req.setRawHeader("Authorization", auth.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(15000);

    QNetworkReply* reply = m_net->get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, wantPublic, page, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            const int sc = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            done(errObj(QStringLiteral("Visualizer shot list failed (HTTP %1): %2")
                            .arg(sc).arg(reply->errorString())));
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray data = root.value(QStringLiteral("data")).toArray();
        QJsonArray shots;
        for (const QJsonValue& v : data) {
            const QJsonObject s = v.toObject();
            const QString sid = s.value(QStringLiteral("id")).toString();
            if (sid.isEmpty())
                continue;
            QJsonObject o{ {QStringLiteral("id"), sid} };
            const qint64 clock = s.value(QStringLiteral("clock")).toVariant().toLongLong();
            if (clock > 0)
                o[QStringLiteral("date")] =
                    QDateTime::fromSecsSinceEpoch(clock).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
            o[QStringLiteral("url")] = kBase + QStringLiteral("/shots/") + sid;
            shots.append(o);
        }
        done(QJsonObject{
            {QStringLiteral("scope"), wantPublic ? QStringLiteral("public") : QStringLiteral("mine")},
            {QStringLiteral("page"), page},
            {QStringLiteral("returnedCount"), shots.size()},
            {QStringLiteral("shots"), shots},
            {QStringLiteral("note"), QStringLiteral(
                "A time-ordered feed of shot ids (newest first). Call get_visualizer_shot on any id to pull its "
                "dial-in + bean. This feed has NO server-side bean/date search — page through it or ask the user "
                "for a specific shot link.")},
        });
    });
}

void BaristaCloudTools::lookUpBean(const QString& query, Done done)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        done(errObj(QStringLiteral("no bean name/query given")));
        return;
    }
    // One-shot listeners on our dedicated client, matched to THIS query, torn down as soon as either fires.
    auto conn = std::make_shared<QMetaObject::Connection>();
    auto connFail = std::make_shared<QMetaObject::Connection>();
    *conn = connect(m_beanBase, &BeanBaseClient::searchResults, this,
        [q, done, conn, connFail](const QString& rq, const QVariantList& entries) {
            if (rq.trimmed().compare(q, Qt::CaseInsensitive) != 0)
                return;
            QObject::disconnect(*conn);
            QObject::disconnect(*connFail);
            QJsonArray arr;
            for (const QVariant& e : entries) {
                if (arr.size() >= 5)
                    break;
                const QVariantMap m = e.toMap();
                QJsonObject o;
                for (const char* k : {"roasterName", "roastName", "degree", "origin", "region", "producer",
                                      "variety", "process", "harvest", "tastingNotes", "elevation", "link"}) {
                    const QString v = m.value(QLatin1String(k)).toString().trimmed();
                    if (!v.isEmpty())
                        o[QLatin1String(k)] = v;
                }
                if (!o.isEmpty())
                    arr.append(o);
            }
            done(QJsonObject{ {QStringLiteral("query"), q},
                              {QStringLiteral("returnedCount"), arr.size()},
                              {QStringLiteral("beans"), arr} });
        });
    *connFail = connect(m_beanBase, &BeanBaseClient::searchFailed, this,
        [q, done, conn, connFail](const QString& rq, const QString& status) {
            if (status == QLatin1String("superseded"))
                return;   // a newer search took over — its own handlers will resolve
            if (rq.trimmed().compare(q, Qt::CaseInsensitive) != 0)
                return;
            QObject::disconnect(*conn);
            QObject::disconnect(*connFail);
            done(errObj(QStringLiteral("bean lookup failed (") + status + QStringLiteral(")")));
        });
    m_beanBase->search(q);
}
