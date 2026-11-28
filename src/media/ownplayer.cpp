#include "ownplayer.h"

#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusObjectPath>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <gst/gst.h>

// ---------------------------------------------------------------- MPRIS

namespace {
const char kBusName[] = "org.mpris.MediaPlayer2.harbour-sailauto";

class MprisRoot : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ canQuit)
    Q_PROPERTY(bool CanRaise READ canRaise)
    Q_PROPERTY(bool HasTrackList READ hasTrackList)
    Q_PROPERTY(QString Identity READ identity)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry)
    Q_PROPERTY(QStringList SupportedUriSchemes READ schemes)
    Q_PROPERTY(QStringList SupportedMimeTypes READ mimes)
public:
    explicit MprisRoot(QObject *p) : QDBusAbstractAdaptor(p) {}
    bool canQuit() const { return true; }
    bool canRaise() const { return false; }
    bool hasTrackList() const { return false; }
    QString identity() const { return QStringLiteral("SailAuto Radio & Musik"); }
    QString desktopEntry() const { return QStringLiteral("harbour-sailauto"); }
    QStringList schemes() const { return {QStringLiteral("http"), QStringLiteral("https"), QStringLiteral("file")}; }
    QStringList mimes() const { return {QStringLiteral("audio/mpeg"), QStringLiteral("audio/ogg"), QStringLiteral("audio/flac")}; }
public slots:
    void Raise() {}
    void Quit() { static_cast<OwnPlayer *>(parent())->stop(); }
};

class MprisPlayer : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ status)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double Rate READ rate)
    Q_PROPERTY(double Volume READ volume)
    Q_PROPERTY(bool CanGoNext READ yes)
    Q_PROPERTY(bool CanGoPrevious READ yes)
    Q_PROPERTY(bool CanPlay READ yes)
    Q_PROPERTY(bool CanPause READ yes)
    Q_PROPERTY(bool CanSeek READ yes)
    Q_PROPERTY(bool CanControl READ yes)
public:
    explicit MprisPlayer(OwnPlayer *p) : QDBusAbstractAdaptor(p), m_p(p) {}
    QString status() const { return m_p->status(); }
    QVariantMap metadata() const { return m_p->metadata(); }
    qlonglong position() const { return m_p->positionMs() * 1000; }
    double rate() const { return 1.0; }
    double volume() const { return 1.0; }
    bool yes() const { return true; }
public slots:
    void Play() { m_p->play(); }
    void Pause() { m_p->pause(); }
    void PlayPause() { m_p->playPause(); }
    void Stop() { m_p->stop(); }
    void Next() { m_p->next(); }
    void Previous() { m_p->previous(); }
    void Seek(qlonglong us) { m_p->seek(us / 1000); }
    void SetPosition(const QDBusObjectPath &, qlonglong us) { m_p->setPosition(us / 1000); }
    void OpenUri(const QString &uri) { m_p->playStation({uri, uri, QString(), QString()}); }
private:
    OwnPlayer *m_p;
};

QString radioApi()
{
    const QString env = QString::fromLocal8Bit(qgetenv("SAILAUTO_RADIO_URL"));
    // all.api.radio-browser.info resolves to the mirrors
    return env.isEmpty() ? QStringLiteral("https://all.api.radio-browser.info") : env;
}
} // namespace

// ---------------------------------------------------------------- player

OwnPlayer::OwnPlayer(QObject *parent) : QObject(parent), m_net(new QNetworkAccessManager(this))
{
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);
    new MprisRoot(this);
    new MprisPlayer(this);
    m_busTimer.setInterval(200);
    connect(&m_busTimer, &QTimer::timeout, this, &OwnPlayer::pollBus);
}

OwnPlayer::~OwnPlayer()
{
    stop();
}

void OwnPlayer::startUri(const QString &uri)
{
    if (!m_pipe) {
        m_pipe = gst_element_factory_make("playbin", "sailauto-player");
        if (!m_pipe) {
            emit log(QStringLiteral("Player: GStreamer playbin fehlt"));
            return;
        }
        // Register on D-Bus only while we have something to play.
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.registerObject(QStringLiteral("/org/mpris/MediaPlayer2"), this);
        bus.registerService(QLatin1String(kBusName));
    }
    gst_element_set_state(m_pipe, GST_STATE_NULL);
    g_object_set(G_OBJECT(m_pipe), "uri", uri.toUtf8().constData(), nullptr);
    gst_element_set_state(m_pipe, GST_STATE_PLAYING);
    m_state = QStringLiteral("Playing");
    m_durationMs = 0;
    m_busTimer.start();
    emit changed();
}

