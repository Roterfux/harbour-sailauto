// TuneBridge (Spotify / YouTube Music app for Sailfish) has no lists over
// MPRIS, so SailAuto reads its files (read only, TuneBridge writes them):
//   ~/.local/share/harbour-tunebridge/
//     favourites.json, history.json           [track, ...]
//     library.json                            {"lists": [entry, ...], "liked": n}
//     links.json                              [entry, ...]
//     list-<kind>-<id>.json                   [track, ...] last loaded songs of a list
//     matches.json                            {spotifyId: {videoId, ...}}
//     bin/yt-dlp                              its stream resolver
//   ~/.cache/harbour-tunebridge/audio/<videoId>.m4a   songs kept on the phone
// track = {id, title, artists, album, cover, ms, videoId?}; entry = {kind, id, name, sub, cover, count}
// Songs play in SailAuto's own player: kept songs from the file, others via
// TuneBridge's yt-dlp (needs internet).
#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QVector>

#include "ownplayer.h"

class QProcess;

class TuneBridgeSource : public QObject
{
    Q_OBJECT
public:
    explicit TuneBridgeSource(QObject *parent = nullptr);

    struct Entry { QString kind, id, name, sub; int count = 0; };
    struct Track { QString id, title, artists, album, cover, videoId; int seconds = 0; };

    static bool installed();
    // Lists like TuneBridge's start page: on the phone, favourites, recent, playlists, albums
    QVector<Entry> lists() const;
    // Songs of a list; *note says why it is empty (not opened in TuneBridge yet, ...)
    QVector<Track> tracks(const QString &kind, const QString &id, QString *note = nullptr) const;
    // Search in all songs TuneBridge knows (offline)
    QVector<Track> search(const QString &text) const;

    // Queue for the own player (uri: file or "ytvid:<id>")
    QVector<QueueItem> queue(const QVector<Track> &tracks) const;
    bool playable(const Track &t) const;
    // Stream URL for a video id via TuneBridge's yt-dlp -> urlReady(index, url)
    void resolve(int index, const QString &videoId);

signals:
    void urlReady(int index, const QString &url);
    void log(const QString &line);

private:
    QString videoIdOf(const Track &t) const;
    QString savedFile(const QString &videoId) const;
    QString coverFile(const QString &url) const;
    static Track toTrack(const QJsonObject &o);
    QVector<Track> readTracks(const QString &file) const;
    QVector<Track> allKnown() const;

    QString m_data, m_cache;
    mutable QJsonObject m_matches;
    mutable qint64 m_matchesStamp = -1;
    QProcess *m_proc = nullptr;
    int m_procIndex = -1;
};
