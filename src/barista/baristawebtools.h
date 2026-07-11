#pragma once

#include <QObject>
#include <QJsonValue>
#include <QString>
#include <functional>

class QNetworkAccessManager;

// [barista-fork] FAST-PATH web tools for the barista. Three single-purpose, KEYLESS HTTP GETs that answer the
// most common real-time questions in ~1s instead of routing through the slow Anthropic web-search round-trip
// (~10s):
//   - get_weather(location)      → open-meteo.com (geocode → forecast)
//   - get_stock_quote(symbol)    → query1.finance.yahoo.com
//   - get_local_news(query)      → news.google.com RSS
//
// Each method owns ONE async GET (get_weather chains two: geocode → forecast), applies a short transfer timeout,
// and delivers a parsed QJsonValue result to its `done` callback — on the MAIN thread (the QNAM lives here, so
// its finished() slots already fire on the main thread; no cross-thread marshalling needed). On any
// error/timeout the result is {"error": "..."} so the model can fall back to web search or say it couldn't
// reach the source — the tool NEVER hangs and NEVER crashes.
//
// PRIVACY: each tool contacts ONLY its single host with ONLY the query the user asked about (a city name, a
// ticker symbol, or a news topic/location). No API keys, no user data beyond that one query string. Authorized:
// the owner explicitly requested these three endpoints.
//   get_weather     → geocoding-api.open-meteo.com + api.open-meteo.com
//   get_stock_quote → query1.finance.yahoo.com
//   get_local_news  → news.google.com
class BaristaWebTools : public QObject {
    Q_OBJECT
public:
    // The QNAM is borrowed (owned by the app, shared with the rest of the network layer). Parented for lifetime.
    explicit BaristaWebTools(QNetworkAccessManager* networkManager, QObject* parent = nullptr);

    using Done = std::function<void(QJsonValue)>;

    // Current conditions for a city. Chained GETs: geocode (name→lat/lon) then forecast. Result on success:
    // {location, tempF, feelsLikeF, condition, windMph, humidity}. Empty/unresolvable city → {error}.
    void getWeather(const QString& location, Done done);

    // Latest quote for a ticker (the model supplies the symbol). Result on success:
    // {symbol, price, change, changePercent, currency}. Empty symbol / no data → {error}.
    void getStockQuote(const QString& symbol, Done done);

    // Recent headlines. `query` is the search query the model built (a topic, or "<city> local news"). Result on
    // success: {query, headlines:[{title, source?, published?}, ...]} (top ~5). No results → {error}.
    void getLocalNews(const QString& query, Done done);

private:
    QNetworkAccessManager* m_networkManager = nullptr;
};
