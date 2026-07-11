#include "baristawebtools.h"

#include "../weather/weathermanager.h"   // reuse WeatherManager::weatherDescription(int wmoCode) — WMO→text

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <QString>
#include <cmath>

// [barista-fork] Short per-request transfer timeout so a slow/unreachable host can never make the barista hang
// mid "thinking" — the reply aborts and the finished() slot surfaces {error}. 8s is snappy but tolerant of a
// cold DNS + TLS handshake on a tablet.
static constexpr int TRANSFER_TIMEOUT_MS = 8000;

// [barista-fork] A plain, honest User-Agent. Yahoo's finance chart endpoint and Google News RSS both reject or
// stall requests with no UA; open-meteo is happy either way, so we send it on all three for consistency. It
// identifies the app and carries no user data.
static const QByteArray kUserAgent = QByteArrayLiteral("Decenza/1.0 (github.com/Kulitorum/Decenza)");

static QNetworkRequest makeRequest(const QUrl& url)
{
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    req.setTransferTimeout(TRANSFER_TIMEOUT_MS);
    return req;
}

// Uniform error result the model can act on (fall back to web search / tell the user it couldn't reach it).
static QJsonObject errorResult(const QString& message)
{
    return QJsonObject{{QStringLiteral("error"), message}};
}

BaristaWebTools::BaristaWebTools(QNetworkAccessManager* networkManager, QObject* parent)
    : QObject(parent)
    , m_networkManager(networkManager)
{
    Q_ASSERT(networkManager);
}

// ─── get_weather ─────────────────────────────────────────────────────────────
// Two chained keyless GETs: geocode the city name → lat/lon, then pull current conditions. Kept snappy by using
// count=1 on the geocode and only the `current=` fields we report. Uses `this` as the connect context so a
// mid-flight destroy of the service can't fire the lambda on a dead object.

