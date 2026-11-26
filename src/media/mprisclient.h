// Finds the phone's music player over MPRIS (session D-Bus,
// org.mpris.MediaPlayer2.*) and exposes what is playing plus play/pause,
// next and previous. Prefers a player that is currently playing.
// Polls once a second: simple and good enough for a car display.
#pragma once

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

public slots:
    void playPause();
    void play();
    void pause();
    void next();
    void previous();
    // Spulen: relative (ms, may be negative) or absolute (fraction 0..1 of the track).
    void seek(qint64 offsetMs);
    void seekToFraction(double fraction);

signals:
    void changed();
    void log(const QString &line);

private:
    void poll();
    void call(const QString &method);

    QTimer m_timer;
    QString m_service, m_identity;
    QString m_title, m_artist, m_album, m_artUrl, m_trackId;
    QImage m_art;
    bool m_playing = false;
    qint64 m_positionMs = 0, m_lengthMs = 0;
};
