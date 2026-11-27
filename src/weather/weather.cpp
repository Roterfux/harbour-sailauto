#include "weather.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QtMath>

WeatherClient::WeatherClient(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

void WeatherClient::setPosition(double lat, double lon)
{
    m_lat = lat;
    m_lon = lon;
    m_havePos = true;
    const double moved = qAbs(lat - m_fetchedLat) + qAbs(lon - m_fetchedLon);
    if (!m_lastFetch.isValid() || m_lastFetch.elapsed() > 20 * 60 * 1000 || moved > 0.15) fetch();
}

void WeatherClient::fetch()
{
    if (m_busy || !m_havePos) return;
    m_busy = true;
    m_lastFetch.start();
    // Round: ~10 km is plenty for weather and keeps the exact position private.
    const double lat = qRound(m_lat * 10) / 10.0, lon = qRound(m_lon * 10) / 10.0;
    m_fetchedLat = m_lat;
    m_fetchedLon = m_lon;
    QString base = QString::fromLocal8Bit(qgetenv("SAILAUTO_WEATHER_URL"));
    if (base.isEmpty()) base = QStringLiteral("https://api.open-meteo.com/v1/forecast");
    const QUrl url(base + QStringLiteral("?latitude=%1&longitude=%2&current=temperature_2m,weather_code,is_day"
                                         "&hourly=temperature_2m,weather_code&daily=weather_code,temperature_2m_max,temperature_2m_min"
                                         "&timezone=auto&forecast_days=3").arg(lat).arg(lon));
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "SailAuto");
    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_busy = false;
        if (r->error() != QNetworkReply::NoError) {
            emit log(QStringLiteral("Wetter: nicht geladen (%1)").arg(r->errorString()));
            return;
        }
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const QJsonObject cur = o.value(QStringLiteral("current")).toObject();
        if (cur.isEmpty()) {
            emit log(QStringLiteral("Wetter: unerwartete Antwort"));
            return;
        }
        WeatherData d;
        d.valid = true;
        d.temp = cur.value(QStringLiteral("temperature_2m")).toDouble();
        d.code = cur.value(QStringLiteral("weather_code")).toInt();
        d.day = cur.value(QStringLiteral("is_day")).toInt(1) != 0;
        // Next 6 full hours
        const QJsonObject h = o.value(QStringLiteral("hourly")).toObject();
        const QJsonArray ht = h.value(QStringLiteral("time")).toArray();
        const QJsonArray htemp = h.value(QStringLiteral("temperature_2m")).toArray();
        const QJsonArray hcode = h.value(QStringLiteral("weather_code")).toArray();
        const QString nowHour = cur.value(QStringLiteral("time")).toString().left(13); // "2026-11-27T15"
        for (int i = 0; i < ht.size() && d.hours.size() < 6; ++i) {
            const QString t = ht[i].toString();
            if (t.left(13) <= nowHour) continue;
            d.hours.append({t.mid(11, 2).toInt(), htemp[i].toDouble(), hcode[i].toInt()});
        }
        const QJsonObject dd = o.value(QStringLiteral("daily")).toObject();
        const QJsonArray dt = dd.value(QStringLiteral("time")).toArray();
        for (int i = 0; i < dt.size() && i < 3; ++i)
            d.days.append({QDate::fromString(dt[i].toString(), Qt::ISODate),
                           dd.value(QStringLiteral("weather_code")).toArray()[i].toInt(),
                           dd.value(QStringLiteral("temperature_2m_max")).toArray()[i].toDouble(),
                           dd.value(QStringLiteral("temperature_2m_min")).toArray()[i].toDouble()});
        m_data = d;
        if (!m_logged) {
            m_logged = true;
            emit log(QStringLiteral("Wetter: %1 °C, %2").arg(qRound(d.temp)).arg(describe(d.code)));
        }
        emit changed();
    });
}

QString WeatherClient::describe(int c)
{
    if (c == 0) return QStringLiteral("klar");
    if (c == 1) return QStringLiteral("überwiegend klar");
    if (c == 2) return QStringLiteral("teilweise bewölkt");
    if (c == 3) return QStringLiteral("bedeckt");
    if (c == 45 || c == 48) return QStringLiteral("Nebel");
    if (c >= 51 && c <= 57) return QStringLiteral("Nieselregen");
    if (c >= 61 && c <= 67) return QStringLiteral("Regen");
    if (c >= 71 && c <= 77) return QStringLiteral("Schnee");
    if (c >= 80 && c <= 82) return QStringLiteral("Regenschauer");
    if (c == 85 || c == 86) return QStringLiteral("Schneeschauer");
    if (c >= 95) return QStringLiteral("Gewitter");
    return QStringLiteral("Wetter %1").arg(c);
}