void BaristaWebTools::getWeather(const QString& location, Done done)
{
    const QString city = location.trimmed();
    if (city.isEmpty()) {
        done(errorResult(QStringLiteral("no location — ask the user which city")));
        return;
    }

    QUrl geoUrl(QStringLiteral("https://geocoding-api.open-meteo.com/v1/search"));
    QUrlQuery geoQuery;
    geoQuery.addQueryItem(QStringLiteral("name"), city);
    geoQuery.addQueryItem(QStringLiteral("count"), QStringLiteral("1"));
    geoUrl.setQuery(geoQuery);

    QNetworkReply* geoReply = m_networkManager->get(makeRequest(geoUrl));
    connect(geoReply, &QNetworkReply::finished, this, [this, geoReply, city, done]() {
        geoReply->deleteLater();
        if (geoReply->error() != QNetworkReply::NoError) {
            done(errorResult(QStringLiteral("weather lookup failed: ") + geoReply->errorString()));
            return;
        }
        const QJsonObject geo = QJsonDocument::fromJson(geoReply->readAll()).object();
        const QJsonArray results = geo.value(QStringLiteral("results")).toArray();
        if (results.isEmpty()) {
            done(errorResult(QStringLiteral("couldn't find a city named \"") + city + QStringLiteral("\"")));
            return;
        }
        const QJsonObject place = results.first().toObject();
        const double lat = place.value(QStringLiteral("latitude")).toDouble();
        const double lon = place.value(QStringLiteral("longitude")).toDouble();
        // Build a friendly resolved name: "City, Admin1, Country" (skip empty parts).
        QStringList nameParts;
        for (const char* k : {"name", "admin1", "country"}) {
            const QString v = place.value(QLatin1String(k)).toString().trimmed();
            if (!v.isEmpty() && !nameParts.contains(v)) nameParts << v;
        }
        const QString resolvedName = nameParts.join(QStringLiteral(", "));

        QUrl fcUrl(QStringLiteral("https://api.open-meteo.com/v1/forecast"));
        QUrlQuery fcQuery;
        fcQuery.addQueryItem(QStringLiteral("latitude"),  QString::number(lat, 'f', 4));
        fcQuery.addQueryItem(QStringLiteral("longitude"), QString::number(lon, 'f', 4));
        fcQuery.addQueryItem(QStringLiteral("current"),
            QStringLiteral("temperature_2m,apparent_temperature,weather_code,wind_speed_10m,relative_humidity_2m"));
        // [barista-fork] Also pull a short DAILY forecast so "what's the forecast / this weekend?" is answerable
        // from this fast tool (previously only `current` was fetched, so forecast questions fell through).
        fcQuery.addQueryItem(QStringLiteral("daily"),
            QStringLiteral("temperature_2m_max,temperature_2m_min,weather_code,precipitation_probability_max"));
        fcQuery.addQueryItem(QStringLiteral("forecast_days"), QStringLiteral("4"));
        fcQuery.addQueryItem(QStringLiteral("timezone"),      QStringLiteral("auto"));
        fcQuery.addQueryItem(QStringLiteral("temperature_unit"), QStringLiteral("fahrenheit"));
        fcQuery.addQueryItem(QStringLiteral("wind_speed_unit"),  QStringLiteral("mph"));
        fcUrl.setQuery(fcQuery);

        QNetworkReply* fcReply = m_networkManager->get(makeRequest(fcUrl));
        connect(fcReply, &QNetworkReply::finished, this, [fcReply, resolvedName, done]() {
            fcReply->deleteLater();
            if (fcReply->error() != QNetworkReply::NoError) {
                done(errorResult(QStringLiteral("weather forecast failed: ") + fcReply->errorString()));
                return;
            }
            const QJsonObject root = QJsonDocument::fromJson(fcReply->readAll()).object();
            const QJsonObject cur = root.value(QStringLiteral("current")).toObject();
            if (cur.isEmpty()) {
                done(errorResult(QStringLiteral("weather source returned no current conditions")));
                return;
            }
            const int wmo = cur.value(QStringLiteral("weather_code")).toInt();
            QJsonObject out;
            out[QStringLiteral("location")]   = resolvedName;
            out[QStringLiteral("tempF")]      = static_cast<int>(std::lround(cur.value(QStringLiteral("temperature_2m")).toDouble()));
            out[QStringLiteral("feelsLikeF")] = static_cast<int>(std::lround(cur.value(QStringLiteral("apparent_temperature")).toDouble()));
            // Reuse the app's canonical WMO→text mapping instead of a parallel table.
            out[QStringLiteral("condition")]  = WeatherManager::weatherDescription(wmo);
            out[QStringLiteral("windMph")]    = static_cast<int>(std::lround(cur.value(QStringLiteral("wind_speed_10m")).toDouble()));
            out[QStringLiteral("humidity")]   = cur.value(QStringLiteral("relative_humidity_2m")).toInt();
            // [barista-fork] Daily forecast rows (parallel arrays from open-meteo) → a compact forecast list the
            // model can read for "what's the forecast / how's the weekend looking?".
            const QJsonObject daily = root.value(QStringLiteral("daily")).toObject();
            const QJsonArray days = daily.value(QStringLiteral("time")).toArray();
            const QJsonArray hi   = daily.value(QStringLiteral("temperature_2m_max")).toArray();
            const QJsonArray lo   = daily.value(QStringLiteral("temperature_2m_min")).toArray();
            const QJsonArray code = daily.value(QStringLiteral("weather_code")).toArray();
            const QJsonArray pop  = daily.value(QStringLiteral("precipitation_probability_max")).toArray();
            QJsonArray forecast;
            for (int i = 0; i < days.size(); ++i) {
                QJsonObject d;
                d[QStringLiteral("date")]      = days.at(i).toString();
                d[QStringLiteral("highF")]     = static_cast<int>(std::lround(hi.at(i).toDouble()));
                d[QStringLiteral("lowF")]      = static_cast<int>(std::lround(lo.at(i).toDouble()));
                d[QStringLiteral("condition")] = WeatherManager::weatherDescription(code.at(i).toInt());
                if (i < pop.size() && !pop.at(i).isNull())
                    d[QStringLiteral("precipChancePct")] = pop.at(i).toInt();
                forecast.append(d);
            }
            if (!forecast.isEmpty())
                out[QStringLiteral("forecast")] = forecast;
            done(out);
        });
    });
}

// ─── get_stock_quote ─────────────────────────────────────────────────────────
// One keyless GET to Yahoo Finance's chart endpoint; we read the `meta` block (no need to parse the time series).

void BaristaWebTools::getStockQuote(const QString& symbol, Done done)
{
    const QString sym = symbol.trimmed().toUpper();
    if (sym.isEmpty()) {
        done(errorResult(QStringLiteral("no ticker symbol given")));
        return;
    }

    QUrl url(QStringLiteral("https://query1.finance.yahoo.com/v8/finance/chart/") + QString::fromUtf8(QUrl::toPercentEncoding(sym)));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("interval"), QStringLiteral("1d"));
    query.addQueryItem(QStringLiteral("range"),    QStringLiteral("1d"));
    url.setQuery(query);

    QNetworkReply* reply = m_networkManager->get(makeRequest(url));
    connect(reply, &QNetworkReply::finished, this, [reply, sym, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(errorResult(QStringLiteral("stock lookup failed: ") + reply->errorString()));
            return;
        }
        const QJsonObject root = QJsonDocument::fromJson(reply->readAll()).object();
        const QJsonArray resultArr = root.value(QStringLiteral("chart")).toObject()
                                         .value(QStringLiteral("result")).toArray();
        if (resultArr.isEmpty()) {
            done(errorResult(QStringLiteral("no quote found for \"") + sym + QStringLiteral("\"")));
            return;
        }
        const QJsonObject meta = resultArr.first().toObject().value(QStringLiteral("meta")).toObject();
        if (!meta.contains(QStringLiteral("regularMarketPrice"))) {
            done(errorResult(QStringLiteral("no price data for \"") + sym + QStringLiteral("\"")));
            return;
        }
        const double price    = meta.value(QStringLiteral("regularMarketPrice")).toDouble();
        // previousClose is the usual key; fall back to chartPreviousClose when absent.
        double prevClose = meta.value(QStringLiteral("previousClose")).toDouble();
        if (prevClose <= 0.0)
            prevClose = meta.value(QStringLiteral("chartPreviousClose")).toDouble();
        const double change = price - prevClose;
        const double changePct = (prevClose > 0.0) ? (change / prevClose) * 100.0 : 0.0;
        QJsonObject out;
        out[QStringLiteral("symbol")]        = meta.value(QStringLiteral("symbol")).toString(sym);
        out[QStringLiteral("price")]         = QString::number(price, 'f', 2).toDouble();
        out[QStringLiteral("change")]        = QString::number(change, 'f', 2).toDouble();
        out[QStringLiteral("changePercent")] = QString::number(changePct, 'f', 2).toDouble();
        const QString currency = meta.value(QStringLiteral("currency")).toString();
        if (!currency.isEmpty()) out[QStringLiteral("currency")] = currency;
        done(out);
    });
}

