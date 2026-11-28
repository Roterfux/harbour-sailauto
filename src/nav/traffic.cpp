#include "traffic.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QUrlQuery>
#include <QtMath>

#include <algorithm>

namespace {
const double kAheadM = 60000;      // look this far along the route
const double kOnRouteM = 35;       // incident point this close to the route = on it

double metres(const QPointF &a, const QPointF &b)
{
    const double k = M_PI / 180.0;
    const double x = (b.y() - a.y()) * k * std::cos((a.x() + b.x()) / 2 * k);
    const double y = (b.x() - a.x()) * k;
    return std::sqrt(x * x + y * y) * 6371000.0;
}

QString monthKey() { return QStringLiteral("traffic/requests/") + QDate::currentDate().toString(QStringLiteral("yyyy-MM")); }
} // namespace

TrafficService::TrafficService(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    m_timer.setInterval(180000); // 3 min: ~20 requests per hour of navigation
    connect(&m_timer, &QTimer::timeout, this, &TrafficService::query);
}

QString TrafficService::categoryName(int cat)
{
    switch (cat) {
    case 1: return QStringLiteral("Unfall");
    case 2: return QStringLiteral("Nebel");
    case 3: return QStringLiteral("Gefahr");
    case 4: return QStringLiteral("Regen");
    case 5: return QStringLiteral("Glätte");
    case 6: return QStringLiteral("Stau");
    case 7: return QStringLiteral("Spur gesperrt");
    case 8: return QStringLiteral("Straße gesperrt");
    case 9: return QStringLiteral("Baustelle");
    case 10: return QStringLiteral("Sturm");
    case 11: return QStringLiteral("Überflutung");
    case 14: return QStringLiteral("Pannenfahrzeug");
    default: return QStringLiteral("Verkehrsmeldung");
    }
}

int TrafficService::requestsThisMonth() const { return QSettings().value(monthKey(), 0).toInt(); }

void TrafficService::setKey(const QString &key)
{
    const QString k = key.trimmed();
    if (k == m_key) return;
    m_key = k;
    if (m_key.isEmpty()) {
        m_timer.stop();
        m_all.clear();
        m_ahead.clear();
        emit changed();
    } else if (!m_route.isEmpty()) {
        m_timer.start();
        query();
    }
}

void TrafficService::setRoute(const QVector<QPointF> &latlon)
{
    m_route = latlon;
    m_cum.clear();
    m_posAlong = 0;
    m_posSeg = 0;
    double s = 0;
    for (int i = 0; i < m_route.size(); ++i) {
        if (i) s += metres(m_route[i - 1], m_route[i]);
        m_cum << s;
    }
    m_all.clear();
    m_ahead.clear();
    m_shown.clear();
    emit changed();
    if (m_route.size() < 2 || m_key.isEmpty()) {
        m_timer.stop();
        return;
    }
    m_timer.start();
    query();
}

double TrafficService::distToRoute(const QPointF &ll, int *seg, double *along) const
{
    double best = 1e18;
    for (int i = 0; i + 1 < m_route.size(); ++i) {
        // local flat projection around the segment start
        const QPointF a = m_route[i], b = m_route[i + 1];
        const double k = std::cos(qDegreesToRadians(a.x()));
        const QPointF A(0, 0), B((b.y() - a.y()) * k, b.x() - a.x()), P((ll.y() - a.y()) * k, ll.x() - a.x());
        const double len2 = B.x() * B.x() + B.y() * B.y();
        const double t = len2 > 0 ? qBound(0.0, (P.x() * B.x() + P.y() * B.y()) / len2, 1.0) : 0;
        const QPointF Q = A + B * t;
        const double d = std::sqrt((P.x() - Q.x()) * (P.x() - Q.x()) + (P.y() - Q.y()) * (P.y() - Q.y())) * M_PI / 180.0 * 6371000.0;
        if (d < best) {
            best = d;
            if (seg) *seg = i;
            if (along) *along = m_cum[i] + (m_cum[i + 1] - m_cum[i]) * t;
        }
    }
    return best;
}

void TrafficService::updatePosition(double lat, double lon)
{
    if (m_route.size() < 2) return;
    double along = 0;
    int seg = 0;
    if (distToRoute(QPointF(lat, lon), &seg, &along) < 200) {
        m_posAlong = along;
        m_posSeg = seg;
    }
    refreshAhead();
}

