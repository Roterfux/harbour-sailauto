#include "speedlimit.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>
#include <QtMath>

namespace {
const double kWindowM = 50000; // ask for this much of the route at once
enum Kind { Window = 1, Snap = 2 };

double metres(const QPointF &a, const QPointF &b)
{
    const double k = M_PI / 180.0;
    const double x = (b.y() - a.y()) * k * std::cos((a.x() + b.x()) / 2 * k);
    const double y = (b.x() - a.x()) * k;
    return std::sqrt(x * x + y * y) * 6371000.0;
}

QString traceUrl(bool online)
{
    const QString env = QString::fromLocal8Bit(qgetenv(online ? "SAILAUTO_TRACE_ONLINE_URL" : "SAILAUTO_TRACE_URL"));
    if (!env.isEmpty()) return env;
    return online ? QStringLiteral("https://valhalla1.openstreetmap.de/trace_attributes")
                  : QStringLiteral("http://127.0.0.1:8553/v2/trace_attributes");
}

// Valhalla: 0 / missing = unknown, very high values = no limit
int validLimit(const QJsonValue &v)
{
    const int k = v.toInt();
    return k > 0 && k < 200 ? k : 0;
}
} // namespace

SpeedLimit::SpeedLimit(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString SpeedLimit::encodePolyline6(const QVector<QPointF> &latlon)
{
    QString out;
    qint64 plat = 0, plon = 0;
    auto enc = [&out](qint64 v) {
        quint64 u = v < 0 ? ~(quint64(v) << 1) : quint64(v) << 1;
        while (u >= 0x20) {
            out += QChar(char((0x20 | (u & 0x1f)) + 63));
            u >>= 5;
        }
        out += QChar(char(u + 63));
    };
    for (const QPointF &p : latlon) {
        const qint64 la = qRound64(p.x() * 1e6), lo = qRound64(p.y() * 1e6);
        enc(la - plat);
        enc(lo - plon);
        plat = la;
        plon = lo;
    }
    return out;
}

void SpeedLimit::setLimit(int kmh)
{
    if (kmh == m_limit) return;
    m_limit = kmh;
    emit limitChanged(kmh);
}

void SpeedLimit::setRoute(const QVector<QPointF> &latlon)
{
    m_route = latlon;
    m_routeLimits = QVector<int>(latlon.size(), -1);
    m_windowEnd = 0;
    m_routeIdx = 0;
    if (m_route.size() > 1) requestWindow();
}

void SpeedLimit::post(const QByteArray &json, bool online, int kind, int windowStart)
{
    QNetworkRequest req{QUrl(traceUrl(online))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("User-Agent", "SailAuto/" APP_VERSION " (Sailfish OS car display)");
    m_busy = true;
    QNetworkReply *r = m_net->post(req, json);
    const int routeSize = m_route.size();
    connect(r, &QNetworkReply::finished, this, [this, r, json, online, kind, windowStart, routeSize]() {
        r->deleteLater();
        m_busy = false;
        const QJsonObject o = QJsonDocument::fromJson(r->readAll()).object();
        const QJsonArray edges = o.value(QStringLiteral("edges")).toArray();
        if (r->error() != QNetworkReply::NoError || edges.isEmpty()) {
            if (!online) { // OSM Scout Server without trace_attributes: online
                m_offlineFailed = true;
                post(json, true, kind, windowStart);
            } else {
                m_failClock.start();
                if (kind == Window) m_windowEnd = qMin(m_windowEnd, windowStart); // ask again later
                if (!m_logged) {
                    m_logged = true;
                    emit log(QStringLiteral("Tempolimit: nicht abrufbar (%1)").arg(r->errorString()));
                }
            }
            return;
        }
        if (kind == Window) {
            if (routeSize != m_route.size()) return; // route changed meanwhile
            for (const QJsonValue &ev : edges) {
                const QJsonObject e = ev.toObject();
                const int lim = validLimit(e.value(QStringLiteral("speed_limit")));
                const int b = windowStart + e.value(QStringLiteral("begin_shape_index")).toInt();
                const int en = windowStart + e.value(QStringLiteral("end_shape_index")).toInt();
                for (int i = b; i < en && i < m_routeLimits.size(); ++i) m_routeLimits[i] = lim;
            }
            if (!m_logged) {
                m_logged = true;
                emit log(QStringLiteral("Tempolimit: Daten für die Route da (%1)").arg(online ? QStringLiteral("online") : QStringLiteral("offline")));
            }
            if (m_routeIdx < m_routeLimits.size()) setLimit(qMax(0, m_routeLimits[m_routeIdx]));
        } else {
            setLimit(validLimit(edges.last().toObject().value(QStringLiteral("speed_limit"))));
        }
    });
}

void SpeedLimit::requestWindow()
{
    if (m_busy || m_windowEnd >= m_route.size() - 1) return;
    if (m_failClock.isValid() && m_failClock.elapsed() < 60000) return;
    // From the window end on, up to kWindowM (one point overlap)
    const int start = m_windowEnd;
    int end = start + 1;
    double d = 0;
    while (end < m_route.size() - 1 && d < kWindowM) {
        d += metres(m_route[end], m_route[end + 1]);
        ++end;
    }
    const QVector<QPointF> part = m_route.mid(start, end - start + 1);
    QJsonObject q;
    q.insert(QStringLiteral("encoded_polyline"), encodePolyline6(part));
    q.insert(QStringLiteral("costing"), QStringLiteral("auto"));
    q.insert(QStringLiteral("shape_match"), QStringLiteral("edge_walk"));
    QJsonObject filters;
    filters.insert(QStringLiteral("attributes"), QJsonArray{QStringLiteral("edge.speed_limit"), QStringLiteral("edge.begin_shape_index"),
                                                            QStringLiteral("edge.end_shape_index")});
    filters.insert(QStringLiteral("action"), QStringLiteral("include"));
    q.insert(QStringLiteral("filters"), filters);
    m_windowEnd = end;
    post(QJsonDocument(q).toJson(QJsonDocument::Compact), m_offlineFailed, Window, start);
}

void SpeedLimit::requestSnap()
{
    if (m_busy || m_trail.size() < 3) return;
    if (m_failClock.isValid() && m_failClock.elapsed() < 60000) return;
    QJsonArray shape;
    for (const QPointF &p : m_trail) shape.append(QJsonObject{{QStringLiteral("lat"), p.x()}, {QStringLiteral("lon"), p.y()}});
    QJsonObject q;
    q.insert(QStringLiteral("shape"), shape);
    q.insert(QStringLiteral("costing"), QStringLiteral("auto"));
    q.insert(QStringLiteral("shape_match"), QStringLiteral("map_snap"));
    QJsonObject filters;
    filters.insert(QStringLiteral("attributes"), QJsonArray{QStringLiteral("edge.speed_limit")});
    filters.insert(QStringLiteral("action"), QStringLiteral("include"));
    q.insert(QStringLiteral("filters"), filters);
    post(QJsonDocument(q).toJson(QJsonDocument::Compact), m_offlineFailed, Snap, 0);
}

void SpeedLimit::updatePosition(double lat, double lon, double speedMs)
{
    const QPointF here(lat, lon);
    if (m_route.size() > 1) {
        // nearest route point around the last one
        int best = m_routeIdx;
        double bestD = 1e18;
        const int from = qMax(0, m_routeIdx - 5), to = qMin(m_route.size() - 1, m_routeIdx + 300);
        for (int i = from; i <= to; ++i) {
            const double d = metres(here, m_route[i]);
            if (d < bestD) { bestD = d; best = i; }
        }
        if (bestD > 300) { // jumped (re-route coming): search all
            for (int i = 0; i < m_route.size(); ++i) {
                const double d = metres(here, m_route[i]);
                if (d < bestD) { bestD = d; best = i; }
            }
        }
        // the point we passed: segment starting there
        m_routeIdx = best > 0 && metres(here, m_route[best]) < metres(m_route[best - 1], m_route[best]) / 2 ? best - 1 : best;
        if (bestD < 60 && m_routeIdx < m_routeLimits.size() && m_routeLimits[m_routeIdx] >= 0)
            setLimit(m_routeLimits[m_routeIdx]);
        // ask for the next part in time
        double ahead = 0;
        for (int i = m_routeIdx; i + 1 < m_windowEnd && ahead < 10000; ++i) ahead += metres(m_route[i], m_route[i + 1]);
        if (ahead < 10000) requestWindow();
        return;
    }
    // No route: map-match the last positions now and then
    if (m_trail.isEmpty() || metres(m_trail.last(), here) > 15) m_trail.append(here);
    while (m_trail.size() > 8) m_trail.removeFirst();
    if (speedMs < 2) return;
    if (!m_snapClock.isValid() || m_snapClock.elapsed() > 15000) {
        m_snapClock.start();
        requestSnap();
    }
}