// ─── get_local_news ──────────────────────────────────────────────────────────
// One keyless GET to Google News RSS. We parse the top ~5 <item> titles (+ source/pubDate when present).

void BaristaWebTools::getLocalNews(const QString& query, Done done)
{
    const QString q = query.trimmed();
    if (q.isEmpty()) {
        done(errorResult(QStringLiteral("no news query — need a topic or a location")));
        return;
    }

    QUrl url(QStringLiteral("https://news.google.com/rss/search"));
    QUrlQuery urlQuery;
    urlQuery.addQueryItem(QStringLiteral("q"),    q);
    urlQuery.addQueryItem(QStringLiteral("hl"),   QStringLiteral("en-US"));
    urlQuery.addQueryItem(QStringLiteral("gl"),   QStringLiteral("US"));
    urlQuery.addQueryItem(QStringLiteral("ceid"), QStringLiteral("US:en"));
    url.setQuery(urlQuery);

    QNetworkReply* reply = m_networkManager->get(makeRequest(url));
    connect(reply, &QNetworkReply::finished, this, [reply, q, done]() {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError) {
            done(errorResult(QStringLiteral("news lookup failed: ") + reply->errorString()));
            return;
        }
        const QString xml = QString::fromUtf8(reply->readAll());
        // Lightweight RSS parse: pull each <item>…</item>, then its <title> and (optional) <source>/<pubDate>.
        // A regex is adequate here (well-formed RSS from a single known feed) and avoids dragging in QXmlStream
        // for three fields. HTML entities in titles are decoded for readability.
        static const QRegularExpression itemRx(QStringLiteral("<item>(.*?)</item>"),
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression titleRx(QStringLiteral("<title>(.*?)</title>"),
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression sourceRx(QStringLiteral("<source[^>]*>(.*?)</source>"),
            QRegularExpression::DotMatchesEverythingOption);
        static const QRegularExpression pubRx(QStringLiteral("<pubDate>(.*?)</pubDate>"),
            QRegularExpression::DotMatchesEverythingOption);

        const auto clean = [](QString s) {
            s = s.trimmed();
            // Strip a CDATA wrapper if present, then decode the handful of entities Google News emits.
            if (s.startsWith(QLatin1String("<![CDATA[")) && s.endsWith(QLatin1String("]]>")))
                s = s.mid(9, s.length() - 12).trimmed();
            s.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
            s.replace(QStringLiteral("&#39;"), QStringLiteral("'"));
            s.replace(QStringLiteral("&quot;"), QStringLiteral("\""));
            s.replace(QStringLiteral("&lt;"), QStringLiteral("<"));
            s.replace(QStringLiteral("&gt;"), QStringLiteral(">"));
            return s;
        };

        QJsonArray headlines;
        QRegularExpressionMatchIterator it = itemRx.globalMatch(xml);
        while (it.hasNext() && headlines.size() < 5) {
            const QString item = it.next().captured(1);
            const QRegularExpressionMatch tm = titleRx.match(item);
            const QString title = tm.hasMatch() ? clean(tm.captured(1)) : QString();
            if (title.isEmpty()) continue;
            QJsonObject h;
            h[QStringLiteral("title")] = title;
            if (const QRegularExpressionMatch sm = sourceRx.match(item); sm.hasMatch()) {
                const QString src = clean(sm.captured(1));
                if (!src.isEmpty()) h[QStringLiteral("source")] = src;
            }
            if (const QRegularExpressionMatch pm = pubRx.match(item); pm.hasMatch()) {
                const QString pub = clean(pm.captured(1));
                if (!pub.isEmpty()) h[QStringLiteral("published")] = pub;
            }
            headlines.append(h);
        }

        if (headlines.isEmpty()) {
            done(errorResult(QStringLiteral("no headlines found for \"") + q + QStringLiteral("\"")));
            return;
        }
        done(QJsonObject{{QStringLiteral("query"), q}, {QStringLiteral("headlines"), headlines}});
    });
}
