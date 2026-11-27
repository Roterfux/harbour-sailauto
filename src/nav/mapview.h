// Live map for the car's navigation page: raster tiles (default: OSM Scout
// Server on the phone), tilted 3D view, heading up, car at the bottom centre.
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

class QNetworkAccessManager;
class QNetworkReply;
class QPainter;

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

    // Rendered layer (rate limited)
    QImage m_layer;
    QElapsedTimer m_layerClock;
};
