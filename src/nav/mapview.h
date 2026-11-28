// Live map for the car's navigation page, tilted 3D view, heading up, car at
// the bottom centre. Self-drawn from vector tiles (OSM Scout Server offline,
// OpenFreeMap online), names placed upright on top (see VectorMap); raster
// tiles only with an own tile URL or when no vector tiles can be had.
// Position comes from the car's GPS (AAP LOCATION sensor), extrapolated
// between fixes with speed and bearing so the map moves smoothly.
// Drawn with QPainter (projective transform), no OpenGL needed.
#pragma once

#include <QColor>
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
    // Car's speed sensor (m/s): used to move on without GPS (tunnel)
    void setCarSpeed(double mps) { m_carSpeed = mps; m_carSpeedAge.start(); }
    // Estimated position now: last fix moved on with speed and heading; without
    // GPS (tunnel) along the route, else straight on. True while estimating
    // without GPS (dead reckoning).
    bool estimate(double *lat, double *lon, double *bearing, double *speed);
    // Draws the map into r (cached, redrawn at most ~15 times a second).
    void paint(QPainter &p, const QRect &r);
    // Route line (lat, lon); empty = none
    void setRoute(const QVector<QPointF> &latlon);
    // Traffic on the route: lines (lat, lon) with colour; one point = warning sign
    struct Mark { QVector<QPointF> line; QColor color; };
    void setTraffic(const QVector<Mark> &marks) { m_traffic = marks; }
    void setSpeedLimit(int kmh) { m_speedLimit = kmh; } // 0 = unknown
    // Load the map along the route ahead of time and keep it on the phone
    void prefetchRoute(const QVector<QPointF> &latlon);
    void setPrefetch(bool on) { m_prefetchOn = on; }
    // Settings: marker 0 = car, 1 = arrow; range -1 near, 0 normal, 1 far;
    // smooth: redraw every video frame (else ~15/s)
    void setMarker(int m) { m_marker = m; }
    void setRange(int r) { if (r != m_range) { m_range = r; m_canvasDirty = true; updateZoom(true); } }
    void setSmooth(bool on) { m_smooth = on; }
    void setAnisotropic(bool on) { m_anisoOn = on; }
    // Areas covered by buttons/cards (map coordinates): no names there
    void setCovered(const QVector<QRectF> &rects) { m_covered = rects; }

signals:
    void log(const QString &line);

private:
    QPointF worldPixel(double lat, double lon, int z) const;
    void request(int z, int x, int y);
    void onReply(QNetworkReply *reply);
    void rebuildCanvas(int z, int cx, int cy);
    void renderLayer(QPainter &p, const QSize &size);
    double m_viewW = 800;
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
    QVector<Mark> m_traffic;
    // GPU drawing of the canvas (mipmaps + anisotropic filtering)
    bool drawCanvasGL(QPainter &p, const QTransform &m, const QRectF &clipDevice);
    class QOpenGLTexture *m_tex = nullptr;
    class QOpenGLShaderProgram *m_prog = nullptr;
    class QOpenGLContext *m_glCtx = nullptr;
    qint64 m_texKey = -1;
    bool m_anisoOn = true, m_glFailed = false, m_anisoLogged = false;
    int m_speedLimit = 0;
    // Tile store on the phone (vector tiles, zoom 14) and route prefetch
    static QString storePath(int z, int x, int y);
    static void saveTile(int z, int x, int y, const QByteArray &data);
    void startPrefetch();
    static void trimStore();
    QNetworkAccessManager *m_prefetchNet = nullptr;
    QVector<QPoint> m_prefetch;
    int m_prefetchRunning = 0, m_prefetchDone = 0;
    bool m_prefetchOn = true;
    // Dead reckoning (world pixels at zoom 20)
    double m_carSpeed = -1;
    QElapsedTimer m_carSpeedAge, m_drClock;
    bool m_dr = false;
    QPointF m_drPos;
    double m_drBearing = 0;
    int m_drSeg = 0;
    QVector<QPointF> m_route20;          // route in world pixels at zoom 20
    double speedNow() const;
    QPointF estimate20(double *bearing);
    static void latLonOf(const QPointF &world20, double *lat, double *lon);
    int m_marker = 0, m_range = 0;
    bool m_smooth = true;
    void updateZoom(bool force);
    void drawCar(QPainter &p, const QTransform &proj, double scale);
    double targetZoom() const;
    double m_zoomF = 16.5;           // smooth zoom (speed), the canvas uses floor()
    void drawDestination(QPainter &p, const QTransform &canvasToScreen, int z, double h, double horizon);
    void drawSpeed(QPainter &p, double w, double h);

    // Rendered layer (rate limited)
    QImage m_layer;
    QElapsedTimer m_layerClock;
};
