// SailAuto's own player for the car: internet radio (Radio Browser station
// directory) and music files from the phone's music folder. Plays with
// GStreamer playbin (sound goes the normal way: PulseAudio -> car) and shows
// up as an MPRIS player "SailAuto", so the car's music page, steering wheel
// keys and progress bar work with it like with any other player.
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <QVector>

typedef struct _GstElement GstElement;
class QNetworkAccessManager;

struct RadioStation {
    QString name, url, favicon, info; // info: country, tags, bitrate
};

class OwnPlayer : public QObject
{
    Q_OBJECT
public:
    explicit OwnPlayer(QObject *parent = nullptr);
    ~OwnPlayer() override;

    // Radio
    void playStation(const RadioStation &s);
    void searchStations(const QString &text);        // -> stationsFound
    void topStations(const QString &countryCode);    // -> stationsFound
    QVector<RadioStation> favorites() const;
    bool isFavorite(const QString &url) const;
    void toggleFavorite();                            // current station
    bool playingRadio() const { return m_isRadio && m_pipe; }
    QString currentStationUrl() const { return m_station.url; }

    // Music files
    static QStringList musicRoots();
    void playFiles(const QStringList &files, int index);
    // Podcast episodes (stream URLs, seekable), queue for next/previous
    void playEpisodes(const QStringList &urls, const QStringList &titles, int index,
                      const QString &podcast, const QString &imageUrl);

    // Transport (also via MPRIS)
    void play();
    void pause();
    void playPause();
    void stop();
    void next();
    void previous();
    void seek(qint64 offsetMs);
    void setPosition(qint64 ms);

    // State for MPRIS
    QString status() const;            // Playing / Paused / Stopped
    QVariantMap metadata() const;
    qint64 positionMs() const;

signals:
    void stationsFound(const QVector<RadioStation> &stations, const QString &error);
    void changed();
    void log(const QString &line);

private:
    void startUri(const QString &uri);
    void pollBus();
    void fetchIcon(const QString &url);

    GstElement *m_pipe = nullptr;
    QTimer m_busTimer;
    QNetworkAccessManager *m_net = nullptr;
    bool m_isRadio = false;
    RadioStation m_station;
    QStringList m_files;         // local files or http URLs (episodes)
    QStringList m_titles;        // episode titles (empty for files)
    QString m_queueAlbum, m_queueImage;
    int m_index = -1;
    QString m_title, m_artist, m_album, m_artFile;
    QString m_state = QStringLiteral("Stopped");
    qint64 m_durationMs = 0;
};