void TrafficService::query()
{
    if (m_key.isEmpty() || m_route.size() < 2 || m_busy) return;
    // Bounding box of the route from here up to kAheadM (TomTom: max 10,000 km²)
    double minLat = 90, maxLat = -90, minLon = 180, maxLon = -180;
    for (int i = m_posSeg; i < m_route.size() && m_cum[i] - m_posAlong <= kAheadM; ++i) {
        minLat = qMin(minLat, m_route[i].x());
        maxLat = qMax(maxLat, m_route[i].x());
        minLon = qMin(minLon, m_route[i].y());
        maxLon = qMax(maxLon, m_route[i].y());
    }
    if (minLat > maxLat) return;
    const double pad = 0.01;
    QString base = QString::fromLocal8Bit(qgetenv("SAILAUTO_TRAFFIC_URL"));
    if (base.isEmpty()) base = QStringLiteral("https://api.tomtom.com/traffic/services/5/incidentDetails");
    QUrl url(base);
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("key"), m_key);
    q.addQueryItem(QStringLiteral("bbox"), QStringLiteral("%1,%2,%3,%4").arg(minLon - pad, 0, 'f', 5).arg(minLat - pad, 0, 'f', 5)
                                               .arg(maxLon + pad, 0, 'f', 5).arg(maxLat + pad, 0, 'f', 5));
    q.addQueryItem(QStringLiteral("fields"), QStringLiteral("{incidents{type,geometry{type,coordinates},properties{iconCategory,"
                                                            "magnitudeOfDelay,events{description,code,iconCategory},from,to,"
                                                            "length,delay,roadNumbers}}}"));
    q.addQueryItem(QStringLiteral("language"), QStringLiteral("de-DE"));
    q.addQueryItem(QStringLiteral("timeValidityFilter"), QStringLiteral("present"));
    // accidents, dangers, ice, jams, lane/road closed, works, broken down vehicles
    q.addQueryItem(QStringLiteral("categoryFilter"), QStringLiteral("1,3,5,6,7,8,9,14"));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "SailAuto/" APP_VERSION " (Sailfish OS car display)");
    m_busy = true;
    QSettings().setValue(monthKey(), requestsThisMonth() + 1);
    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_busy = false;
        const int http = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError) {
            const QString err = http == 403 || http == 401 ? QStringLiteral("Schlüssel ungültig oder ohne Traffic-API")
                              : http == 429 ? QStringLiteral("Gratis-Kontingent aufgebraucht")
                                            : r->errorString();
            if (err != m_lastError) emit log(QStringLiteral("Verkehr (TomTom): ") + err);
            m_lastError = err;
            return;
        }
        m_lastError.clear();
        parse(r->readAll());
    });
}

void TrafficService::parse(const QByteArray &json)
{
    m_all.clear();
    for (const QJsonValue &v : QJsonDocument::fromJson(json).object().value(QStringLiteral("incidents")).toArray()) {
        const QJsonObject o = v.toObject();
        const QJsonObject pr = o.value(QStringLiteral("properties")).toObject();
        const QJsonObject geo = o.value(QStringLiteral("geometry")).toObject();
        TrafficIncident in;
        in.category = pr.value(QStringLiteral("iconCategory")).toInt();
        in.magnitude = pr.value(QStringLiteral("magnitudeOfDelay")).toInt();
        in.from = pr.value(QStringLiteral("from")).toString();
        in.to = pr.value(QStringLiteral("to")).toString();
        in.lengthM = pr.value(QStringLiteral("length")).toDouble();
        in.delayS = pr.value(QStringLiteral("delay")).toDouble();
        QStringList roads;
        for (const QJsonValue &rn : pr.value(QStringLiteral("roadNumbers")).toArray()) roads << rn.toString();
        in.roads = roads.join(QStringLiteral(", "));
        const QJsonArray events = pr.value(QStringLiteral("events")).toArray();
        if (!events.isEmpty()) in.text = events.first().toObject().value(QStringLiteral("description")).toString();
        if (in.text.isEmpty()) in.text = categoryName(in.category);
        const QJsonArray c = geo.value(QStringLiteral("coordinates")).toArray();
        if (geo.value(QStringLiteral("type")).toString() == QLatin1String("Point")) {
            in.line << QPointF(c.at(1).toDouble(), c.at(0).toDouble());
        } else {
            for (const QJsonValue &pt : c) {
                const QJsonArray a = pt.toArray();
                in.line << QPointF(a.at(1).toDouble(), a.at(0).toDouble());
            }
        }
        if (in.line.isEmpty()) continue;
        // On the route: (most of) its points near the route, in driving direction
        // (a jam on the other carriageway runs the other way)
        int hits = 0;
        for (const QPointF &pt : in.line) {
            double along = 0;
            if (distToRoute(pt, nullptr, &along) <= kOnRouteM) {
                ++hits;
                if (in.startAlong < 0) in.startAlong = along;
                in.endAlong = along;
            }
        }
        const bool onRoute = in.line.size() == 1 ? hits == 1 : hits * 2 >= in.line.size() && in.endAlong >= in.startAlong;
        if (!onRoute) in.startAlong = in.endAlong = -1;
        m_all.append(in);
    }
    refreshAhead();
    int n = 0;
    for (const TrafficIncident &i : m_ahead) n += i.aheadM >= 0;
    emit log(QStringLiteral("Verkehr (TomTom): %1 Meldungen im Gebiet, %2 auf deiner Route").arg(m_all.size()).arg(n));
}

void TrafficService::refreshAhead()
{
    QVector<TrafficIncident> ahead;
    for (TrafficIncident in : m_all) {
        if (in.startAlong < 0 || in.endAlong < m_posAlong) continue; // not on the route / behind us
        in.aheadM = qMax(0.0, in.startAlong - m_posAlong);
        if (in.aheadM > kAheadM) continue;
        ahead.append(in);
    }
    std::sort(ahead.begin(), ahead.end(), [](const TrafficIncident &a, const TrafficIncident &b) { return a.aheadM < b.aheadM; });
    // Tell the screen when something changed or we came 50 m closer
    const bool same = ahead.size() == m_shown.size()
                      && std::equal(ahead.begin(), ahead.end(), m_shown.begin(), [](const TrafficIncident &a, const TrafficIncident &b) {
                             return a.text == b.text && qAbs(a.aheadM - b.aheadM) < 50;
                         });
    m_ahead = ahead;
    if (!same) {
        m_shown = ahead;
        emit changed();
    }
}
