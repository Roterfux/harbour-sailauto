// Weather for the car screen from Open-Meteo (free, no key). Position: the
// car's GPS (or the parked position), rounded to 0.1° (~10 km) before it
// leaves the phone. Refreshed every 20 min or after ~15 km.
#pragma once

#include <QDate>
#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

struct WeatherData {
    bool valid = false;
    double temp = 0;
    int code = 0;      // WMO weather code
    bool day = true;
    struct Hour { int hour; double temp; int code; };
    struct Day { QDate date; int code; double max, min; };
    QVector<Hour> hours;  // next 6 hours
    QVector<Day> days;    // today + 2
};

class WeatherClient : public QObject
{
    Q_OBJECT
public:
    explicit WeatherClient(QObject *parent = nullptr);
    void setPosition(double lat, double lon);
    const WeatherData &data() const { return m_data; }
    static QString describe(int code);   // German text for a WMO code

signals:
    void changed();
    void log(const QString &line);

private:
    void fetch();
    QNetworkAccessManager *m_net;
    double m_lat = 0, m_lon = 0;
    bool m_havePos = false;
    double m_fetchedLat = 1000, m_fetchedLon = 1000;
    QElapsedTimer m_lastFetch;
    bool m_busy = false;
    bool m_logged = false;
    WeatherData m_data;
};