void OwnPlayer::playStation(const RadioStation &s)
{
    m_isRadio = true;
    m_station = s;
    m_files.clear();
    m_index = -1;
    m_title = s.name;
    m_artist = s.info;
    m_album.clear();
    m_artFile.clear();
    if (!s.favicon.isEmpty()) fetchIcon(s.favicon);
    emit log(QStringLiteral("Player: Radio \"%1\"").arg(s.name));
    startUri(s.url);
}

void OwnPlayer::playFiles(const QStringList &files, int index)
{
    m_titles.clear();
    m_queueAlbum.clear();
    m_queueImage.clear();
    playEpisodes(files, QStringList(), index, QString(), QString(), QList<int>());
}

void OwnPlayer::playEpisodes(const QStringList &urls, const QStringList &titles, int index,
                             const QString &podcast, const QString &imageUrl, const QList<int> &seconds)
{
    if (index < 0 || index >= urls.size()) return;
    m_queueSeconds = seconds;
    m_files = urls;
    m_titles = titles;
    m_queueArtists.clear();
    m_queueCovers.clear();
    m_queueAlbum = podcast;
    m_queueImage = imageUrl;
    playIndex(index);
}

void OwnPlayer::playQueue(const QVector<QueueItem> &items, int index, const QString &album)
{
    if (index < 0 || index >= items.size()) return;
    m_files.clear();
    m_titles.clear();
    m_queueArtists.clear();
    m_queueCovers.clear();
    m_queueSeconds.clear();
    for (const QueueItem &it : items) {
        m_files << it.uri;
        m_titles << it.title;
        m_queueArtists << (it.artist.isEmpty() ? QStringLiteral(" ") : it.artist); // non-empty marks a queue
        m_queueCovers << it.cover;
        m_queueSeconds << it.seconds;
    }
    m_queueAlbum = album;
    m_queueImage.clear();
    playIndex(index);
}

void OwnPlayer::resolved(int index, const QString &url)
{
    if (index != m_index || index < 0 || index >= m_files.size()) return;
    if (url.isEmpty()) {
        emit log(QStringLiteral("Player: Song %1 nicht abspielbar, nächster").arg(index + 1));
        if (index + 1 < m_files.size()) playIndex(index + 1);
        else { m_state = QStringLiteral("Stopped"); emit changed(); }
        return;
    }
    m_files[index] = url;
    startUri(url);
}

void OwnPlayer::playIndex(int index)
{
    if (index < 0 || index >= m_files.size()) return;
    m_isRadio = false;
    m_index = index;
    const QString item = m_files[index];
    const bool remote = item.startsWith(QLatin1String("http")) || item.startsWith(QLatin1String("ytvid:"));
    m_artist = m_queueArtists.value(index).trimmed();
    m_artFile.clear();
    if (!m_queueArtists.isEmpty()) {
        const QFileInfo fi(item);
        m_title = m_titles.value(index, remote ? QString() : fi.completeBaseName());
        m_album = m_queueAlbum;
        const QString cover = m_queueCovers.value(index);
        if (cover.startsWith(QLatin1String("http"))) fetchIcon(cover, index);
        else if (!cover.isEmpty() && QFile::exists(cover)) m_artFile = cover;
    } else if (remote) {
        m_title = m_titles.value(index, QUrl(item).fileName());
        m_album = m_queueAlbum;
        if (!m_queueImage.isEmpty()) fetchIcon(m_queueImage);
    } else {
        const QFileInfo fi(item);
        m_title = fi.completeBaseName();
        m_album = fi.dir().dirName();
        for (const QString &c : {QStringLiteral("cover.jpg"), QStringLiteral("folder.jpg"), QStringLiteral("cover.png"), QStringLiteral("Cover.jpg")})
            if (QFile::exists(fi.dir().filePath(c))) { m_artFile = fi.dir().filePath(c); break; }
    }
    emit log(QStringLiteral("Player: %1 %2 von %3").arg(m_queueArtists.isEmpty() ? (remote ? QStringLiteral("Folge") : QStringLiteral("Datei"))
                                                                                 : QStringLiteral("Song"))
                 .arg(index + 1).arg(m_files.size()));
    if (item.startsWith(QLatin1String("ytvid:"))) {
        // stream address comes later (resolved); show the song meanwhile
        if (m_pipe) gst_element_set_state(m_pipe, GST_STATE_NULL);
        m_state = QStringLiteral("Playing");
        m_durationMs = 0;
        emit changed();
        emit needUrl(index, item.mid(6));
        return;
    }
    startUri(remote ? item : QUrl::fromLocalFile(item).toString());
}

