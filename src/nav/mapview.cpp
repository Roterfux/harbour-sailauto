#include "mapview.h"

#include <QDateTime>
#include <QPainterPath>
#include <QUrlQuery>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonArray>
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
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrentRun>

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
    // Online: openstreetmap.org standard tiles (no key needed; names are in the
    // picture, the upright overlay keeps the important ones readable).
    if (m_online) return QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png");
    return m_url;
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
    m_vm.setNight(night);
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

void MapView::requestVector(int z, int x, int y)
{
    const QString key = QStringLiteral("v/%1/%2/%3").arg(z).arg(x).arg(y);
    if (m_vm.has(z, x, y) || m_pending.contains(key) || m_pending.size() >= 8) return;
    const qint64 failedAt = m_failed.value(key, 0);
    if (failedAt && QDateTime::currentMSecsSinceEpoch() - failedAt < 30000) return;
    QString url = QString::fromLocal8Bit(qgetenv("SAILAUTO_VT_URL"));
    if (url.isEmpty()) {
        if (m_vtSource == 1 && m_vtOnline.isEmpty()) { fetchTileJson(); return; }
        url = m_vtSource == 1 ? m_vtOnline : QStringLiteral("http://127.0.0.1:8553/v1/mbgl/tile?z={z}&x={x}&y={y}");
    }
    url.replace(QStringLiteral("{z}"), QString::number(z)).replace(QStringLiteral("{x}"), QString::number(x))
        .replace(QStringLiteral("{y}"), QString::number(y));
    QNetworkRequest req{QUrl(url)};
    req.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    req.setRawHeader("User-Agent", "SailAuto/" APP_VERSION " (Sailfish OS car display)");
    QNetworkReply *r = m_net->get(req);
    r->setProperty("key", key);
    m_pending.insert(key);
}

void MapView::fetchTileJson()
{
    // OpenFreeMap: free OpenMapTiles vector tiles, no key. The tile URL holds a
    // version, so it comes from the TileJSON.
    if (m_tilejsonBusy) return;
    m_tilejsonBusy = true;
    if (!m_placesNet) m_placesNet = new QNetworkAccessManager(this);
    QNetworkRequest req(QUrl(QStringLiteral("https://tiles.openfreemap.org/planet")));
    req.setRawHeader("User-Agent", "SailAuto/" APP_VERSION " (Sailfish OS car display)");
    QNetworkReply *r = m_placesNet->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_tilejsonBusy = false;
        const QJsonArray tiles = QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("tiles")).toArray();
        if (r->error() != QNetworkReply::NoError || tiles.isEmpty()) {
            m_vtSource = 2; // no vector tiles at all: raster tiles from openstreetmap.org
            m_online = true;
            m_canvasDirty = true;
            emit log(QStringLiteral("Karte: keine Vektorkarte erreichbar (%1), nutze Bildkacheln")
                         .arg(r->error() != QNetworkReply::NoError ? r->errorString() : QStringLiteral("TileJSON leer")));
            return;
        }
        m_vtOnline = tiles.first().toString();
        m_canvasDirty = true;
        emit log(QStringLiteral("Karte: Online-Vektorkarte (OpenFreeMap)"));
    });
}

void MapView::onVectorReply(QNetworkReply *reply, const QString &key)
{
    const QStringList k = key.split(QLatin1Char('/'));
    const int z = k.value(1).toInt(), x = k.value(2).toInt(), y = k.value(3).toInt();
    const QByteArray data = reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray();
    QSharedPointer<VectorTile> tile(new VectorTile);
    const bool ok = reply->error() == QNetworkReply::NoError && (data.isEmpty() || tile->parse(data));
    if (ok) {
        // empty answer = empty tile (sea, outside the map)
        m_vm.insert(z, x, y, tile);
        if (m_vtOk++ == 0) emit log(QStringLiteral("Karte: Vektorkacheln kommen an (%1)")
                                        .arg(m_vtSource == 1 ? QStringLiteral("OpenFreeMap") : QStringLiteral("OSM Scout Server")));
        m_lastError.clear();
        m_vm.prune(z, x, y, 3);
        m_canvasDirty = true;
        return;
    }
    m_failed.insert(key, QDateTime::currentMSecsSinceEpoch());
    const QString err = reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("unlesbare Kachel");
    // OSM Scout Server missing or without vector maps: go online
    if (m_vtSource == 0 && m_vtOk == 0 && qgetenv("SAILAUTO_VT_URL").isEmpty() && ++m_vtFails >= 2) {
        m_vtSource = 1;
        m_failed.clear();
        emit log(QStringLiteral("Karte: OSM Scout Server ohne Vektorkarte (%1), versuche online").arg(err));
        fetchTileJson();
        return;
    }
    if (err != m_lastError) {
        m_lastError = err;
        emit log(QStringLiteral("Karte: Vektorkachel nicht geladen (%1)").arg(err));
    }
}

