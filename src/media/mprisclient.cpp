#include "mprisclient.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDBusVariant>
#include <QDBusMetaType>
#include <QUrl>
#include <QVariantMap>

namespace {
const char kPlayerIface[] = "org.mpris.MediaPlayer2.Player";
const char kRootIface[] = "org.mpris.MediaPlayer2";
const char kPath[] = "/org/mpris/MediaPlayer2";

QVariant getProperty(const QString &service, const char *iface, const char *name)
{
    QDBusMessage m = QDBusMessage::createMethodCall(service, QLatin1String(kPath),
                                                    QStringLiteral("org.freedesktop.DBus.Properties"),
                                                    QStringLiteral("Get"));
    m << QLatin1String(iface) << QLatin1String(name);
    QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 500);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty()) return QVariant();
    return r.arguments().first().value<QDBusVariant>().variant();
}

QVariantMap toMap(const QVariant &v)
{
    if (v.canConvert<QDBusArgument>()) {
        QVariantMap map;
        const QDBusArgument arg = v.value<QDBusArgument>();
        arg >> map;
        return map;
    }
    return v.toMap();
}

QString joinList(const QVariant &v)
{
    if (v.type() == QVariant::StringList) return v.toStringList().join(QStringLiteral(", "));
    return v.toString();
}
} // namespace

MprisClient::MprisClient(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, &MprisClient::poll);
}

void MprisClient::start()
{
    poll();
    m_timer.start();
}

void MprisClient::stop()
{
    m_timer.stop();
}

