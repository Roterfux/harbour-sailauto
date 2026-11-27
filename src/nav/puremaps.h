// Follows Pure Maps' navigation over its D-Bus API
// (io.github.rinigus.PureMaps, /io/github/rinigus/PureMaps/navigator) and
// provides what the car screen shows: next maneuver, distances, times.
// Polls once a second while Pure Maps runs.
#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

struct NavState {
    bool app = false;        // Pure Maps is running
    bool running = false;    // navigation active
    bool hasRoute = false;
    bool alongRoute = false;
    QString icon;            // maneuver icon name, e.g. "turn-left"
    QString manDist, manTime, street, narrative;
    QString destDist, destTime, destEta;
    int progress = 0;
    int roundaboutExit = 0;
    bool operator==(const NavState &o) const;
    bool operator!=(const NavState &o) const { return !(*this == o); }
};

class PureMapsClient : public QObject
{
    Q_OBJECT
public:
    explicit PureMapsClient(QObject *parent = nullptr);
    void start();
    void stop();
    const NavState &state() const { return m_state; }
    // Maneuver icon from the Pure Maps installation (white variant), cached.
    // Null image if not found; the car screen then draws its own arrow.
    QImage icon(const QString &name, int size);
    void stopNavigation();
    // Destination: Pure Maps search (shows results on the phone; pick one and
    // "Route" there). Returns false if Pure Maps is not running.
    bool search(const QString &text);
    // Start the route Pure Maps has (hasRoute && !running).
    bool startNavigation();

signals:
    void changed();
    void log(const QString &line);

private:
    void poll();
    QTimer m_timer;
    NavState m_state;
    QHash<QString, QImage> m_icons;
};
