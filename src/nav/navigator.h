// SailAuto's own turn-by-turn navigation (instead of Pure Maps).
// Route: Valhalla API, offline via OSM Scout Server (localhost:8553/v2/route)
// or online (FOSSGIS Valhalla server). Guidance: the position is matched to the
// route shape; next maneuver, distances, times, arrival, re-routing when off
// the route. Produces the same NavState the car screen already shows.
#pragma once

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

#include "puremaps.h" // NavState

class QNetworkAccessManager;

class Navigator : public QObject
{
    Q_OBJECT
public:
    explicit Navigator(QObject *parent = nullptr);

    void setDestination(const QString &title, double lat, double lon);
    void updatePosition(double lat, double lon, double bearing, double speed);
    void stop();
    bool active() const { return m_active; }
    const NavState &state() const { return m_state; }
    // Route shape as (lat, lon) for the map
    const QVector<QPointF> &shape() const { return m_shape; }

signals:
    void changed();
    void routeChanged();
    void log(const QString &line);
    // Short spoken-style text before a maneuver (for a later voice output)
    void announce(const QString &text);

public:
    static QString fmtDist(double m);
    static QString fmtTime(double s);

private:
    struct Maneuver {
        int type = 0;
        QString instruction, street, verbal;
        int beginIndex = 0;
        int roundaboutExit = 0;
        double alongM = 0;   // distance from route start to the maneuver
        QString lanes;       // see NavState::lanes
    };
    void requestRoute(bool reroute);
    void parseRoute(const QByteArray &json);
    // Lanes: the same route once more in OSRM format (only that has turn lanes)
    void requestLanes(const QByteArray &json, bool online);
    void parseLanes(const QByteArray &json);
    void update();
    static QString iconFor(int type, int exitCount);

    QNetworkAccessManager *m_net;
    bool m_active = false;
    bool m_busy = false;
    QString m_destTitle;
    double m_destLat = 0, m_destLon = 0;
    bool m_havePos = false;
    double m_lat = 0, m_lon = 0, m_bearing = -1, m_speed = 0;

    QVector<QPointF> m_shape;      // lat, lon
    QVector<double> m_cum;         // metres from start per shape point
    QVector<Maneuver> m_man;
    double m_totalM = 0, m_totalS = 0;
    int m_seg = 0;                 // matched segment
    double m_along = 0;            // metres along the route
    int m_offCount = 0;
    QElapsedTimer m_lastReroute;
    int m_announced = -1;          // maneuver index announced last
    int m_arrivedTicks = 0;
    bool m_usedOnline = false;
    NavState m_state;
};