void MprisClient::poll()
{
    QDBusConnectionInterface *bus = QDBusConnection::sessionBus().interface();
    if (!bus) return;
    const QStringList names = bus->registeredServiceNames().value();
    QStringList players;
    for (const QString &n : names)
        if (n.startsWith(QLatin1String("org.mpris.MediaPlayer2."))) players << n;

    // Prefer the app just started from the car (for 30 s, or while it plays
    // or nothing else does); else the one that is playing; else keep the
    // current one; else the first.
    QString chosen, playingOne, preferredOne;
    for (const QString &p : players) {
        if (!m_desktopOf.contains(p))
            m_desktopOf.insert(p, getProperty(p, kRootIface, "DesktopEntry").toString());
        const bool playing = getProperty(p, kPlayerIface, "PlaybackStatus").toString() == QLatin1String("Playing");
        if (playing && playingOne.isEmpty()) playingOne = p;
        if (!m_preferred.isEmpty()
            && (m_desktopOf.value(p) == m_preferred
                || p.contains(QString(m_preferred).remove(QStringLiteral("harbour-")), Qt::CaseInsensitive)))
            preferredOne = p;
        if (playing && p == preferredOne) playingOne = p;
    }
    if (!preferredOne.isEmpty()
        && (m_preferredSince.elapsed() < 30000 || playingOne.isEmpty() || playingOne == preferredOne)) {
        chosen = preferredOne;
        m_showingPreferred = true;
    } else {
        if (!preferredOne.isEmpty()) m_preferred.clear(); // user went back to another player
        chosen = playingOne;
    }
    if (chosen.isEmpty() && players.contains(m_service)) chosen = m_service;
    if (chosen.isEmpty() && !players.isEmpty()) chosen = players.first();

    if (chosen != m_service) {
        m_service = chosen;
        m_identity = chosen.isEmpty() ? QString() : getProperty(chosen, kRootIface, "Identity").toString();
        emit log(chosen.isEmpty() ? QStringLiteral("Musik: kein Player gefunden")
                                  : QStringLiteral("Musik: Player \"%1\" (%2), insgesamt %3 Player")
                                        .arg(m_identity, chosen).arg(players.size()));
    }
    if (m_service.isEmpty()) {
        if (!m_title.isEmpty() || m_playing) {
            m_title.clear(); m_artist.clear(); m_album.clear(); m_art = QImage(); m_playing = false;
            emit changed();
        }
        return;
    }

    const QVariantMap md = toMap(getProperty(m_service, kPlayerIface, "Metadata"));
    const QString title = md.value(QStringLiteral("xesam:title")).toString();
    const QString artist = joinList(md.value(QStringLiteral("xesam:artist")));
    const QString album = md.value(QStringLiteral("xesam:album")).toString();
    const QString artUrl = md.value(QStringLiteral("mpris:artUrl")).toString();
    const qint64 lenMs = md.value(QStringLiteral("mpris:length")).toLongLong() / 1000;
    const QVariant tid = md.value(QStringLiteral("mpris:trackid"));
    m_trackId = tid.canConvert<QDBusObjectPath>() ? tid.value<QDBusObjectPath>().path() : tid.toString();
    const bool playing = getProperty(m_service, kPlayerIface, "PlaybackStatus").toString() == QLatin1String("Playing");
    const qint64 posMs = getProperty(m_service, kPlayerIface, "Position").toLongLong() / 1000;

    if (artUrl != m_artUrl) {
        m_artUrl = artUrl;
        m_art = QImage();
        const QUrl u(artUrl);
        if (u.isLocalFile()) m_art.load(u.toLocalFile());
        if (!m_art.isNull()) m_art = m_art.scaled(256, 256, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const bool trackChanged = title != m_title || artist != m_artist;
    if (trackChanged || playing != m_playing || album != m_album || lenMs != m_lengthMs || posMs != m_positionMs) {
        if (trackChanged && !title.isEmpty())
            emit log(QStringLiteral("Musik: %1 – %2%3").arg(artist, title)
                         .arg(lenMs > 0 ? QString() : posMs > 0 ? QStringLiteral(" (Player meldet keine Länge)")
                                                                : QStringLiteral(" (Player meldet weder Länge noch Position)")));
        m_title = title; m_artist = artist; m_album = album;
        m_lengthMs = lenMs; m_positionMs = posMs; m_playing = playing;
        emit changed();
    }
}

void MprisClient::call(const QString &method)
{
    if (m_service.isEmpty()) {
        emit log(QStringLiteral("Musik: kein Player für \"%1\"").arg(method));
        return;
    }
    QDBusMessage m = QDBusMessage::createMethodCall(m_service, QLatin1String(kPath),
                                                    QLatin1String(kPlayerIface), method);
    QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 1000);
    emit log(QStringLiteral("Musik: %1 -> %2%3").arg(method, playerName())
                 .arg(r.type() == QDBusMessage::ErrorMessage ? QStringLiteral(" (Fehler: %1)").arg(r.errorMessage()) : QString()));
    QTimer::singleShot(150, this, &MprisClient::poll);
}

void MprisClient::playPause() { call(QStringLiteral("PlayPause")); }
void MprisClient::play() { call(QStringLiteral("Play")); }
void MprisClient::pause() { call(QStringLiteral("Pause")); }
void MprisClient::next() { call(QStringLiteral("Next")); }
void MprisClient::previous() { call(QStringLiteral("Previous")); }

void MprisClient::seek(qint64 offsetMs)
{
    if (m_service.isEmpty()) {
        emit log(QStringLiteral("Musik: kein Player zum Spulen"));
        return;
    }
    QDBusMessage m = QDBusMessage::createMethodCall(m_service, QLatin1String(kPath),
                                                    QLatin1String(kPlayerIface), QStringLiteral("Seek"));
    m << static_cast<qlonglong>(offsetMs * 1000);
    QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 1000);
    if (r.type() == QDBusMessage::ErrorMessage)
        emit log(QStringLiteral("Musik: Spulen %1 s -> %2 (Fehler: %3)")
                     .arg(offsetMs / 1000).arg(playerName(), r.errorMessage()));
    // Show the new position right away (the next poll corrects it).
    m_positionMs = qBound<qint64>(0, m_positionMs + offsetMs, m_lengthMs > 0 ? m_lengthMs : m_positionMs + offsetMs);
    emit changed();
    QTimer::singleShot(150, this, &MprisClient::poll);
}

void MprisClient::seekToFraction(double fraction)
{
    if (m_service.isEmpty() || m_lengthMs <= 0) return;
    const qint64 target = static_cast<qint64>(qBound(0.0, fraction, 1.0) * m_lengthMs);
    emit log(QStringLiteral("Musik: Spulen auf %1:%2 -> %3")
                 .arg(target / 60000).arg((target / 1000) % 60, 2, 10, QLatin1Char('0')).arg(playerName()));
    if (m_trackId.startsWith(QLatin1Char('/'))) {
        QDBusMessage m = QDBusMessage::createMethodCall(m_service, QLatin1String(kPath),
                                                        QLatin1String(kPlayerIface), QStringLiteral("SetPosition"));
        m << QVariant::fromValue(QDBusObjectPath(m_trackId)) << static_cast<qlonglong>(target * 1000);
        QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 1000);
        if (r.type() != QDBusMessage::ErrorMessage) {
            m_positionMs = target;
            emit changed();
            QTimer::singleShot(150, this, &MprisClient::poll);
            return;
        }
        emit log(QStringLiteral("Musik: SetPosition geht nicht (%1), versuche Seek").arg(r.errorMessage()));
    }
    // No track id (or refused): relative seek from the current position.
    const qint64 now = getProperty(m_service, kPlayerIface, "Position").toLongLong() / 1000;
    m_positionMs = now;
    seek(target - now);
}

