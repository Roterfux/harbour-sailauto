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
    QString title, url, date, duration;
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
    static QStringList sailcastFeeds(QString *error = nullptr);

    // Loads all Sailcast feeds (async) -> podcastsLoaded
    void loadPodcasts();
    const QVector<Podcast> &podcasts() const { return m_podcasts; }

signals:
    void podcastsLoaded();
    void log(const QString &line);

private:
    void parseFeed(Podcast &p, const QByteArray &xml);
    QNetworkAccessManager *m_net;
    QVector<Podcast> m_podcasts;
    int m_pending = 0;
};
