#include "mapview.h"

#include <QDateTime>
#include <QDir>
#include <QLinearGradient>
#include <QNetworkAccessManager>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QPolygonF>
#include <QStandardPaths>
#include <QTransform>
#include <QtMath>

namespace {
const int kTile = 256;
const int kGrid = 5;              // canvas = 5x5 tiles
const int kMaxPending = 6;        // OSM Scout Server renders one by one
}

MapView::MapView(QObject *parent) : QObject(parent)
{
    m_net = new QNetworkAccessManager(this);
    auto *cache = new QNetworkDiskCache(this);
    cache->setCacheDirectory(QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/tiles"));
    cache->setMaximumCacheSize(80 * 1024 * 1024);
    m_net->setCache(cache);
    connect(m_net, &QNetworkAccessManager::finished, this, &MapView::onReply);
    m_url = QStringLiteral("http://127.0.0.1:8553/v1/tile?style=default&daylight={daylight}&shift=0&scale=1&z={z}&x={x}&y={y}");
}

QString MapView::tileTemplate() const
{
    return m_online ? QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png") : m_url;
}

void MapView::setTileUrl(const QString &url)
{
    if (url.trimmed().isEmpty() || url == m_url) return;
    m_url = url.trimmed();
    m_customUrl = !m_url.startsWith(QLatin1String("http://127.0.0.1:8553/"));
    m_online = false;
    m_tiles.clear();
    m_failed.clear();
    m_canvasDirty = true;
}

void MapView::setNight(bool night)
{
    if (night == m_night) return;
    m_night = night;
    m_tiles.clear(); // day/night tiles differ
    m_failed.clear();
    m_canvasDirty = true;
}

void MapView::updateFix(double lat, double lon, double bearing, double speed)
{
    if (m_haveFix && bearing < 0) {
        // No bearing from the car: from the movement since the last fix.
        const QPointF a = worldPixel(m_lat, m_lon, 20), b = worldPixel(lat, lon, 20);
        if (QLineF(a, b).length() > 20) // > ~2 m at z20
            bearing = qRadiansToDegrees(qAtan2(b.x() - a.x(), -(b.y() - a.y())));
    }
    if (!m_haveFix) m_shownBearing = bearing >= 0 ? bearing : 0;
    m_prevLat = m_lat;
    m_prevLon = m_lon;
    m_lat = lat;
    m_lon = lon;
    if (bearing >= 0 && (speed < 0 || speed > 1.0)) m_bearing = bearing; // keep heading when standing
    m_speed = speed < 0 ? 0 : speed;
    m_fixAge.start();
    if (!m_haveFix) emit log(QStringLiteral("Karte: erste Auto-Position"));
    m_haveFix = true;
    // Zoom by speed (hysteresis via thresholds)
    const double kmh = m_speed * 3.6;
    const int z = kmh > 95 ? 15 : kmh > 45 ? 16 : 17;
    if (z != m_zoom && (kmh > 100 || kmh < 90) && (kmh > 50 || kmh < 40)) {
        m_zoom = z;
        m_canvasDirty = true;
    }
}

QPointF MapView::worldPixel(double lat, double lon, int z) const
{
    const double n = kTile * std::pow(2.0, z);
    const double x = (lon + 180.0) / 360.0 * n;
    const double s = std::sin(qDegreesToRadians(qBound(-85.0, lat, 85.0)));
    const double y = (0.5 - std::log((1 + s) / (1 - s)) / (4 * M_PI)) * n;
    return QPointF(x, y);
}

QString MapView::tileKey(int z, int x, int y) const
{
    return QStringLiteral("%1/%2/%3").arg(z).arg(x).arg(y);
}

void MapView::request(int z, int x, int y)
{
    const QString key = tileKey(z, x, y);
    if (m_tiles.contains(key) || m_pending.contains(key) || m_pending.size() >= kMaxPending) return;
    const qint64 failedAt = m_failed.value(key, 0);
    if (failedAt && QDateTime::currentMSecsSinceEpoch() - failedAt < 30000) return;
    QString url = tileTemplate();
    url.replace(QStringLiteral("{z}"), QString::number(z)).replace(QStringLiteral("{x}"), QString::number(x))
        .replace(QStringLiteral("{y}"), QString::number(y))
        .replace(QStringLiteral("{daylight}"), m_night ? QStringLiteral("0") : QStringLiteral("1"));
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    // openstreetmap.org tile policy: identify the app, use the cache.
    req.setRawHeader("User-Agent", "SailAuto/" APP_VERSION " (Sailfish OS car display; github.com/Roterfux/harbour-sailauto)");
    QNetworkReply *r = m_net->get(req);
    r->setProperty("key", key);
    m_pending.insert(key);
}

void MapView::onReply(QNetworkReply *reply)
{
    reply->deleteLater();
    const QString key = reply->property("key").toString();
    m_pending.remove(key);
    QImage img;
    if (reply->error() == QNetworkReply::NoError) img.loadFromData(reply->readAll());
    if (img.isNull() && m_online && reply->url().host() == QLatin1String("127.0.0.1")) return; // late offline replies
    if (img.isNull()) {
        // OSM Scout Server not running at all: switch to online tiles once.
        const auto e = reply->error();
        if (!m_online && !m_customUrl
            && (e == QNetworkReply::ConnectionRefusedError || e == QNetworkReply::HostNotFoundError
                || e == QNetworkReply::TimeoutError || e == QNetworkReply::RemoteHostClosedError)) {
            m_online = true;
            m_failed.clear();
            m_canvasDirty = true;
            emit log(QStringLiteral("Karte: OSM Scout Server nicht erreichbar, nutze Online-Karte (openstreetmap.org)"));
            return;
        }
        m_failed.insert(key, QDateTime::currentMSecsSinceEpoch());
        const QString err = reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("kein Bild");
        if (err != m_lastError) {
            m_lastError = err;
            emit log(m_online ? QStringLiteral("Karte: Online-Kachel nicht geladen (%1). Internet da?").arg(err)
                              : QStringLiteral("Karte: Kachel nicht geladen (%1). Läuft OSM Scout Server mit Karten?").arg(err));
        }
        return;
    }
    if (img.width() != kTile) img = img.scaled(kTile, kTile, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (m_okCount++ == 0) emit log(QStringLiteral("Karte: Kacheln kommen an"));
    m_lastError.clear();
    m_tiles.insert(key, img.convertToFormat(QImage::Format_RGB32));
    if (m_tiles.size() > 300) { // crude limit: drop other zoom levels first
        for (auto it = m_tiles.begin(); it != m_tiles.end() && m_tiles.size() > 200;) {
            if (!it.key().startsWith(QString::number(m_zoom) + QLatin1Char('/'))) it = m_tiles.erase(it);
            else ++it;
        }
    }
    m_canvasDirty = true;
}

void MapView::rebuildCanvas(int z, int cx, int cy)
{
    if (m_canvas.isNull()) m_canvas = QImage(kGrid * kTile, kGrid * kTile, QImage::Format_RGB32);
    m_canvas.fill(m_night ? QColor(30, 34, 44) : QColor(225, 222, 214));
    QPainter p(&m_canvas);
    const int x0 = cx - kGrid / 2, y0 = cy - kGrid / 2;
    const int n = 1 << z;
    // Nearest tiles first (the server renders in order)
    QVector<QPoint> order;
    for (int dy = 0; dy < kGrid; ++dy)
        for (int dx = 0; dx < kGrid; ++dx) order.append(QPoint(dx, dy));
    std::sort(order.begin(), order.end(), [](const QPoint &a, const QPoint &b) {
        return qAbs(a.x() - kGrid / 2) + qAbs(a.y() - kGrid / 2) < qAbs(b.x() - kGrid / 2) + qAbs(b.y() - kGrid / 2);
    });
    for (const QPoint &o : order) {
        const int tx = ((x0 + o.x()) % n + n) % n, ty = y0 + o.y();
        if (ty < 0 || ty >= n) continue;
        const QString key = tileKey(z, tx, ty);
        auto it = m_tiles.constFind(key);
        if (it != m_tiles.constEnd()) p.drawImage(o.x() * kTile, o.y() * kTile, *it);
        else request(z, tx, ty);
    }
    m_canvasZ = z;
    m_canvasX = x0;
    m_canvasY = y0;
    m_canvasDirty = false;
}

void MapView::renderLayer(const QSize &size)
{
    if (m_layer.size() != size) m_layer = QImage(size, QImage::Format_RGB32);
    const QColor sky = m_night ? QColor(14, 18, 28) : QColor(150, 185, 215);
    QPainter p(&m_layer);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.setRenderHint(QPainter::Antialiasing);
    const double w = size.width(), h = size.height();
    const double horizon = h * 0.10;
    QLinearGradient g(0, 0, 0, horizon);
    g.setColorAt(0, sky.darker(130));
    g.setColorAt(1, sky);
    p.fillRect(QRectF(0, 0, w, horizon + 1), g);
    p.fillRect(QRectF(0, horizon, w, h - horizon), m_night ? QColor(30, 34, 44) : QColor(225, 222, 214));

    if (!m_haveFix) {
        p.setPen(m_night ? Qt::white : Qt::black);
        QFont f = p.font();
        f.setPixelSize(qMax(14, int(h / 16)));
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(0, 0, w, h), Qt::AlignCenter, QStringLiteral("Warte auf Position (Auto oder Telefon-GPS)"));
        return;
    }
    // Predicted position: last fix + speed * age along the bearing (max 3 s)
    const int z = m_zoom;
    const double age = qMin(3.0, m_fixAge.elapsed() / 1000.0);
    QPointF car = worldPixel(m_lat, m_lon, z);
    const double metersPerPixel = 156543.03392 * std::cos(qDegreesToRadians(m_lat)) / std::pow(2.0, z);
    const double dist = m_speed * age / metersPerPixel;
    car += QPointF(std::sin(qDegreesToRadians(m_bearing)) * dist, -std::cos(qDegreesToRadians(m_bearing)) * dist);
    // Smooth turning of the map
    double diff = std::remainder(m_bearing - m_shownBearing, 360.0);
    m_shownBearing += diff * 0.25;

    const int ctx = int(car.x() / kTile), cty = int(car.y() / kTile);
    if (m_canvasDirty || z != m_canvasZ || ctx - kGrid / 2 != m_canvasX || cty - kGrid / 2 != m_canvasY)
        rebuildCanvas(z, ctx, cty);

    // Ground (car-centred, heading up, pixels) -> screen trapezoid (tilted view)
    const double ahead = 900, behind = 90, half = 420; // small "behind" puts the car low on screen
    const QPolygonF ground({QPointF(-half, -ahead), QPointF(half, -ahead), QPointF(half, behind), QPointF(-half, behind)});
    const QPolygonF screen({QPointF(w * 0.5 - w * 0.62, horizon), QPointF(w * 0.5 + w * 0.62, horizon),
                            QPointF(w * 0.5 + w * 1.55, h * 1.02), QPointF(w * 0.5 - w * 1.55, h * 1.02)});
    QTransform proj;
    if (!QTransform::quadToQuad(ground, screen, proj)) return;
    QTransform t;
    t.translate(m_canvasX * kTile - car.x(), m_canvasY * kTile - car.y()); // canvas -> car-centred world
    QTransform rot;
    rot.rotate(-m_shownBearing);
    p.save();
    p.setClipRect(QRectF(0, horizon, w, h - horizon));
    p.setTransform(t * rot * proj);
    p.drawImage(0, 0, m_canvas);
    if (m_online && m_night) // online tiles have no night style: darken
        p.fillRect(QRectF(0, 0, m_canvas.width(), m_canvas.height()), QColor(0, 0, 20, 140));
    // Route line in canvas coordinates (same transform as the tiles)
    if (m_route.size() > 1) {
        QPolygonF line;
        const QPointF origin(m_canvasX * kTile, m_canvasY * kTile);
        for (const QPointF &ll : m_route) line << worldPixel(ll.x(), ll.y(), z) - origin;
        p.setPen(QPen(QColor(255, 255, 255, 200), 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
        p.setPen(QPen(QColor(30, 120, 230), 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
    }
    p.restore();
    // Haze towards the horizon
    QLinearGradient haze(0, horizon, 0, horizon + h * 0.18);
    QColor hz = sky;
    hz.setAlpha(170);
    haze.setColorAt(0, hz);
    hz.setAlpha(0);
    haze.setColorAt(1, hz);
    p.fillRect(QRectF(0, horizon, w, h * 0.18), haze);

    // Car marker at its ground position
    const QPointF c = proj.map(QPointF(0, 0));
    const double s = h / 14;
    QPolygonF arrow({QPointF(c.x(), c.y() - s), QPointF(c.x() + s * 0.7, c.y() + s * 0.6),
                     QPointF(c.x(), c.y() + s * 0.25), QPointF(c.x() - s * 0.7, c.y() + s * 0.6)});
    p.setPen(QPen(Qt::white, 3));
    p.setBrush(QColor(0, 150, 230));
    p.drawPolygon(arrow);

    // Attribution (OSM licence) and problems
    QFont f = p.font();
    f.setPixelSize(qMax(10, int(h / 34)));
    p.setFont(f);
    p.setPen(m_night ? QColor(200, 200, 200) : QColor(60, 60, 60));
    p.drawText(QRectF(0, 0, w - 6, h - 4), Qt::AlignRight | Qt::AlignBottom, QStringLiteral("© OpenStreetMap-Mitwirkende"));
    if (!m_lastError.isEmpty() && m_tiles.isEmpty()) {
        f.setPixelSize(qMax(12, int(h / 22)));
        f.setBold(true);
        p.setFont(f);
        p.setPen(m_night ? Qt::white : Qt::black);
        p.drawText(QRectF(0, horizon, w, h / 3), Qt::AlignCenter | Qt::TextWordWrap,
                   m_online ? QStringLiteral("Keine Kartenkacheln – Internet prüfen")
                            : QStringLiteral("Keine Kartenkacheln – OSM Scout Server starten und Karten laden"));
    }
}

void MapView::paint(QPainter &p, const QRect &r)
{
    // ~15 fps is plenty for a map; reuse the last layer in between.
    if (m_layer.size() != r.size() || !m_layerClock.isValid() || m_layerClock.elapsed() >= 66) {
        renderLayer(r.size());
        m_layerClock.start();
    }
    p.drawImage(r.topLeft(), m_layer);
}