void MprisClient::setPreferred(const QString &desktopId)
{
    m_preferred = desktopId;
    m_showingPreferred = false;
    m_preferredSince.start();
    QTimer::singleShot(1500, this, &MprisClient::poll);
}

QVector<MprisClient::Entry> MprisClient::browse(QString *info)
{
    QVector<Entry> out;
    if (m_service.isEmpty()) {
        if (info) *info = QStringLiteral("kein Player");
        return out;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    // Playlists: GetPlaylists(u index, u max, s order, b reverse) -> a(oss)
    int playlists = 0, tracks = 0;
    {
        QDBusMessage m = QDBusMessage::createMethodCall(m_service, QLatin1String(kPath),
                                                        QStringLiteral("org.mpris.MediaPlayer2.Playlists"),
                                                        QStringLiteral("GetPlaylists"));
        m << 0u << 200u << QStringLiteral("Alphabetical") << false;
        const QDBusMessage r = bus.call(m, QDBus::Block, 1500);
        if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
            const QDBusArgument arg = r.arguments().first().value<QDBusArgument>();
            arg.beginArray();
            while (!arg.atEnd()) {
                QDBusObjectPath id;
                QString name, icon;
                arg.beginStructure();
                arg >> id >> name >> icon;
                arg.endStructure();
                out.append({id.path(), name, QString(), true});
                ++playlists;
            }
            arg.endArray();
        }
    }
    // TrackList: Tracks (ao) + GetTracksMetadata(ao) -> aa{sv}
    if (getProperty(m_service, kRootIface, "HasTrackList").toBool()) {
        const QVariant v = getProperty(m_service, "org.mpris.MediaPlayer2.TrackList", "Tracks");
        QList<QDBusObjectPath> ids;
        if (v.canConvert<QDBusArgument>()) v.value<QDBusArgument>() >> ids;
        if (!ids.isEmpty()) {
            QDBusMessage m = QDBusMessage::createMethodCall(m_service, QLatin1String(kPath),
                                                            QStringLiteral("org.mpris.MediaPlayer2.TrackList"),
                                                            QStringLiteral("GetTracksMetadata"));
            m << QVariant::fromValue(ids.mid(0, 200));
            const QDBusMessage r = bus.call(m, QDBus::Block, 2000);
            if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty()) {
                const QDBusArgument arg = r.arguments().first().value<QDBusArgument>();
                arg.beginArray();
                while (!arg.atEnd()) {
                    QVariantMap md;
                    arg >> md;
                    const QVariant tid = md.value(QStringLiteral("mpris:trackid"));
                    const QString id = tid.canConvert<QDBusObjectPath>() ? tid.value<QDBusObjectPath>().path() : tid.toString();
                    out.append({id, md.value(QStringLiteral("xesam:title")).toString(),
                                joinList(md.value(QStringLiteral("xesam:artist"))), false});
                    ++tracks;
                }
                arg.endArray();
            }
        }
    }
    if (info)
        *info = QStringLiteral("%1: %2 Playlists, %3 Titel in der Liste").arg(playerName()).arg(playlists).arg(tracks);
    return out;
}

void MprisClient::activate(const Entry &e)
{
    QDBusMessage m = e.playlist
        ? QDBusMessage::createMethodCall(m_service, QLatin1String(kPath), QStringLiteral("org.mpris.MediaPlayer2.Playlists"),
                                         QStringLiteral("ActivatePlaylist"))
        : QDBusMessage::createMethodCall(m_service, QLatin1String(kPath), QStringLiteral("org.mpris.MediaPlayer2.TrackList"),
                                         QStringLiteral("GoTo"));
    m << QVariant::fromValue(QDBusObjectPath(e.id));
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 2000);
    emit log(r.type() == QDBusMessage::ErrorMessage
                 ? QStringLiteral("Musik: Auswahl fehlgeschlagen (%1)").arg(r.errorMessage())
                 : QStringLiteral("Musik: %1 gewählt").arg(e.playlist ? QStringLiteral("Playlist") : QStringLiteral("Titel")));
    QTimer::singleShot(300, this, &MprisClient::poll);
}
