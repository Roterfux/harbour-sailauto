#include "geosearch.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>

GeoSearch::GeoSearch(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

void GeoSearch::search(const QString &text)
{
    QString base = QString::fromLocal8Bit(qgetenv("SAILAUTO_GEOCODER_URL"));
    if (base.isEmpty()) base = QStringLiteral("http://127.0.0.1:8553/v1/search");
    QUrl url(base);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("8"));
    q.addQueryItem(QStringLiteral("search"), text);
    if (m_bias) {
        q.addQueryItem(QStringLiteral("lat"), QString::number(m_lat, 'f', 5));
        q.addQueryItem(QStringLiteral("lng"), QString::number(m_lon, 'f', 5));
    }
    url.setQuery(q);
    QNetworkReply *r = m_net->get(QNetworkRequest(url));
    // No timeout on purpose: OSM Scout Server may still be starting (see its API notes).
    connect(r, &QNetworkReply::finished, this, [this, r, text]() {
        r->deleteLater();
        QVector<GeoResult> out;
        if (r->error() != QNetworkReply::NoError) {
            searchPhoton(text, r->errorString());
            return;
        }
        for (const QJsonValue &v : QJsonDocument::fromJson(r->readAll()).array()) {
            const QJsonObject o = v.toObject();
            GeoResult g;
            g.title = o.value(QStringLiteral("title")).toString();
            g.region = o.value(QStringLiteral("admin_region")).toString();
            g.lat = o.value(QStringLiteral("lat")).toDouble();
            g.lon = o.value(QStringLiteral("lng")).toDouble();
            if (!g.title.isEmpty()) out.append(g);
        }
        emit finished(text, out, QString());
    });
}

void GeoSearch::searchPhoton(const QString &text, const QString &firstError)
{
    QString base = QString::fromLocal8Bit(qgetenv("SAILAUTO_PHOTON_URL"));
    if (base.isEmpty()) base = QStringLiteral("https://photon.komoot.io/api/");
    QUrl url(base);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), text);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("8"));
    q.addQueryItem(QStringLiteral("lang"), QStringLiteral("de"));
    if (m_bias) { // rounded: enough to prefer nearby results
        q.addQueryItem(QStringLiteral("lat"), QString::number(m_lat, 'f', 2));
        q.addQueryItem(QStringLiteral("lon"), QString::number(m_lon, 'f', 2));
    }
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "SailAuto (Sailfish OS)");
    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r, text, firstError]() {
        r->deleteLater();
        QVector<GeoResult> out;
        if (r->error() != QNetworkReply::NoError) {
            emit finished(text, out, firstError + QStringLiteral(" / Photon: ") + r->errorString());
            return;
        }
        // GeoJSON: features[].geometry.coordinates [lon, lat], properties name/street/...
        for (const QJsonValue &v : QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("features")).toArray()) {
            const QJsonObject f = v.toObject();
            const QJsonObject pr = f.value(QStringLiteral("properties")).toObject();
            const QJsonArray c = f.value(QStringLiteral("geometry")).toObject().value(QStringLiteral("coordinates")).toArray();
            if (c.size() < 2) continue;
            const QString street = (pr.value(QStringLiteral("street")).toString() + QLatin1Char(' ')
                                    + pr.value(QStringLiteral("housenumber")).toString()).trimmed();
            const QString name = pr.value(QStringLiteral("name")).toString();
            GeoResult g;
            g.title = !name.isEmpty() ? name : street;
            QStringList region;
            if (!name.isEmpty() && !street.isEmpty()) region << street;
            const QString city = (pr.value(QStringLiteral("postcode")).toString() + QLatin1Char(' ')
                                  + pr.value(QStringLiteral("city")).toString()).trimmed();
            if (!city.isEmpty()) region << city;
            if (!pr.value(QStringLiteral("country")).toString().isEmpty()) region << pr.value(QStringLiteral("country")).toString();
            g.region = region.join(QStringLiteral(", "));
            g.lon = c[0].toDouble();
            g.lat = c[1].toDouble();
            if (!g.title.isEmpty()) out.append(g);
        }
        emit finished(text, out, QString());
    });
}
