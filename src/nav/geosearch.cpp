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
            emit finished(text, out, r->errorString());
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
