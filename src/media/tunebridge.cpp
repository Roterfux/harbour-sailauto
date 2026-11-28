#include "tunebridge.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegExp>
#include <QSet>

namespace {
QJsonDocument readJson(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QJsonDocument();
    return QJsonDocument::fromJson(f.readAll());
}

QString dataBase()
{
    const QString x = QString::fromLocal8Bit(qgetenv("XDG_DATA_HOME"));
    return x.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : x;
}
QString cacheBase()
{
    const QString x = QString::fromLocal8Bit(qgetenv("XDG_CACHE_HOME"));
    return x.isEmpty() ? QDir::homePath() + QStringLiteral("/.cache") : x;
}
} // namespace

TuneBridgeSource::TuneBridgeSource(QObject *parent)
    : QObject(parent),
      m_data(dataBase() + QStringLiteral("/harbour-tunebridge")),
      m_cache(cacheBase() + QStringLiteral("/harbour-tunebridge/audio"))
{
}

bool TuneBridgeSource::installed()
{
    return QFileInfo(dataBase() + QStringLiteral("/harbour-tunebridge")).isDir();
}

TuneBridgeSource::Track TuneBridgeSource::toTrack(const QJsonObject &o)
{
    Track t;
    t.id = o.value(QStringLiteral("id")).toString();
    t.title = o.value(QStringLiteral("title")).toString();
    t.artists = o.value(QStringLiteral("artists")).toString();
    t.album = o.value(QStringLiteral("album")).toString();
    t.cover = o.value(QStringLiteral("cover")).toString();
    t.videoId = o.value(QStringLiteral("videoId")).toString();
    t.seconds = int(o.value(QStringLiteral("ms")).toDouble() / 1000);
    return t;
}

QVector<TuneBridgeSource::Track> TuneBridgeSource::readTracks(const QString &file) const
{
    QVector<Track> out;
    for (const QJsonValue &v : readJson(m_data + QLatin1Char('/') + file).array())
        if (v.isObject()) out.append(toTrack(v.toObject()));
    return out;
}

QString TuneBridgeSource::videoIdOf(const Track &t) const
{
    if (!t.videoId.isEmpty()) return t.videoId;
    if (t.id.startsWith(QLatin1String("yt:"))) return t.id.mid(3);
    // Spotify song: the YouTube match TuneBridge found for it
    const QFileInfo mi(m_data + QStringLiteral("/matches.json"));
    const qint64 stamp = mi.exists() ? mi.lastModified().toMSecsSinceEpoch() : 0;
    if (stamp != m_matchesStamp) {
        m_matches = readJson(mi.filePath()).object();
        m_matchesStamp = stamp;
    }
    return m_matches.value(t.id).toObject().value(QStringLiteral("videoId")).toString();
}

QString TuneBridgeSource::savedFile(const QString &videoId) const
{
    if (videoId.isEmpty()) return QString();
    const QString f = m_cache + QLatin1Char('/') + videoId + QStringLiteral(".m4a");
    return QFile::exists(f) ? f : QString();
}

QString TuneBridgeSource::coverFile(const QString &url) const
{
    if (url.isEmpty()) return QString();
    // TuneBridge keeps covers as cover-<sha1(url)[:16]>.jpg
    const QString f = m_cache + QStringLiteral("/cover-")
                      + QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1).toHex().left(16))
                      + QStringLiteral(".jpg");
    return QFile::exists(f) ? f : url;
}

bool TuneBridgeSource::playable(const Track &t) const
{
    const QString vid = videoIdOf(t);
    return !savedFile(vid).isEmpty() || (!vid.isEmpty() && QFile::exists(m_data + QStringLiteral("/bin/yt-dlp")));
}

QVector<TuneBridgeSource::Track> TuneBridgeSource::allKnown() const
{
    QVector<Track> all = readTracks(QStringLiteral("favourites.json")) + readTracks(QStringLiteral("history.json"));
    const QStringList lists = QDir(m_data).entryList(QStringList() << QStringLiteral("list-*.json"), QDir::Files, QDir::Time);
    for (const QString &f : lists) all += readTracks(f);
    QVector<Track> out;
    QSet<QString> seen;
    for (const Track &t : all)
        if (!t.id.isEmpty() && !seen.contains(t.id)) {
            seen.insert(t.id);
            out.append(t);
        }
    return out;
}

QVector<TuneBridgeSource::Entry> TuneBridgeSource::lists() const
{
    QVector<Entry> out;
    int saved = 0;
    for (const Track &t : allKnown())
        if (!savedFile(videoIdOf(t)).isEmpty()) ++saved;
    if (saved) out.append({QStringLiteral("saved"), QString(), QStringLiteral("Auf dem Telefon"),
                           QStringLiteral("%1 Songs, ohne Internet").arg(saved), saved});
    const int favs = readTracks(QStringLiteral("favourites.json")).size();
    if (favs) out.append({QStringLiteral("favs"), QString(), QStringLiteral("Favoriten"), QStringLiteral("%1 Songs").arg(favs), favs});
    const int recent = readTracks(QStringLiteral("history.json")).size();
    if (recent) out.append({QStringLiteral("recent"), QString(), QStringLiteral("Zuletzt gehört"), QStringLiteral("%1 Songs").arg(recent), recent});
    const QJsonObject lib = readJson(m_data + QStringLiteral("/library.json")).object();
    const int liked = lib.value(QStringLiteral("liked")).toInt();
    if (liked) out.append({QStringLiteral("liked"), QStringLiteral("liked"), QStringLiteral("Lieblingssongs"),
                           QStringLiteral("%1 Songs (Spotify)").arg(liked), liked});
    QJsonArray entries = lib.value(QStringLiteral("lists")).toArray();
    for (const QJsonValue &v : readJson(m_data + QStringLiteral("/links.json")).array()) entries.append(v);
    for (const QJsonValue &v : entries) {
        const QJsonObject o = v.toObject();
        Entry e;
        e.kind = o.value(QStringLiteral("kind")).toString();
        e.id = o.value(QStringLiteral("id")).toString();
        e.name = o.value(QStringLiteral("name")).toString();
        e.count = o.value(QStringLiteral("count")).toInt();
        QString sub = o.value(QStringLiteral("sub")).toString();
        if (sub.isEmpty()) sub = e.kind == QLatin1String("album") ? QStringLiteral("Album") : QStringLiteral("Playlist");
        if (e.count > 0) sub += QStringLiteral(" · %1 Songs").arg(e.count);
        e.sub = sub;
        if (!e.id.isEmpty() && !e.name.isEmpty()) out.append(e);
    }
    return out;
}

