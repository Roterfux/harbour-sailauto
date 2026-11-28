#include "appsources.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegExp>
#include <algorithm>
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

QStringList AppSources::appFiles(const QString &hint, int max)
{
    QStringList out;
    const QString home = QDir::homePath();
    for (const QString &base : {home + QStringLiteral("/.local/share"), home + QStringLiteral("/.config"), home + QStringLiteral("/.cache")}) {
        QDirIterator it(base, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && out.size() < max) {
            const QString f = it.next();
            if (f.mid(base.size()).contains(hint, Qt::CaseInsensitive)) out << f;
        }
    }
    return out;
}

bool AppSources::sailcastInstalled()
{
    return QFile::exists(QStringLiteral("/usr/share/applications/harbour-sailcast.desktop"))
        || !appFiles(QStringLiteral("sailcast"), 1).isEmpty();
}

namespace {
bool isFeedUrl(const QString &v)
{
    return (v.startsWith(QLatin1String("http://")) || v.startsWith(QLatin1String("https://"))) && !v.contains(QLatin1Char(' '))
        && !v.endsWith(QLatin1String(".mp3"), Qt::CaseInsensitive) && !v.endsWith(QLatin1String(".m4a"), Qt::CaseInsensitive)
        && !v.endsWith(QLatin1String(".jpg"), Qt::CaseInsensitive) && !v.endsWith(QLatin1String(".png"), Qt::CaseInsensitive);
}
} // namespace

QStringList AppSources::sailcastFeeds(QString *error, QString *where)
{
    // Known layout first: QML LocalStorage "SailcastDatabase", subscriptions(feed_url).
    QStringList out;
    const QString known = findLocalStorage(QStringLiteral("SailcastDatabase"));
    if (!known.isEmpty()) {
        for (const QStringList &r : query(known, QStringLiteral("SELECT feed_url FROM subscriptions ORDER BY position"), 1, nullptr))
            if (isFeedUrl(r[0])) out << r[0];
        if (!out.isEmpty()) {
            if (where) *where = QFileInfo(known).fileName() + QStringLiteral(" subscriptions.feed_url");
            return out;
        }
    }
    // Otherwise: every SQLite database / JSON / OPML file of the app. In SQLite,
    // tables that look like subscriptions (podcast, feed, subscr, channel, show)
    // with a column holding http(s) URLs (feed, rss, url, link, href).
    const QStringList files = appFiles(QStringLiteral("sailcast"));
    QStringList seen;
    for (const QString &f : files) {
        QFile fh(f);
        if (!fh.open(QIODevice::ReadOnly)) continue;
        const QByteArray head = fh.read(16);
        fh.close();
        seen << QFileInfo(f).fileName();
        if (head.startsWith("SQLite format 3")) {
            QString err;
            const QList<QStringList> tables = query(f, QStringLiteral("SELECT name FROM sqlite_master WHERE type='table'"), 1, &err);
            for (const QStringList &t : tables) {
                const QString tn = t[0];
                if (QRegExp(QStringLiteral("podcast|feed|subscr|channel|show"), Qt::CaseInsensitive).indexIn(tn) < 0) continue;
                if (QRegExp(QStringLiteral("episode|item|entry|download|chapter"), Qt::CaseInsensitive).indexIn(tn) >= 0) continue;
                const QList<QStringList> cols = query(f, QStringLiteral("PRAGMA table_info(\"%1\")").arg(tn), 2, nullptr);
                QStringList cand;
                for (const QStringList &c : cols)
                    if (QRegExp(QStringLiteral("feed|rss|url|link|href"), Qt::CaseInsensitive).indexIn(c[1]) >= 0
                        && QRegExp(QStringLiteral("image|cover|art|logo|icon|web|site"), Qt::CaseInsensitive).indexIn(c[1]) < 0)
                        cand << c[1];
                // feed-ish names first
                std::stable_sort(cand.begin(), cand.end(), [](const QString &x, const QString &y) {
                    return x.contains(QLatin1String("feed"), Qt::CaseInsensitive) > y.contains(QLatin1String("feed"), Qt::CaseInsensitive);
                });
                for (const QString &col : cand) {
                    QStringList urls;
                    for (const QStringList &r : query(f, QStringLiteral("SELECT \"%1\" FROM \"%2\"").arg(col, tn), 1, nullptr))
                        if (isFeedUrl(r[0]) && !urls.contains(r[0])) urls << r[0];
                    if (!urls.isEmpty()) {
                        if (where) *where = QStringLiteral("%1 %2.%3").arg(QFileInfo(f).fileName(), tn, col);
                        return urls;
                    }
                }
            }
        } else if (f.endsWith(QLatin1String(".json"), Qt::CaseInsensitive) || f.endsWith(QLatin1String(".opml"), Qt::CaseInsensitive)
                   || f.endsWith(QLatin1String(".xml"), Qt::CaseInsensitive)) {
            if (QFileInfo(f).size() > 4 * 1024 * 1024 || !fh.open(QIODevice::ReadOnly)) continue;
            const QString txt = QString::fromUtf8(fh.readAll());
            // OPML xmlUrl="...", JSON "feed...": "..." / "url": "..."
            QRegExp rx(QStringLiteral("(?:xmlUrl=\"|\"(?:feed[_a-zA-Z]*|rss[_a-zA-Z]*|url)\"\\s*:\\s*\")(https?://[^\"]+)\""));
            QStringList urls;
            for (int pos = 0; (pos = rx.indexIn(txt, pos)) >= 0; pos += rx.matchedLength()) {
                const QString u = rx.cap(1).replace(QStringLiteral("\\/"), QStringLiteral("/"));
                if (isFeedUrl(u) && !urls.contains(u)) urls << u;
            }
            if (!urls.isEmpty()) {
                if (where) *where = QFileInfo(f).fileName();
                return urls;
            }
        }
    }
    if (error)
        *error = seen.isEmpty() ? QStringLiteral("keine Sailcast-Daten gefunden")
                                : QStringLiteral("keine Abos in %1").arg(seen.mid(0, 6).join(QStringLiteral(", ")));
    return out;
}

void AppSources::loadPodcasts()
{
    if (m_pending > 0) return;
    QString err, where;
    const QStringList feeds = sailcastFeeds(&err, &where);
    m_info = feeds.isEmpty() ? err : QStringLiteral("%1 Abos aus %2").arg(feeds.size()).arg(where);
    emit log(QStringLiteral("Podcasts: ") + m_info);
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
