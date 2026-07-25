#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <functional>

class QNetworkAccessManager;
class Settings;
class BeanBaseClient;

// [barista-fork] The barista's CLOUD data tools — reach the user's espresso data beyond the local DB:
//   get_visualizer_shot     — pull ANY Visualizer shot by id or link (the user's OWN or a public/community
//                             one — /download has no ownership check) and summarize its dial-in + bean.
//   search_visualizer_shots — list the user's OWN uploaded shots (authenticated), or the PUBLIC feed
//                             (Shot.visible, unauthenticated) as ids the barista can then pull in detail.
//   look_up_bean            — Visualizer canonical bean lookup (origin/process/roast/variety/notes), keyless.
//
// All async; each resolves `done` on the MAIN THREAD (QNAM finished() slots fire there — the whole barista
// tool loop runs on the main thread). Every failure returns { error: "..." } so the model degrades gracefully
// rather than hanging. Registered WITH the barista module (pulls QtNetwork) and offered to the model under the
// same internet toggle as the fast-path web tools. Credentials come from the app's stored Visualizer login;
// public reads work with no login at all.
class BaristaCloudTools : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(QJsonValue)>;

    BaristaCloudTools(QNetworkAccessManager* net, Settings* appSettings, QObject* parent = nullptr);

    void getVisualizerShot(const QString& shotIdOrUrl, Done done);
    void searchVisualizerShots(const QJsonObject& input, Done done);
    void lookUpBean(const QString& query, Done done);

private:
    QString authHeader() const;                          // "Basic ..." from stored creds, or "" if unset
    static QString shotIdFrom(const QString& idOrUrl);   // extract the id out of a visualizer.coffee link

    QNetworkAccessManager* m_net = nullptr;
    Settings* m_appSettings = nullptr;
    BeanBaseClient* m_beanBase = nullptr;                // dedicated instance (keyless) to avoid UI contention
};