void OwnPlayer::play()
{
    if (!m_pipe) return;
    gst_element_set_state(m_pipe, GST_STATE_PLAYING);
    m_state = QStringLiteral("Playing");
    emit changed();
}

void OwnPlayer::pause()
{
    if (!m_pipe) return;
    gst_element_set_state(m_pipe, GST_STATE_PAUSED);
    m_state = QStringLiteral("Paused");
    emit changed();
}

void OwnPlayer::playPause()
{
    if (m_state == QLatin1String("Playing")) pause(); else play();
}

void OwnPlayer::stop()
{
    m_busTimer.stop();
    if (m_pipe) {
        gst_element_set_state(m_pipe, GST_STATE_NULL);
        gst_object_unref(m_pipe);
        m_pipe = nullptr;
        QDBusConnection::sessionBus().unregisterService(QLatin1String(kBusName));
        QDBusConnection::sessionBus().unregisterObject(QStringLiteral("/org/mpris/MediaPlayer2"));
    }
    m_state = QStringLiteral("Stopped");
    emit changed();
}

void OwnPlayer::next()
{
    if (!m_isRadio && m_index + 1 < m_files.size()) playIndex(m_index + 1);
    else if (m_isRadio) { // next favorite
        const QVector<RadioStation> fav = favorites();
        for (int i = 0; i < fav.size(); ++i)
            if (fav[i].url == m_station.url) { playStation(fav[(i + 1) % fav.size()]); return; }
        if (!fav.isEmpty()) playStation(fav.first());
    }
}

void OwnPlayer::previous()
{
    if (!m_isRadio) {
        if (positionMs() > 3000 || m_index == 0) setPosition(0);
        else playIndex(m_index - 1);
    } else {
        const QVector<RadioStation> fav = favorites();
        for (int i = 0; i < fav.size(); ++i)
            if (fav[i].url == m_station.url) { playStation(fav[(i + fav.size() - 1) % fav.size()]); return; }
    }
}

void OwnPlayer::seek(qint64 offsetMs)
{
    setPosition(qMax<qint64>(0, positionMs() + offsetMs));
}

void OwnPlayer::setPosition(qint64 ms)
{
    if (!m_pipe || m_isRadio) return;
    gst_element_seek_simple(m_pipe, GST_FORMAT_TIME, GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
                            ms * GST_MSECOND);
}

qint64 OwnPlayer::positionMs() const
{
    gint64 pos = 0;
    if (m_pipe && gst_element_query_position(m_pipe, GST_FORMAT_TIME, &pos)) return pos / GST_MSECOND;
    return 0;
}

QString OwnPlayer::status() const
{
    return m_state;
}

QVariantMap OwnPlayer::metadata() const
{
    QVariantMap m;
    m.insert(QStringLiteral("mpris:trackid"), QVariant::fromValue(QDBusObjectPath(
        QStringLiteral("/org/sailauto/track/%1").arg(m_isRadio ? 0 : qMax(0, m_index)))));
    m.insert(QStringLiteral("xesam:title"), m_title);
    if (!m_artist.isEmpty()) m.insert(QStringLiteral("xesam:artist"), QStringList(m_artist));
    if (!m_album.isEmpty()) m.insert(QStringLiteral("xesam:album"), m_album);
    qint64 len = m_durationMs;
    if (len <= 0 && !m_isRadio) len = qint64(m_queueSeconds.value(m_index, 0)) * 1000; // from the feed
    if (len > 0 && !m_isRadio) m.insert(QStringLiteral("mpris:length"), len * 1000);
    if (!m_artFile.isEmpty()) m.insert(QStringLiteral("mpris:artUrl"), QUrl::fromLocalFile(m_artFile).toString());
    return m;
}