QVector<TuneBridgeSource::Track> TuneBridgeSource::tracks(const QString &kind, const QString &id, QString *note) const
{
    QVector<Track> out;
    if (kind == QLatin1String("favs")) out = readTracks(QStringLiteral("favourites.json"));
    else if (kind == QLatin1String("recent")) out = readTracks(QStringLiteral("history.json"));
    else if (kind == QLatin1String("saved")) {
        for (const Track &t : allKnown())
            if (!savedFile(videoIdOf(t)).isEmpty()) out.append(t);
    } else {
        // the copy TuneBridge keeps of a list it has opened
        QString safe = id;
        safe.remove(QRegExp(QStringLiteral("[^A-Za-z0-9_]")));
        out = readTracks(QStringLiteral("list-%1-%2.json").arg(kind, safe));
        if (out.isEmpty()) {
            const QStringList any = QDir(m_data).entryList(QStringList() << QStringLiteral("list-%1-*.json").arg(kind), QDir::Files);
            if (kind == QLatin1String("liked") && !any.isEmpty()) out = readTracks(any.first());
        }
        if (out.isEmpty() && note) *note = QStringLiteral("Liste einmal in TuneBridge öffnen, dann ist sie hier");
    }
    return out;
}

QVector<TuneBridgeSource::Track> TuneBridgeSource::search(const QString &text) const
{
    QVector<Track> out;
    const QStringList words = text.simplified().split(QLatin1Char(' '), QString::SkipEmptyParts);
    if (words.isEmpty()) return out;
    for (const Track &t : allKnown()) {
        const QString hay = t.title + QLatin1Char(' ') + t.artists + QLatin1Char(' ') + t.album;
        bool all = true;
        for (const QString &w : words)
            if (!hay.contains(w, Qt::CaseInsensitive)) { all = false; break; }
        if (all) out.append(t);
        if (out.size() >= 60) break;
    }
    return out;
}

QVector<QueueItem> TuneBridgeSource::queue(const QVector<Track> &tracks) const
{
    QVector<QueueItem> out;
    for (const Track &t : tracks) {
        const QString vid = videoIdOf(t);
        const QString file = savedFile(vid);
        QueueItem q;
        q.uri = !file.isEmpty() ? file : vid.isEmpty() ? QString() : QStringLiteral("ytvid:") + vid;
        q.title = t.title;
        q.artist = t.artists;
        q.cover = coverFile(t.cover);
        q.seconds = t.seconds;
        if (!q.uri.isEmpty()) out.append(q);
    }
    return out;
}

bool TuneBridgeSource::readCarSearch(const QString &query, qint64 notBeforeMs, QVector<Track> *tracks) const
{
    const QFileInfo fi(m_data + QStringLiteral("/car-search.json"));
    if (!fi.exists() || fi.lastModified().toMSecsSinceEpoch() < notBeforeMs) return false;
    const QJsonObject o = readJson(fi.filePath()).object();
    if (o.value(QStringLiteral("query")).toString() != query) return false;
    tracks->clear();
    for (const QJsonValue &v : o.value(QStringLiteral("tracks")).toArray())
        if (v.isObject()) tracks->append(toTrack(v.toObject()));
    return true;
}

void TuneBridgeSource::resolve(int index, const QString &videoId)
{
    const QString ytdlp = m_data + QStringLiteral("/bin/yt-dlp");
    if (!QFile::exists(ytdlp)) {
        emit log(QStringLiteral("TuneBridge: yt-dlp fehlt, nur gespeicherte Songs spielbar"));
        emit urlReady(index, QString());
        return;
    }
    if (m_proc) {
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->deleteLater();
    }
    m_proc = new QProcess(this);
    m_procIndex = index;
    QProcess *proc = m_proc;
    connect(proc, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this,
            [this, proc, index](int code, QProcess::ExitStatus) {
                const QString url = QString::fromUtf8(proc->readAllStandardOutput()).split(QLatin1Char('\n')).value(0).trimmed();
                const QString err = QString::fromUtf8(proc->readAllStandardError()).trimmed().split(QLatin1Char('\n')).last();
                proc->deleteLater();
                if (m_proc == proc) m_proc = nullptr;
                if (code != 0 || !url.startsWith(QLatin1String("http")))
                    emit log(QStringLiteral("TuneBridge: Song nicht ladbar (%1)").arg(err.left(120)));
                emit urlReady(index, url.startsWith(QLatin1String("http")) ? url : QString());
            });
    // TuneBridge's own yt-dlp is a Python zipapp; python3 is on every Sailfish phone
    proc->start(ytdlp, QStringList() << QStringLiteral("-f") << QStringLiteral("bestaudio[ext=m4a]/bestaudio")
                                     << QStringLiteral("-g") << QStringLiteral("--no-playlist") << QStringLiteral("--no-warnings")
                                     << QStringLiteral("https://music.youtube.com/watch?v=") + videoId);
}
