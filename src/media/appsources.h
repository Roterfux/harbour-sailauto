// Lists from other Sailfish audio apps that do not share them over MPRIS:
// read straight from their local (QML LocalStorage) databases, played by
// SailAuto's own player.
//  - SailWave (internet radio): table MyStations(name, stream) in DB "sailwave"
//  - Sailcast (podcasts): table subscriptions(feed_url) in DB "SailcastDatabase";
//    the episodes come from the RSS feeds themselves.
#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include "ownplayer.h"

class QNetworkAccessManager;

struct Episode {
    QString title, url, date, duration; // duration: "40 min"
    int seconds = 0;
};

struct Podcast {
    QString feed, title, image;
    QVector<Episode> episodes;
    bool loaded = false;
    QString error;
};

class AppSources : public QObject
{
    Q_OBJECT
public:
    explicit AppSources(QObject *parent = nullptr);

    // Path of a QML LocalStorage database by its name, empty if not found.
    static QString findLocalStorage(const QString &dbName);
    static QVector<RadioStation> sailwaveStations(QString *error = nullptr);
    // Sailcast's subscriptions: looks through all its databases / files for feed
    // URLs (the storage differs between versions). where: what was found.
    static QStringList sailcastFeeds(QString *error = nullptr, QString *where = nullptr);
    static bool sailcastInstalled();
    // Data files of an app (path contains hint, case-insensitive) below ~/.local/share, ~/.config, ~/.cache
    static QStringList appFiles(const QString &hint, int max = 60);

    // Loads all Sailcast feeds (async) -> podcastsLoaded
    void loadPodcasts();
    const QVector<Podcast> &podcasts() const { return m_podcasts; }
    QString lastInfo() const { return m_info; } // where the feeds came from / why none

signals:
    void podcastsLoaded();
    void log(const QString &line);

private:
    void parseFeed(Podcast &p, const QByteArray &xml);
    QNetworkAccessManager *m_net;
    QVector<Podcast> m_podcasts;
    int m_pending = 0;
    QString m_info;
};