void OwnPlayer::pollBus()
{
    if (!m_pipe) return;
    GstBus *bus = gst_element_get_bus(m_pipe);
    while (GstMessage *msg = gst_bus_pop(bus)) {
        switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            if (!m_isRadio && m_index + 1 < m_files.size())
                playIndex(m_index + 1);
            else { m_state = QStringLiteral("Stopped"); emit changed(); }
            break;
        case GST_MESSAGE_ERROR: {
            GError *err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            emit log(QStringLiteral("Player: Fehler %1").arg(QString::fromUtf8(err ? err->message : "?")));
            if (err) g_error_free(err);
            m_state = QStringLiteral("Stopped");
            emit changed();
            break;
        }
        case GST_MESSAGE_TAG: {
            GstTagList *tags = nullptr;
            gst_message_parse_tag(msg, &tags);
            gchar *v = nullptr;
            bool c = false;
            if (gst_tag_list_get_string(tags, GST_TAG_TITLE, &v)) {
                // Radio: TITLE is "Artist - Song" (stream title); keep the station as album.
                const QString t = QString::fromUtf8(v);
                if (m_isRadio) {
                    m_album = m_station.name;
                    const int dash = t.indexOf(QStringLiteral(" - "));
                    if (dash > 0) { m_artist = t.left(dash); m_title = t.mid(dash + 3); }
                    else m_title = t.isEmpty() ? m_station.name : t;
                } else if (!t.isEmpty() && m_titles.isEmpty()) {
                    m_title = t;
                }
                g_free(v);
                c = true;
            }
            if (!m_isRadio && gst_tag_list_get_string(tags, GST_TAG_ARTIST, &v)) {
                m_artist = QString::fromUtf8(v);
                g_free(v);
                c = true;
            }
            if (!m_isRadio && gst_tag_list_get_string(tags, GST_TAG_ALBUM, &v)) {
                m_album = QString::fromUtf8(v);
                g_free(v);
                c = true;
            }
            gst_tag_list_unref(tags);
            if (c) emit changed();
            break;
        }
        default:
            break;
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gint64 dur = 0;
    if (!m_isRadio && m_durationMs <= 0 && gst_element_query_duration(m_pipe, GST_FORMAT_TIME, &dur) && dur > 0) {
        m_durationMs = dur / GST_MSECOND;
        emit changed();
    }
}

void OwnPlayer::fetchIcon(const QString &url, int forIndex)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/radio");
    QDir().mkpath(dir);
    const QString file = dir + QLatin1Char('/') + QString::fromLatin1(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex());
    if (QFile::exists(file)) { m_artFile = file; return; }
    QNetworkReply *r = m_net->get(QNetworkRequest(QUrl(url)));
    connect(r, &QNetworkReply::finished, this, [this, r, file, forIndex]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) return;
        QFile f(file);
        if (f.open(QIODevice::WriteOnly)) f.write(r->readAll());
        f.close();
        if (forIndex >= 0 && forIndex != m_index) return; // song changed meanwhile
        m_artFile = file;
        emit changed();
    });
}

// ---------------------------------------------------------------- radio directory

static QVector<RadioStation> parseStations(const QByteArray &json)
{
    QVector<RadioStation> out;
    for (const QJsonValue &v : QJsonDocument::fromJson(json).array()) {
        const QJsonObject o = v.toObject();
        RadioStation s;
        s.name = o.value(QStringLiteral("name")).toString().trimmed();
        s.url = o.value(QStringLiteral("url_resolved")).toString();
        if (s.url.isEmpty()) s.url = o.value(QStringLiteral("url")).toString();
        s.favicon = o.value(QStringLiteral("favicon")).toString();
        QStringList info;
        if (!o.value(QStringLiteral("country")).toString().isEmpty()) info << o.value(QStringLiteral("country")).toString();
        const QString tags = o.value(QStringLiteral("tags")).toString().section(QLatin1Char(','), 0, 1);
        if (!tags.isEmpty()) info << tags;
        if (o.value(QStringLiteral("bitrate")).toInt() > 0) info << QStringLiteral("%1 kbit/s").arg(o.value(QStringLiteral("bitrate")).toInt());
        s.info = info.join(QStringLiteral(" · "));
        if (!s.name.isEmpty() && !s.url.isEmpty()) out.append(s);
    }
    return out;
}

