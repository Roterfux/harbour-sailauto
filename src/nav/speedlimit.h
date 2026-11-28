// Speed limit of the road the car is on, from Valhalla's trace_attributes
// (OpenStreetMap maxspeed): offline OSM Scout Server first, else the FOSSGIS
// server. With a route: the limits along the next ~50 km of it, looked up by
// position. Without a route: the last positions are map-matched every 15 s.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QVector>

class QNetworkAccessManager;

class SpeedLimit : public QObject
{
    Q_OBJECT
public:
    explicit SpeedLimit(QObject *parent = nullptr);

    void setRoute(const QVector<QPointF> &latlon); // empty = no route
    void updatePosition(double lat, double lon, double speedMs);
    int limit() const { return m_limit; } // km/h, 0 = unknown

    static QString encodePolyline6(const QVector<QPointF> &latlon);

signals:
    void limitChanged(int kmh);
    void log(const QString &line);

private:
    void requestWindow();
    void requestSnap();
    void post(const QByteArray &json, bool online, int kind, int windowStart);
    void setLimit(int kmh);

    QNetworkAccessManager *m_net;
    QVector<QPointF> m_route;
    QVector<int> m_routeLimits;    // per route point (segment starting there), -1 unknown
    int m_windowEnd = 0;           // route points up to here are known / asked for
    int m_routeIdx = 0;
    bool m_busy = false, m_offlineFailed = false, m_logged = false;
    QVector<QPointF> m_trail;      // recent positions without a route
    QElapsedTimer m_snapClock, m_failClock; // no new tries for a minute after a failure
    int m_limit = 0;
};
