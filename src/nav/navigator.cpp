#include "navigator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QUrlQuery>
#include <QtMath>

namespace {
const double kEarth = 6371000.0;

double distM(double lat1, double lon1, double lat2, double lon2)
{
    const double p1 = qDegreesToRadians(lat1), p2 = qDegreesToRadians(lat2);
    const double dp = p2 - p1, dl = qDegreesToRadians(lon2 - lon1);
    const double a = std::sin(dp / 2) * std::sin(dp / 2) + std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
    return 2 * kEarth * std::atan2(std::sqrt(a), std::sqrt(1 - a));
}

// Valhalla: encoded polyline, precision 6
QVector<QPointF> decodePolyline6(const QString &enc)
{
    QVector<QPointF> out;
    const QByteArray b = enc.toLatin1();
    int i = 0;
    qint64 lat = 0, lon = 0;
    while (i < b.size()) {
        qint64 v[2];
        for (int k = 0; k < 2; ++k) {
            qint64 result = 0;
            int shift = 0, c;
            do {
                c = b[i++] - 63;
                result |= qint64(c & 0x1f) << shift;
                shift += 5;
            } while (c >= 0x20 && i < b.size());
            v[k] = (result & 1) ? ~(result >> 1) : (result >> 1);
        }
        lat += v[0];
        lon += v[1];
        out.append(QPointF(lat / 1e6, lon / 1e6));
    }
    return out;
}

QString routeUrl(bool online)
{
    const QString env = QString::fromLocal8Bit(qgetenv(online ? "SAILAUTO_ROUTE_ONLINE_URL" : "SAILAUTO_ROUTE_URL"));
    if (!env.isEmpty()) return env;
    return online ? QStringLiteral("https://valhalla1.openstreetmap.de/route")
                  : QStringLiteral("http://127.0.0.1:8553/v2/route");
}
} // namespace

Navigator::Navigator(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

void Navigator::setDestination(const QString &title, double lat, double lon)
{
    m_destTitle = title;
    m_destLat = lat;
    m_destLon = lon;
    m_active = true;
    m_shape.clear();
    m_man.clear();
    m_state = NavState();
    m_state.app = true;
    m_state.hasRoute = false;
    m_state.narrative = QStringLiteral("Route wird berechnet");
    emit changed();
    emit log(QStringLiteral("Navigation (SailAuto): Ziel gesetzt"));
    if (m_havePos) requestRoute(false);
}

void Navigator::stop()
{
    if (!m_active) return;
    m_active = false;
    m_shape.clear();
    m_man.clear();
    m_state = NavState();
    emit routeChanged();
    emit changed();
    emit log(QStringLiteral("Navigation (SailAuto): beendet"));
}

void Navigator::requestRoute(bool reroute)
{
    if (m_busy || !m_havePos) return;
    m_busy = true;
    m_lastReroute.start();
    QJsonObject from{{QStringLiteral("lat"), m_lat}, {QStringLiteral("lon"), m_lon}};
    if (m_bearing >= 0 && m_speed > 2) {
        from.insert(QStringLiteral("heading"), qRound(m_bearing));
        from.insert(QStringLiteral("heading_tolerance"), 45);
    }
    const QJsonObject to{{QStringLiteral("lat"), m_destLat}, {QStringLiteral("lon"), m_destLon}};
    const QJsonObject req{{QStringLiteral("locations"), QJsonArray{from, to}},
                          {QStringLiteral("costing"), QStringLiteral("auto")},
                          {QStringLiteral("directions_options"),
                           QJsonObject{{QStringLiteral("units"), QStringLiteral("kilometers")},
                                       {QStringLiteral("language"), QStringLiteral("de-DE")}}}};
    const QByteArray json = QJsonDocument(req).toJson(QJsonDocument::Compact);
    auto send = [this, json, reroute](bool online, auto &&self) -> void {
        QUrl url(routeUrl(online));
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("json"), QString::fromUtf8(json));
        url.setQuery(q);
        QNetworkRequest r(url);
        r.setRawHeader("User-Agent", "SailAuto (Sailfish OS car navigation)");
        QNetworkReply *rep = m_net->get(r);
        connect(rep, &QNetworkReply::finished, this, [this, rep, online, reroute, self]() {
            rep->deleteLater();
            const QByteArray body = rep->readAll();
            const bool ok = rep->error() == QNetworkReply::NoError
                            && QJsonDocument::fromJson(body).object().contains(QStringLiteral("trip"));
            if (!ok && !online) { // offline server missing or no route: online
                self(true, self);
                return;
            }
            m_busy = false;
            if (!ok) {
                const QString err = QJsonDocument::fromJson(body).object().value(QStringLiteral("error")).toString();
                emit log(QStringLiteral("Navigation (SailAuto): keine Route (%1)").arg(err.isEmpty() ? rep->errorString() : err));
                m_state.narrative = QStringLiteral("Keine Route gefunden");
                emit changed();
                return;
            }
            if (!m_active) return;
            m_usedOnline = online;
            parseRoute(body);
            emit log(QStringLiteral("Navigation (SailAuto): %1 %2, %3 (%4)")
                         .arg(reroute ? QStringLiteral("neue Route") : QStringLiteral("Route"), fmtDist(m_totalM), fmtTime(m_totalS),
                              online ? QStringLiteral("online") : QStringLiteral("offline")));
        });
    };
    send(false, send);
}