void OwnPlayer::searchStations(const QString &text)
{
    QUrl url(radioApi() + QStringLiteral("/json/stations/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("name"), text);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
    q.addQueryItem(QStringLiteral("hidebroken"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("order"), QStringLiteral("clickcount"));
    q.addQueryItem(QStringLiteral("reverse"), QStringLiteral("true"));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "SailAuto/1 (Sailfish OS)");
    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { emit stationsFound({}, r->errorString()); return; }
        emit stationsFound(parseStations(r->readAll()), QString());
    });
}

void OwnPlayer::topStations(const QString &countryCode)
{
    QUrl url(radioApi() + QStringLiteral("/json/stations/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("countrycode"), countryCode);
    q.addQueryItem(QStringLiteral("limit"), QStringLiteral("30"));
    q.addQueryItem(QStringLiteral("hidebroken"), QStringLiteral("true"));
    q.addQueryItem(QStringLiteral("order"), QStringLiteral("clickcount"));
    q.addQueryItem(QStringLiteral("reverse"), QStringLiteral("true"));
    url.setQuery(q);
    QNetworkRequest req(url);
    req.setRawHeader("User-Agent", "SailAuto/1 (Sailfish OS)");
    QNetworkReply *r = m_net->get(req);
    connect(r, &QNetworkReply::finished, this, [this, r]() {
        r->deleteLater();
        if (r->error() != QNetworkReply::NoError) { emit stationsFound({}, r->errorString()); return; }
        emit stationsFound(parseStations(r->readAll()), QString());
    });
}

QVector<RadioStation> OwnPlayer::favorites() const
{
    QVector<RadioStation> out;
    QSettings st;
    const int n = st.beginReadArray(QStringLiteral("radio"));
    for (int i = 0; i < n; ++i) {
        st.setArrayIndex(i);
        out.append({st.value(QStringLiteral("name")).toString(), st.value(QStringLiteral("url")).toString(),
                    st.value(QStringLiteral("favicon")).toString(), st.value(QStringLiteral("info")).toString()});
    }
    st.endArray();
    return out;
}

bool OwnPlayer::isFavorite(const QString &url) const
{
    for (const RadioStation &s : favorites())
        if (s.url == url) return true;
    return false;
}

void OwnPlayer::toggleFavorite()
{
    if (!m_isRadio || m_station.url.isEmpty()) return;
    QVector<RadioStation> fav = favorites();
    bool removed = false;
    for (int i = 0; i < fav.size(); ++i)
        if (fav[i].url == m_station.url) { fav.remove(i); removed = true; break; }
    if (!removed) fav.append(m_station);
    QSettings st;
    st.remove(QStringLiteral("radio"));
    st.beginWriteArray(QStringLiteral("radio"));
    for (int i = 0; i < fav.size(); ++i) {
        st.setArrayIndex(i);
        st.setValue(QStringLiteral("name"), fav[i].name);
        st.setValue(QStringLiteral("url"), fav[i].url);
        st.setValue(QStringLiteral("favicon"), fav[i].favicon);
        st.setValue(QStringLiteral("info"), fav[i].info);
    }
    st.endArray();
    emit log(removed ? QStringLiteral("Player: Favorit entfernt") : QStringLiteral("Player: als Favorit gemerkt"));
    emit changed();
}

QStringList OwnPlayer::musicRoots()
{
    QStringList roots;
    const QString music = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    if (QFileInfo(music).isDir()) roots << music;
    // SD card / USB: /run/media/<user>/<label>/Music (or the card itself)
    QDirIterator it(QStringLiteral("/run/media"), QDir::Dirs | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        const QString userDir = it.next();
        for (const QFileInfo &card : QDir(userDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString m = card.absoluteFilePath() + QStringLiteral("/Music");
            roots << (QFileInfo(m).isDir() ? m : card.absoluteFilePath());
        }
    }
    return roots;
}

#include "ownplayer.moc"
