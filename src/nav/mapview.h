// Live map for the car's navigation page, tilted 3D view, heading up, car at
// the bottom centre. Self-drawn from vector tiles (OSM Scout Server offline,
// OpenFreeMap online), names placed upright on top (see VectorMap); raster
// tiles only with an own tile URL or when no vector tiles can be had.
// Position comes from the car's GPS (AAP LOCATION sensor), extrapolated
// between fixes with speed and bearing so the map moves smoothly.
// Drawn with QPainter (projective transform), no OpenGL needed.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointF>
#include <QSet>
#include <QString>
#include <QVector>

#include "vectormap.h"

class QNetworkAccessManager;
class QNetworkReply;
class QPainter;
class QTransform;

class MapView : public QObject
{
    Q_OBJECT
public:
    explicit MapView(QObject *parent = nullptr);

    // {z} {x} {y} {daylight} are replaced.
    void setTileUrl(const QString &url);
    QString tileUrl() const { return m_url; }
    void setNight(bool night);
    // Car GPS: degrees; bearing in degrees (<0 unknown); speed in m/s (<0 unknown)
    void updateFix(double lat, double lon, double bearing, double speed);
    bool hasFix() const { return m_haveFix; }
    // Draws the map into r (cached, redrawn at most ~15 times a second).
    void paint(QPainter &p, const QRect &r);
    // Route line (lat, lon); empty = none
    void setRoute(const QVector<QPointF> &latlon) { m_route = latlon; }
    // Areas covered by buttons/cards (map coordinates): no names there
    void setCovered(const QVector<QRectF> &rects) { m_covered = rects; }

signals:
    void log(const QString &line);

private:
    QPointF worldPixel(double lat, double lon, int z) const;
    void request(int z, int x, int y);
    void onReply(QNetworkReply *reply);
    void rebuildCanvas(int z, int cx, int cy);
    void renderLayer(const QSize &size);
    QString tileKey(int z, int x, int y) const;

    QNetworkAccessManager *m_net = nullptr;
    QString m_url;
    bool m_night = false;
    // Offline server unreachable -> openstreetmap.org tiles (only with the default URL)
    bool m_online = false;
    bool m_customUrl = false;
    QString tileTemplate() const;
    // Vector tiles: 0 = OSM Scout Server, 1 = OpenFreeMap (online), 2 = none (raster)
    int m_vtSource = 0;
    QString m_vtOnline;            // tile URL template from the OpenFreeMap TileJSON
    bool m_tilejsonBusy = false;
    int m_vtFails = 0, m_vtOk = 0;
    VectorMap m_vm;
    bool vectorMode() const { return !m_customUrl && m_vtSource < 2; }
    void requestVector(int z, int x, int y);
    void onVectorReply(QNetworkReply *reply, const QString &key);
    void fetchTileJson();
    // Place names overlay (OpenStreetMap via Overpass)
    struct Place { QString name; double lat, lon; int rank; };
    QVector<Place> m_places;
    QNetworkAccessManager *m_placesNet = nullptr;
    double m_placesLat = 1000, m_placesLon = 1000;
    bool m_placesBusy = false, m_placesLogged = false;
    void fetchPlaces(double lat, double lon);
    void drawPlaceNames(QPainter &p, const QTransform &canvasToScreen, int z, double w, double h, double horizon);

    // Position
    bool m_haveFix = false;
    double m_lat = 0, m_lon = 0, m_bearing = 0, m_speed = 0;
    QElapsedTimer m_fixAge;
    double m_prevLat = 0, m_prevLon = 0;
    double m_shownBearing = 0;

    // Tiles
    QHash<QString, QImage> m_tiles;          // key -> 256 px tile
    QSet<QString> m_pending;
    QHash<QString, qint64> m_failed;          // key -> ms when failed
    int m_zoom = 16;
    QString m_lastError;
    int m_okCount = 0;

    // Canvas: 5x5 tiles around the centre tile, north up
    QImage m_canvas;
    int m_canvasZ = -1, m_canvasX = 0, m_canvasY = 0; // top-left tile
    bool m_canvasDirty = true;
    bool m_canvasBusy = false;   // vector canvas being drawn in the background

    QVector<QPointF> m_route;
    QVector<QRectF> m_covered;

    // Rendered layer (rate limited)
    QImage m_layer;
    QElapsedTimer m_layerClock;
};