void Navigator::parseRoute(const QByteArray &json)
{
    const QJsonObject trip = QJsonDocument::fromJson(json).object().value(QStringLiteral("trip")).toObject();
    const QJsonArray legs = trip.value(QStringLiteral("legs")).toArray();
    m_shape.clear();
    m_man.clear();
    if (legs.isEmpty()) return;
    const QJsonObject leg = legs.first().toObject();
    m_shape = decodePolyline6(leg.value(QStringLiteral("shape")).toString());
    m_cum.resize(m_shape.size());
    double acc = 0;
    for (int i = 0; i < m_shape.size(); ++i) {
        if (i > 0) acc += distM(m_shape[i - 1].x(), m_shape[i - 1].y(), m_shape[i].x(), m_shape[i].y());
        m_cum[i] = acc;
    }
    m_totalM = acc;
    m_totalS = trip.value(QStringLiteral("summary")).toObject().value(QStringLiteral("time")).toDouble();
    for (const QJsonValue &v : leg.value(QStringLiteral("maneuvers")).toArray()) {
        const QJsonObject o = v.toObject();
        Maneuver m;
        m.type = o.value(QStringLiteral("type")).toInt();
        m.instruction = o.value(QStringLiteral("instruction")).toString();
        m.verbal = o.value(QStringLiteral("verbal_pre_transition_instruction")).toString();
        const QJsonArray streets = o.value(QStringLiteral("street_names")).toArray();
        m.street = streets.isEmpty() ? QString() : streets.first().toString();
        m.beginIndex = qBound(0, o.value(QStringLiteral("begin_shape_index")).toInt(), qMax(0, m_shape.size() - 1));
        m.roundaboutExit = o.value(QStringLiteral("roundabout_exit_count")).toInt();
        m.alongM = m_cum.value(m.beginIndex);
        m_man.append(m);
    }
    m_seg = 0;
    m_along = 0;
    m_offCount = 0;
    m_announced = -1;
    m_arrivedTicks = 0;
    m_state.hasRoute = true;
    m_state.running = true;
    emit routeChanged();
    update();
}

void Navigator::updatePosition(double lat, double lon, double bearing, double speed)
{
    m_lat = lat;
    m_lon = lon;
    if (bearing >= 0) m_bearing = bearing;
    m_speed = speed < 0 ? 0 : speed;
    const bool first = !m_havePos;
    m_havePos = true;
    if (!m_active) return;
    if (m_shape.isEmpty()) {
        if (first || !m_busy) requestRoute(false);
        return;
    }
    // Match to the route: nearest point on segments around the last match.
    const double kx = std::cos(qDegreesToRadians(lat)) * kEarth * M_PI / 180.0, ky = kEarth * M_PI / 180.0;
    double best = 1e18, bestAlong = m_along;
    int bestSeg = m_seg;
    const int from = qMax(0, m_seg - 3), to = qMin(m_shape.size() - 2, m_seg + 60);
    for (int i = from; i <= to; ++i) {
        const double ax = (m_shape[i].y() - lon) * kx, ay = (m_shape[i].x() - lat) * ky;
        const double bx = (m_shape[i + 1].y() - lon) * kx, by = (m_shape[i + 1].x() - lat) * ky;
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        double t = len2 > 0 ? -(ax * dx + ay * dy) / len2 : 0;
        t = qBound(0.0, t, 1.0);
        const double px = ax + t * dx, py = ay + t * dy;
        const double d = std::sqrt(px * px + py * py);
        if (d < best) {
            best = d;
            bestSeg = i;
            bestAlong = m_cum[i] + t * (m_cum[i + 1] - m_cum[i]);
        }
    }
    const bool onRoute = best < 45;
    if (onRoute) {
        m_seg = bestSeg;
        m_along = qMax(m_along - 20, bestAlong); // no jumping backwards
        m_offCount = 0;
    } else if (++m_offCount >= 3 && m_speed > 1.5 && (!m_lastReroute.isValid() || m_lastReroute.elapsed() > 10000)) {
        emit log(QStringLiteral("Navigation (SailAuto): abseits der Route, berechne neu"));
        requestRoute(true);
    }
    m_state.alongRoute = onRoute;
    update();
}

