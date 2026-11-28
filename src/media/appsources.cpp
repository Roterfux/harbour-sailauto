#include "appsources.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>
#include <QUrl>
#include <QXmlStreamReader>

AppSources::AppSources(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this)) {}

QString AppSources::findLocalStorage(const QString &dbName)
{
    // QML LocalStorage: .../QML/OfflineStorage/Databases/<md5>.sqlite + .ini with "Name=<dbName>".
    // Sailjail apps: ~/.local/share/<org>/<app>/..., older ones ~/.local/share/<app>/<app>/...
    QDirIterator it(QDir::homePath() + QStringLiteral("/.local/share"), QStringList() << QStringLiteral("*.ini"),
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString ini = it.next();
        if (!ini.contains(QStringLiteral("/OfflineStorage/Databases/"))) continue;
        QFile f(ini);
        if (!f.open(QIODevice::ReadOnly)) continue;
        QTextStream in(&f);
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.startsWith(QLatin1String("Name=")) && line.mid(5) == dbName) {
                const QString db = ini.left(ini.size() - 4) + QStringLiteral(".sqlite");
                if (QFile::exists(db)) return db;
            }
        }
    }
    return QString();
}

namespace {
QList<QStringList> query(const QString &path, const QString &sql, int cols, QString *error)
{
    QList<QStringList> rows;
    const QString conn = QStringLiteral("appsrc-") + path;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(path);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (!db.open()) {
            if (error) *error = db.lastError().text();
        } else {
            QSqlQuery q(db);
            if (!q.exec(sql) && error) *error = q.lastError().text();
            while (q.next()) {
                QStringList r;
                for (int i = 0; i < cols; ++i) r << q.value(i).toString();
                rows << r;
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return rows;
}
} // namespace

QVector<RadioStation> AppSources::sailwaveStations(QString *error)
{
    QVector<RadioStation> out;
    const QString db = findLocalStorage(QStringLiteral("sailwave"));
    if (db.isEmpty()) {
        if (error) *error = QStringLiteral("SailWave-Datenbank nicht gefunden");
        return out;
    }
    for (const QStringList &r : query(db, QStringLiteral("SELECT name, stream FROM MyStations ORDER BY name"), 2, error))
        if (!r[1].isEmpty()) out.append({r[0], r[1], QString(), QStringLiteral("SailWave")});
    return out;
}

QStringList AppSources::sailcastFeeds(QString *error)
{
    QStringList out;
    const QString db = findLocalStorage(QStringLiteral("SailcastDatabase"));
    if (db.isEmpty()) {
        if (error) *error = QStringLiteral("Sailcast-Datenbank nicht gefunden");
        return out;
    }
    for (const QStringList &r : query(db, QStringLiteral("SELECT feed_url FROM subscriptions ORDER BY position"), 1, error))
        if (!r[0].isEmpty()) out << r[0];
    return out;
}

void AppSources::loadPodcasts()
{
    if (m_pending > 0) return;
    QString err;
    const QStringList feeds = sailcastFeeds(&err);
    if (!err.isEmpty()) emit log(QStringLiteral("Podcasts: ") + err);
    m_podcasts.clear();
    for (const QString &f : feeds) {
        Podcast p;
        p.feed = f;
        p.title = QUrl(f).host();
        m_podcasts.append(p);
    }
    if (m_podcasts.isEmpty()) { emit podcastsLoaded(); return; }
    m_pending = m_podcasts.size();
    for (int i = 0; i < m_podcasts.size(); ++i) {
        QNetworkRequest req{QUrl(m_podcasts[i].feed)};
        req.setRawHeader("User-Agent", "SailAuto (Sailfish OS)");
        req.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        QNetworkReply *r = m_net->get(req);
        connect(r, &QNetworkReply::finished, this, [this, r, i]() {
            r->deleteLater();
            if (i < m_podcasts.size()) {
                if (r->error() != QNetworkReply::NoError) m_podcasts[i].error = r->errorString();
                else parseFeed(m_podcasts[i], r->readAll());
                m_podcasts[i].loaded = true;
            }
            if (--m_pending == 0) {
                int eps = 0;
                for (const Podcast &p : m_podcasts) eps += p.episodes.size();
                emit log(QStringLiteral("Podcasts: %1 Abos, %2 Folgen").arg(m_podcasts.size()).arg(eps));
                emit podcastsLoaded();
            }
        });
    }
}

void AppSources::parseFeed(Podcast &p, const QByteArray &xml)
{
    // RSS 2.0: channel/title, channel/image/url (or itunes:image href),
    // item/title, item/enclosure@url, item/pubDate, itunes:duration
    QXmlStreamReader x(xml);
    bool inItem = false;
    Episode e;
    while (!x.atEnd()) {
        x.readNext();
        if (x.isStartElement()) {
            const QStringRef n = x.name();
            if (n == QLatin1String("item")) { inItem = true; e = Episode(); }
            else if (n == QLatin1String("title")) {
                const QString t = x.readElementText().trimmed();
                if (inItem) e.title = t; else if (p.title.isEmpty() || p.title == QUrl(p.feed).host()) p.title = t;
            } else if (n == QLatin1String("enclosure") && inItem) {
                e.url = x.attributes().value(QStringLiteral("url")).toString();
            } else if (n == QLatin1String("pubDate") && inItem) {
                e.date = x.readElementText().trimmed().section(QLatin1Char(' '), 1, 3); // "27 Nov 2026"
            } else if (n == QLatin1String("duration") && inItem) {
                // itunes:duration: "3600", "59:30" or "1:05:00" -> seconds
                const QStringList parts = x.readElementText().trimmed().split(QLatin1Char(':'));
                int secs = 0;
                for (const QString &pt : parts) secs = secs * 60 + pt.toInt();
                e.seconds = secs;
                const int min = (secs + 30) / 60;
                e.duration = secs <= 0 ? QString() : min < 60 ? QStringLiteral("%1 min").arg(qMax(1, min))
                                                               : QStringLiteral("%1 h %2 min").arg(min / 60).arg(min % 60, 2, 10, QLatin1Char('0'));
            } else if (n == QLatin1String("image") && !inItem && p.image.isEmpty()) {
                const QString href = x.attributes().value(QStringLiteral("href")).toString();
                if (!href.isEmpty()) p.image = href;
            } else if (n == QLatin1String("url") && !inItem && p.image.isEmpty()) {
                p.image = x.readElementText().trimmed();
            }
        } else if (x.isEndElement() && x.name() == QLatin1String("item")) {
            inItem = false;
            if (!e.url.isEmpty()) p.episodes.append(e);
            if (p.episodes.size() >= 50) break; // newest first in feeds; enough for the car
        }
    }
}
