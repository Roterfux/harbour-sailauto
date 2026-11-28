// Traffic warnings on the own route from TomTom (Traffic Incidents API, own
// free key of the user, set on the phone). Only while navigating: every
// 3 minutes the stretch ahead (up to ~60 km) is asked for jams, accidents,
// closures; incidents lying on the route are kept with their distance ahead.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QTimer>
#include <QVector>

class QNetworkAccessManager;

struct TrafficIncident {
    int category = 0;          // TomTom iconCategory: 1 accident, 6 jam, 8 closed, 7 lane closed, 9 works, 14 broken down ...
    int magnitude = 0;         // 0 unknown, 1 minor, 2 moderate, 3 major, 4 undefined (closures)
    QString text, from, to, roads;
    double lengthM = 0, delayS = 0;
    double aheadM = -1;        // distance along the route to its start, -1 = not on the route
    double startAlong = -1, endAlong = -1; // where it lies on the route (m from the start)
    QVector<QPointF> line;     // (lat, lon)
};

class TrafficService : public QObject
{
    Q_OBJECT
public:
    explicit TrafficService(QObject *parent = nullptr);

    void setKey(const QString &key);
    bool enabled() const { return !m_key.isEmpty(); }
    // Route (lat, lon); empty = navigation ended
    void setRoute(const QVector<QPointF> &latlon);
    void updatePosition(double lat, double lon);
    // On the route ahead, nearest first
    const QVector<TrafficIncident> &ahead() const { return m_ahead; }
    int requestsThisMonth() const;

    static QString categoryName(int cat);

signals:
    void changed();
    void log(const QString &line);

private:
    void query();
    void parse(const QByteArray &json);
    void refreshAhead();
    double distToRoute(const QPointF &ll, int *seg, double *along) const;

    QNetworkAccessManager *m_net;
    QString m_key;
    QTimer m_timer;
    QVector<QPointF> m_route;          // lat, lon
    QVector<double> m_cum;             // metres along the route
    double m_posAlong = 0;             // where we are on the route (m)
    int m_posSeg = 0;
    QVector<TrafficIncident> m_all, m_ahead, m_shown;
    bool m_busy = false;
    QString m_lastError;
};