void MapView::request(int z, int x, int y)
{
    const QString key = tileKey(z, x, y);
    if (m_tiles.contains(key) || m_pending.contains(key) || m_pending.size() >= (m_online ? 10 : kMaxPending)) return;
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
    if (key.startsWith(QLatin1String("v/"))) {
        onVectorReply(reply, key);
        return;
    }
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
    const int x0 = cx - kGrid / 2, y0 = cy - kGrid / 2;
    if (vectorMode()) {
        // Drawn in a worker thread (a city tile takes a while on the phone);
        // the old canvas stays on screen meanwhile.
        if (m_canvasBusy) return;
        QVector<QVector<int>> missing;
        const VectorMap::Job job = m_vm.prepare(z, x0, y0, kGrid, &missing);
        const int dz = qMin(z, int(VectorMap::kMaxDataZoom));
        const double cxD = (cx + 0.5) / double(1 << (z - dz)), cyD = (cy + 0.5) / double(1 << (z - dz));
        std::sort(missing.begin(), missing.end(), [&](const QVector<int> &a, const QVector<int> &b) {
            return qAbs(a[1] + 0.5 - cxD) + qAbs(a[2] + 0.5 - cyD) < qAbs(b[1] + 0.5 - cxD) + qAbs(b[2] + 0.5 - cyD);
        });
        for (const QVector<int> &m : missing) requestVector(m[0], m[1], m[2]);
        if (job.srcs.isEmpty() && !m_canvas.isNull() && m_canvasZ == z) { m_canvasDirty = false; return; }
        m_canvasBusy = true;
        m_canvasDirty = false;
        struct Out { QImage img; VectorMap::Labels labels; VectorMap::Job job; qint64 ms; };
        auto *watcher = new QFutureWatcher<Out>(this);
        connect(watcher, &QFutureWatcher<Out>::finished, this, [this, watcher]() {
            const Out o = watcher->result();
            watcher->deleteLater();
            m_canvasBusy = false;
            if (o.job.night != m_night) { m_canvasDirty = true; return; }
            m_canvas = o.img;
            m_vm.setLabels(o.labels);
            m_canvasZ = o.job.z;
            m_canvasX = o.job.cx0;
            m_canvasY = o.job.cy0;
            static const bool dbg = !qgetenv("SAILAUTO_MAPTIME").isEmpty();
            if (dbg) qInfo("canvas %lld ms", o.ms);
        });
        watcher->setFuture(QtConcurrent::run([job]() {
            QElapsedTimer tm;
            tm.start();
            Out o;
            o.job = job;
            o.img = QImage(kGrid * kTile, kGrid * kTile, QImage::Format_RGB32);
            o.img.fill(VectorMap::backgroundFor(job.night));
            QPainter vp(&o.img);
            o.labels = VectorMap::draw(vp, job);
            vp.end();
            o.job.srcs.clear();
            o.ms = tm.elapsed();
            return o;
        }));
        return;
    }
    if (m_canvas.size() != QSize(kGrid * kTile, kGrid * kTile)) m_canvas = QImage(kGrid * kTile, kGrid * kTile, QImage::Format_RGB32);
    m_canvas.fill(m_night ? QColor(30, 34, 44) : QColor(225, 222, 214));
    QPainter p(&m_canvas);
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
        if (it != m_tiles.constEnd()) {
            p.drawImage(o.x() * kTile, o.y() * kTile, *it);
        } else {
            request(z, tx, ty);
            // Meanwhile: the matching quarter of the coarser tile, enlarged (no holes)
            auto parent = m_tiles.constFind(tileKey(z - 1, tx / 2, ty / 2));
            if (parent != m_tiles.constEnd())
                p.drawImage(QRect(o.x() * kTile, o.y() * kTile, kTile, kTile), *parent,
                            QRect((tx % 2) * kTile / 2, (ty % 2) * kTile / 2, kTile / 2, kTile / 2));
        }
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
    const double horizon = h * 0.03;
    QLinearGradient g(0, 0, 0, horizon);
    g.setColorAt(0, sky.darker(130));
    g.setColorAt(1, sky);
    p.fillRect(QRectF(0, 0, w, horizon + 1), g);
    p.fillRect(QRectF(0, horizon, w, h - horizon), vectorMode() ? m_vm.background() : m_night ? QColor(30, 34, 44) : QColor(225, 222, 214));

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
    // The vector canvas is drawn in the background: until the new one is there,
    // show the old one (maybe another zoom) at the right place.
    const int cz = m_canvasZ > 0 ? m_canvasZ : z;
    if (cz != z) car *= std::pow(2.0, cz - z);

    // Ground (car-centred, heading up, pixels) -> screen trapezoid (tilted view)
    // Gentle tilt: far edge only a bit narrower than the near edge.
    const double ahead = 620, behind = 80, half = 420; // small "behind" puts the car low on screen
    const QPolygonF ground({QPointF(-half, -ahead), QPointF(half, -ahead), QPointF(half, behind), QPointF(-half, behind)});
    const QPolygonF screen({QPointF(w * 0.5 - w * 0.85, horizon), QPointF(w * 0.5 + w * 0.85, horizon),
                            QPointF(w * 0.5 + w * 1.25, h * 1.02), QPointF(w * 0.5 - w * 1.25, h * 1.02)});
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
    if (m_online && m_night && !vectorMode()) // OSM tiles have no night style: darken gently
        p.fillRect(QRectF(0, 0, m_canvas.width(), m_canvas.height()), QColor(0, 0, 20, 70));
    // Route line in canvas coordinates (same transform as the tiles)
    if (m_route.size() > 1) {
        QPolygonF line;
        const QPointF origin(m_canvasX * kTile, m_canvasY * kTile);
        for (const QPointF &ll : m_route) line << worldPixel(ll.x(), ll.y(), cz) - origin;
        p.setPen(QPen(QColor(255, 255, 255, 200), 16, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
        p.setPen(QPen(QColor(30, 120, 230), 10, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.drawPolyline(line);
    }
    p.restore();
    // Haze towards the horizon
    QLinearGradient haze(0, horizon, 0, horizon + h * 0.06);
    QColor hz = sky;
    hz.setAlpha(110);
    haze.setColorAt(0, hz);
    hz.setAlpha(0);
    haze.setColorAt(1, hz);
    p.fillRect(QRectF(0, horizon, w, h * 0.06), haze);
    // Car marker at its ground position
    const QPointF c = proj.map(QPointF(0, 0));
    const double s = h / 14;
    // Names upright on top (readable, unlike names baked into tilted tiles)
    if (vectorMode())
        m_vm.drawLabels(p, t * rot * proj, QRectF(0, 0, w, h), horizon,
                        {QRectF(c.x() - s * 1.6, c.y() - s * 1.6, s * 3.2, s * 3.2), QRectF(w * 0.72, 0, w * 0.28, h * 0.2)});
    else
        drawPlaceNames(p, t * rot * proj, cz, w, h, horizon);
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
    p.drawText(QRectF(0, 0, w - 6, h - 4), Qt::AlignRight | Qt::AlignBottom, vectorMode() ? QStringLiteral("© OpenMapTiles © OpenStreetMap-Mitwirkende")
                                                                                      : QStringLiteral("© OpenStreetMap-Mitwirkende"));
    if (!m_lastError.isEmpty() && (vectorMode() ? m_vm.count() == 0 : m_tiles.isEmpty())) {
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
    // As many pictures as the car takes (every video tick); the slow part (the
    // vector canvas) is drawn in the background anyway.
    if (m_layer.size() != r.size() || !m_layerClock.isValid() || m_layerClock.elapsed() >= 10) {
        QElapsedTimer tm;
        tm.start();
        renderLayer(r.size());
        m_layerClock.start();
        static const bool dbgRender = !qgetenv("SAILAUTO_MAPTIME").isEmpty();
        if (dbgRender && tm.elapsed() > 30) qInfo("map render %lld ms", tm.elapsed());
    }
    p.drawImage(r.topLeft(), m_layer);
}

void MapView::fetchPlaces(double lat, double lon)
{
    if (m_placesBusy) return;
    if (m_placesLat < 900 && qAbs(lat - m_placesLat) < 0.05 && qAbs(lon - m_placesLon) < 0.08) return;
    m_placesBusy = true;
    m_placesLat = lat;
    m_placesLon = lon;
    const double dlat = 0.15, dlon = 0.25; // ~17 x 17 km
    const QString q = QStringLiteral("[out:json][timeout:15];node[place~\"^(city|town|village|suburb|hamlet)$\"][name](%1,%2,%3,%4);out 300;")
                          .arg(lat - dlat, 0, 'f', 3).arg(lon - dlon, 0, 'f', 3).arg(lat + dlat, 0, 'f', 3).arg(lon + dlon, 0, 'f', 3);
    QString base = QString::fromLocal8Bit(qgetenv("SAILAUTO_OVERPASS_URL"));
    if (base.isEmpty()) base = QStringLiteral("https://overpass-api.de/api/interpreter");
    QUrl url(base);
    QUrlQuery uq;
    uq.addQueryItem(QStringLiteral("data"), q);
    url.setQuery(uq);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "SailAuto/" APP_VERSION " (Sailfish OS car display)");
    if (!m_placesNet) m_placesNet = new QNetworkAccessManager(this); // m_net routes every reply to the tile handler
    QNetworkReply *r = m_placesNet->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        m_placesBusy = false;
        if (r->error() != QNetworkReply::NoError) {
            m_placesLat = 1000; // try again later
            return;
        }
        QVector<Place> places;
        for (const QJsonValue &v : QJsonDocument::fromJson(r->readAll()).object().value(QStringLiteral("elements")).toArray()) {
            const QJsonObject o = v.toObject();
            const QJsonObject tags = o.value(QStringLiteral("tags")).toObject();
            const QString type = tags.value(QStringLiteral("place")).toString();
            const int rank = type == QLatin1String("city") ? 4 : type == QLatin1String("town") ? 3
                             : type == QLatin1String("village") || type == QLatin1String("suburb") ? 2 : 1;
            places.append({tags.value(QStringLiteral("name")).toString(), o.value(QStringLiteral("lat")).toDouble(),
                           o.value(QStringLiteral("lon")).toDouble(), rank});
        }
        std::sort(places.begin(), places.end(), [](const Place &a, const Place &b) { return a.rank > b.rank; });
        m_places = places;
        if (!m_placesLogged) {
            m_placesLogged = true;
            emit log(QStringLiteral("Karte: %1 Ortsnamen geladen").arg(places.size()));
        }
    });
}

void MapView::drawPlaceNames(QPainter &p, const QTransform &canvasToScreen, int z, double w, double h, double horizon)
{
    fetchPlaces(m_lat, m_lon);
    if (m_places.isEmpty()) return;
    const QPointF origin(m_canvasX * kTile, m_canvasY * kTile);
    QVector<QRectF> taken;
    QFont f = p.font();
    f.setBold(true);
    for (const Place &pl : m_places) {
        const QPointF sp = canvasToScreen.map(worldPixel(pl.lat, pl.lon, z) - origin);
        if (sp.y() < horizon + h * 0.04 || sp.y() > h - h * 0.08 || sp.x() < 0 || sp.x() > w) continue;
        // bigger for cities and when near (lower on screen)
        const double nearF = 0.7 + 0.5 * (sp.y() - horizon) / (h - horizon);
        f.setPixelSize(qMax(11, int(h / 26 * (0.8 + 0.2 * pl.rank) * nearF)));
        p.setFont(f);
        const QFontMetricsF fm(f);
        QRectF box(0, 0, fm.width(pl.name) + 8, fm.height());
        box.moveCenter(sp);
        bool clash = false;
        for (const QRectF &t : taken)
            if (t.intersects(box.adjusted(-6, -4, 6, 4))) { clash = true; break; }
        if (clash) continue;
        taken.append(box);
        QPainterPath path;
        path.addText(box.left() + 4, box.top() + fm.ascent(), f, pl.name);
        p.setPen(QPen(m_night ? QColor(10, 12, 20, 220) : QColor(255, 255, 255, 230), 4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);                       // halo
        p.setPen(Qt::NoPen);
        p.setBrush(m_night ? QColor(225, 230, 240) : QColor(40, 44, 56));
        p.drawPath(path);
        if (taken.size() >= 12) break;
    }
}