void Navigator::update()
{
    if (m_man.isEmpty()) return;
    // Next maneuver: the first one ahead of us
    int next = m_man.size() - 1;
    for (int i = 0; i < m_man.size(); ++i)
        if (m_man[i].alongM > m_along + 5) { next = i; break; }
    const Maneuver &m = m_man[next];
    const double toMan = qMax(0.0, m.alongM - m_along);
    const double remaining = qMax(0.0, m_totalM - m_along);
    const double remainingS = m_totalM > 0 ? m_totalS * remaining / m_totalM : 0;
    m_state.app = true;
    m_state.running = true;
    m_state.hasRoute = true;
    m_state.icon = iconFor(m.type, m.roundaboutExit);
    m_state.roundaboutExit = m.roundaboutExit;
    m_state.manDist = fmtDist(toMan);
    m_state.manTime = fmtTime(m_speed > 1 ? toMan / m_speed : 0);
    m_state.street = m.street;
    m_state.narrative = m.instruction;
    m_state.destDist = fmtDist(remaining);
    m_state.destTime = fmtTime(remainingS);
    m_state.destEta = QDateTime::currentDateTime().addSecs(qint64(remainingS)).toString(QStringLiteral("HH:mm"));
    m_state.progress = m_totalM > 0 ? int(100 * m_along / m_totalM) : 0;
    // Announce once per maneuver: ~ 10 s ahead (at least 150 m)
    const double ahead = qMax(150.0, m_speed * 12);
    if (next != m_announced && toMan < ahead && !m.verbal.isEmpty()) {
        m_announced = next;
        emit announce(m.verbal);
    }
    // Arrival
    if (remaining < 25 || (m.type >= 4 && m.type <= 6 && toMan < 25)) {
        m_state.manDist = QStringLiteral("Ziel erreicht");
        if (++m_arrivedTicks == 1) emit log(QStringLiteral("Navigation (SailAuto): Ziel erreicht"));
        if (m_arrivedTicks > 10) { stop(); return; }
    }
    emit changed();
}

QString Navigator::iconFor(int t, int exitCount)
{
    Q_UNUSED(exitCount)
    switch (t) {
    case 1: case 2: case 3: return QStringLiteral("depart");
    case 4: return QStringLiteral("arrive");
    case 5: return QStringLiteral("arrive-right");
    case 6: return QStringLiteral("arrive-left");
    case 9: return QStringLiteral("turn-slight-right");
    case 10: return QStringLiteral("turn-right");
    case 11: return QStringLiteral("turn-sharp-right");
    case 12: case 13: return QStringLiteral("uturn");
    case 14: return QStringLiteral("turn-sharp-left");
    case 15: return QStringLiteral("turn-left");
    case 16: return QStringLiteral("turn-slight-left");
    case 17: return QStringLiteral("on-ramp-straight");
    case 18: return QStringLiteral("on-ramp-right");
    case 19: return QStringLiteral("on-ramp-left");
    case 20: return QStringLiteral("off-ramp-right");
    case 21: return QStringLiteral("off-ramp-left");
    case 22: return QStringLiteral("fork-straight");
    case 23: return QStringLiteral("fork-right");
    case 24: return QStringLiteral("fork-left");
    case 25: return QStringLiteral("merge-straight");
    case 26: case 27: return QStringLiteral("roundabout-right");
    case 28: case 29: return QStringLiteral("ferry");
    case 37: return QStringLiteral("merge-right");
    case 38: return QStringLiteral("merge-left");
    default: return QStringLiteral("continue");
    }
}

QString Navigator::fmtDist(double m)
{
    if (m < 1000) return QStringLiteral("%1 m").arg(qMax(0, qRound(m / 10) * 10));
    if (m < 10000) return QStringLiteral("%1 km").arg(QString::number(m / 1000, 'f', 1).replace(QLatin1Char('.'), QLatin1Char(',')));
    return QStringLiteral("%1 km").arg(qRound(m / 1000));
}

QString Navigator::fmtTime(double s)
{
    const int min = qRound(s / 60);
    if (min < 60) return QStringLiteral("%1 min").arg(min);
    return QStringLiteral("%1 h %2 min").arg(min / 60).arg(min % 60, 2, 10, QLatin1Char('0'));
}
