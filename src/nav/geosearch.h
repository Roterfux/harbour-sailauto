// Address search for the car's destination list: OSM Scout Server geocoder
// (offline, http://127.0.0.1:8553/v1/search), biased to the car's position.
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

struct GeoResult {
    QString title, region;
    double lat = 0, lon = 0;
};

class GeoSearch : public QObject
{
    Q_OBJECT
public:
    explicit GeoSearch(QObject *parent = nullptr);
    void setBias(double lat, double lon) { m_lat = lat; m_lon = lon; m_bias = true; }
    // OSM Scout Server first (offline); if it is not there: Photon (online,
    // photon.komoot.io, the search Pure Maps uses as well).
    void search(const QString &text);

signals:
    // error empty = ok (results may still be empty)
    void finished(const QString &text, const QVector<GeoResult> &results, const QString &error);

private:
    void searchPhoton(const QString &text, const QString &firstError);
    QNetworkAccessManager *m_net;
    double m_lat = 0, m_lon = 0;
    bool m_bias = false;
};
