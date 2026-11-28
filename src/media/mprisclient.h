// Finds the phone's music player over MPRIS (session D-Bus,
// org.mpris.MediaPlayer2.*) and exposes what is playing plus play/pause,
// next and previous. Prefers a player that is currently playing.
// Polls once a second: simple and good enough for a car display.
#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QSet>
#include <QVector>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

class MprisClient : public QObject
{
    Q_OBJECT
public:
    explicit MprisClient(QObject *parent = nullptr);

    void start();
    void stop();

    bool hasPlayer() const { return !m_service.isEmpty(); }
    QString playerName() const { return m_identity.isEmpty() ? m_service : m_identity; }
    QString title() const { return m_title; }
    QString artist() const { return m_artist; }
    QString album() const { return m_album; }
    QImage art() const { return m_art; }
    bool playing() const { return m_playing; }
    qint64 positionMs() const { return m_positionMs; }
    qint64 lengthMs() const { return m_lengthMs; }
    // Browsing (optional MPRIS interfaces Playlists and TrackList).
    struct Entry { QString id, title, sub; bool playlist; };
    QVector<Entry> browse(QString *info);       // playlists first, then tracks
    void activate(const Entry &e);

public slots:
    void playPause();
    void play();
    void pause();
    void next();
    void previous();
    // Spulen: relative (ms, may be negative) or absolute (fraction 0..1 of the track).
    void seek(qint64 offsetMs);
    void seekToFraction(double fraction);
    // An app was started from the car: show its player once it appears.
    void setPreferred(const QString &desktopId);
    // The player of the app started from the car is now the shown one.
    bool showingPreferred() const { return m_showingPreferred; }
    // Close players that have not played for idleMs (Quit, else SIGTERM).
    // Returns "Name beendet/gestoppt" per closed player.
    QStringList quitIdle(qint64 idleMs);
    // End the shown player (Quit, else SIGTERM). Returns its name or empty.
    QString quitCurrent();
    QString currentDesktopEntry() const { return m_desktopOf.value(m_service); }

signals:
    void changed();
    void log(const QString &line);
    // Playback ended (stopped at the end, or the app closed while playing)
    void finished();

private:
    void poll();
    void call(const QString &method);

    QTimer m_timer;
    QString m_preferred;
    bool m_showingPreferred = false;
    QString m_lastStatus, m_lastStatusService;
    // Length from the audio itself when the player reports none (xesam:url)
    QHash<QString, qint64> m_probedLen;   // url -> ms (0 = probing / unknown)
    void probeLength(const QString &url);
    QSet<QString> m_metaLogged;
    QElapsedTimer m_preferredSince;
    QHash<QString, QString> m_desktopOf; // service -> DesktopEntry
    QString m_service, m_identity;
    QString m_title, m_artist, m_album, m_artUrl, m_trackId;
    QImage m_art;
    bool m_playing = false;
    qint64 m_positionMs = 0, m_lengthMs = 0;
    // own position clock for players that do not report Position
    QElapsedTimer m_ownClock;
    qint64 m_ownBaseMs = 0, m_reportedLast = -1;
    int m_stuckPolls = 0;
    bool m_posWarned = false;
    QHash<QString, qint64> m_lastActive; // player -> last time seen playing (ms)
    QSet<QString> m_reportLogged;
};
