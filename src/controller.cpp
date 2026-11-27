#include "controller.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QRegularExpression>
#include <QFileInfo>
#include <QGeoPositionInfoSource>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusReply>
#include <QDBusInterface>
#include <QNetworkInterface>
#include <QStandardPaths>
#include <QTcpSocket>

#include "aap/usbaccessory.h"

Controller::Controller(QObject *parent) : QObject(parent)
{
    bool ok = false;
    const int envPort = qEnvironmentVariableIntValue("SAILAUTO_PORT", &ok);
    if (ok && envPort > 0) m_port = envPort;

    m_certDir = QString::fromLocal8Bit(qgetenv("SAILAUTO_CERT_DIR"));
    if (m_certDir.isEmpty())
        m_certDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(m_certDir);

    m_traceBase = QString::fromLocal8Bit(qgetenv("SAILAUTO_TRACE_DIR"));
    if (m_traceBase.isEmpty())
        m_traceBase = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + QStringLiteral("/SailAuto");

    connect(&m_server, &QTcpServer::newConnection, this, &Controller::onNewConnection);
    connect(&m_frameTimer, &QTimer::timeout, this, &Controller::tick);
    connect(&m_encoder, &H264Encoder::encoded, this, &Controller::onEncoded);
    connect(&m_encoder, &H264Encoder::error, this, [this](const QString &e) {
        appendLog(QStringLiteral("Encoder: ") + e);
    });
    connect(&m_encoder, &H264Encoder::restarted, this, [this](const QString &name) {
        appendLog(QStringLiteral("Encoder gewechselt: ") + name);
        m_screen.setInfo(m_session ? m_session->headUnitName() : QString(), name);
        m_queue.clear();
    });
    connect(&m_audioTest, &AudioTest::log, this, &Controller::appendLog);
    connect(&m_router, &AudioRouter::log, this, &Controller::appendLog);
    connect(&m_mpris, &MprisClient::log, this, &Controller::appendLog);
    pushAppsToScreen();
    connect(&m_apps, &AppCatalog::selectionChanged, this, &Controller::pushAppsToScreen);
    connect(&m_screen, &CarScreen::launchApp, this, [this](const QString &id) {
        QString err;
        const AppEntry *a = m_apps.find(id);
        const QString name = a ? a->name : id;
        switch (m_apps.launch(id, &err)) {
        case AppCatalog::Failed:
            appendLog(QStringLiteral("App: ") + err);
            return;
        case AppCatalog::TooSoon:
            return;
        case AppCatalog::AlreadyRunning:
            appendLog(QStringLiteral("App: \"%1\" läuft schon, starte nicht nochmal").arg(name));
            break;
        case AppCatalog::Started: {
            appendLog(QStringLiteral("App: starte \"%1\" vom Auto aus").arg(name));
            QImage icon;
            if (a && !a->iconPath.isEmpty()) icon.load(a->iconPath);
            m_screen.showLoading(name, icon);
            // Music apps: until their player shows up (see MPRIS). Others: a short moment.
            if (!a || !a->media) QTimer::singleShot(3000, this, [this]() { m_screen.hideLoading(); });
            break;
        }
        }
        if (a && a->media) m_mpris.setPreferred(id);
    });
    connect(&m_calls, &CallMonitor::log, this, &Controller::appendLog);
    connect(&m_phonebook, &PhoneBook::log, this, &Controller::appendLog);
    connect(&m_phonebook, &PhoneBook::loaded, this, [this]() {
        m_screen.setContacts(m_phonebook.contacts());
        m_screen.setRecentCalls(m_phonebook.recentCalls());
        m_calls.setContacts(m_phonebook.contacts());
    });
    connect(&m_screen, &CarScreen::dial, this, [this](const QString &number, const QString &name, const QImage &photo) {
        appendLog(QStringLiteral("Telefonbuch: Anruf vom Auto aus"));
        m_calls.dial(number, name, photo);
    });
    connect(&m_calls, &CallMonitor::changed, this, &Controller::onCallChanged);
    connect(&m_screen, &CarScreen::callAction, this, &Controller::onCallAction);
    connect(&m_nav, &PureMapsClient::log, this, &Controller::appendLog);
    connect(&m_map, &MapView::log, this, &Controller::appendLog);
    m_weatherEnabled = QSettings().value(QStringLiteral("weather/enabled"), true).toBool();
    m_closeIdle = QSettings().value(QStringLiteral("music/closeIdle"), true).toBool();
    // Music apps not playing for 2 min while in the car: close them (saves battery).
    m_idleTimer.setInterval(qEnvironmentVariableIsSet("SAILAUTO_IDLE_MS") ? 2000 : 15000);
    connect(&m_idleTimer, &QTimer::timeout, this, [this]() {
        if (!m_closeIdle || !m_session || m_pausedForCall || m_calls.call().state != CallInfo::None) return;
        const int idle = qEnvironmentVariableIsSet("SAILAUTO_IDLE_MS") ? qgetenv("SAILAUTO_IDLE_MS").toInt() : 120000;
        for (const QString &d : m_mpris.quitIdle(idle)) appendLog(QStringLiteral("Musik: ") + d + QStringLiteral(" (2 min Pause)"));
    });
    connect(&m_weather, &WeatherClient::log, this, &Controller::appendLog);
    connect(&m_weather, &WeatherClient::changed, this, [this]() { m_screen.setWeather(m_weather.data()); });
    {
        QSettings st;
        m_mapEnabled = st.value(QStringLiteral("map/enabled"), false).toBool();
        const QString u = st.value(QStringLiteral("map/url")).toString();
        if (!u.isEmpty()) m_map.setTileUrl(u);
    }
    m_screen.setMap(m_mapEnabled ? &m_map : nullptr);
    {
        QSettings st;
        m_home = st.value(QStringLiteral("nav/home")).toString();
        m_work = st.value(QStringLiteral("nav/work")).toString();
        m_recentDest = st.value(QStringLiteral("nav/recent")).toStringList();
    }
    pushDestinations();
    connect(&m_screen, &CarScreen::navSearch, this, &Controller::onNavSearch);
    connect(&m_geo, &GeoSearch::finished, this, [this](const QString &text, const QVector<GeoResult> &res, const QString &error) {
        m_geoResults = res;
        if (!error.isEmpty()) {
            appendLog(QStringLiteral("Navigation: Adresssuche (OSM Scout Server) nicht erreichbar, suche in Pure Maps"));
            m_screen.setSearchResults({}, false, QStringLiteral("Ohne OSM Scout Server: Pure Maps sucht am Telefon.\n"
                                                               "Dort Ergebnis antippen, dann „Route“."));
            sendToPureMaps(text);
            return;
        }
        appendLog(QStringLiteral("Navigation: %1 Treffer").arg(res.size()));
        m_screen.setSearchResults(res, false, QStringLiteral("Nichts gefunden. Anders schreiben?"));
    });
    connect(&m_screen, &CarScreen::navPick, this, [this](int i) {
        if (i < 0 || i >= m_geoResults.size()) return;
        const GeoResult g = m_geoResults[i];
        if (m_nav.state().app) {
            m_nav.showPoi(g.title, g.lat, g.lon);
            return;
        }
        m_pendingPoi = g;
        QString err;
        if (m_apps.launch(QStringLiteral("harbour-pure-maps"), &err) == AppCatalog::Failed)
            appendLog(QStringLiteral("Navigation: Pure Maps nicht startbar (%1)").arg(err));
    });
    connect(&m_screen, &CarScreen::musicListRequested, this, [this]() {
        QString info;
        m_browse = m_mpris.browse(&info);
        appendLog(QStringLiteral("Musik: Liste – ") + info);
        if (m_browse.isEmpty()) {
            m_screen.showNotification(QStringLiteral("Musik"), QStringLiteral("Keine Liste verfügbar"),
                                      QStringLiteral("%1 bietet keine Playlists/Titelliste an (MPRIS).").arg(m_mpris.playerName()));
            return;
        }
        QVector<CarScreen::ListItem> items;
        for (const MprisClient::Entry &e : m_browse) items.append({e.title, e.sub, e.playlist});
        m_screen.setMusicList(items);
    });
    connect(&m_screen, &CarScreen::musicListPicked, this, [this](int i) {
        if (i >= 0 && i < m_browse.size()) m_mpris.activate(m_browse[i]);
    });
    connect(&m_screen, &CarScreen::navStart, &m_nav, &PureMapsClient::startNavigation);
    connect(&m_nav, &PureMapsClient::changed, this, [this]() {
        const NavState &n = m_nav.state();
        m_screen.setNav(n, m_nav.icon(n.icon, 256));
        if (n.app && !m_pendingPoi.title.isEmpty()) {
            const GeoResult g = m_pendingPoi;
            m_pendingPoi = GeoResult();
            QTimer::singleShot(1500, this, [this, g]() { m_nav.showPoi(g.title, g.lat, g.lon); });
        }
        if (n.app && !m_pendingSearch.isEmpty()) {
            const QString t = m_pendingSearch;
            m_pendingSearch.clear();
            QTimer::singleShot(1500, this, [this, t]() { m_nav.search(t); }); // let its UI come up
        }
    });
    connect(&m_phoneStatus, &PhoneStatus::changed, this, [this]() { m_screen.setPhoneState(m_phoneStatus.state()); });
    connect(&m_phoneStatus, &PhoneStatus::log, this, &Controller::appendLog);
    connect(&m_mpris, &MprisClient::changed, this, [this]() {
        if (m_mpris.showingPreferred()) m_screen.hideLoading();
        m_screen.setNowPlaying(m_mpris.hasPlayer(), m_mpris.playerName(), m_mpris.title(), m_mpris.artist(),
                               m_mpris.album(), m_mpris.art(), m_mpris.playing(), m_mpris.positionMs(), m_mpris.lengthMs());
    });
    connect(&m_screen, &CarScreen::mediaCommand, this, &Controller::onMediaCommand);
    connect(&m_screen, &CarScreen::mediaSeek, &m_mpris, &MprisClient::seekToFraction);
    connect(&m_router, &AudioRouter::chunk, this, &Controller::onPhoneAudio);
    m_soundToCar = QSettings().value(QStringLiteral("soundToCar"), true).toBool();
    m_autostart = QSettings().value(QStringLiteral("autostart"), false).toBool();
    m_notifyToCar = QSettings().value(QStringLiteral("notifyToCar"), true).toBool();
    // Repair a phone left silent by an older version or a crash.
    QTimer::singleShot(1500, this, [this]() { if (!m_router.running()) m_router.repairSilentStreams(); });
    {
        QSettings st;
        m_parked.valid = st.value(QStringLiteral("parked/valid"), false).toBool();
        m_parked.lat = st.value(QStringLiteral("parked/lat")).toDouble();
        m_parked.lon = st.value(QStringLiteral("parked/lon")).toDouble();
        m_parked.acc = st.value(QStringLiteral("parked/accuracy"), -1).toDouble();
        m_parked.time = st.value(QStringLiteral("parked/time")).toDateTime();
    }
    connect(&m_notify, &NotificationWatcher::log, this, &Controller::appendLog);
    connect(&m_notify, &NotificationWatcher::notification, this,
            [this](const QString &app, const QString &title, const QString &body, const QString &category) {
        if (app.contains(QStringLiteral("sailauto"), Qt::CaseInsensitive)) return;
        // Log only who and how long: the log may be shared for diagnosis.
        appendLog(QStringLiteral("Benachrichtigung: %1%2 (Titel %3 Zeichen, Text %4 Zeichen)")
                      .arg(app, category.isEmpty() ? QString() : QStringLiteral(" [") + category + QLatin1Char(']'))
                      .arg(title.size()).arg(body.size()));
        m_screen.showNotification(app, title, body);
    });
    // A running app follows the autostart watcher too (car came back while open).
    m_lastCarSeen = QFileInfo(QStringLiteral("/run/harbour-sailauto/car")).lastModified();
    m_carPoll.setInterval(2000);
    connect(&m_carPoll, &QTimer::timeout, this, &Controller::checkCarSignal);
    if (m_autostart) m_carPoll.start();
    connect(&m_screen, &CarScreen::tapped, this, [this](const QString &w) {
        appendLog(QStringLiteral("Auto-Display: ") + w);
        // Music page play button = media sound test.
        if (w == QLatin1String("Play")) soundTest(QStringLiteral("media"));
        else if (w == QLatin1String("Pause")) stopSoundTest();
    });
    m_usbRetry.setInterval(2000);
    connect(&m_usbRetry, &QTimer::timeout, this, &Controller::tryOpenUsb);
    m_holdTimer.setSingleShot(true);
    m_holdTimer.setInterval(600);
    connect(&m_holdTimer, &QTimer::timeout, this, [this]() {
        if (m_heldKey == 87 || m_heldKey == 88) startSeeking(m_heldKey == 87 ? 1 : -1);
    });
    m_seekTimer.setInterval(500);
    connect(&m_seekTimer, &QTimer::timeout, this, [this]() {
        // Safety net in case the release never arrives: stop after 60 s.
        if (!m_seekDir || !m_session || ++m_seekSteps > 120) {
            m_seekTimer.stop();
            m_seekDir = 0;
            m_heldKey = 0;
            return;
        }
        m_mpris.seek(m_seekDir * 10000);
    });
    // The car only starts Android Auto on a freshly enumerated device. After
    // a dropped session it stays silent on the old one, so "re-plug" the cable.
    m_usbKick.setSingleShot(true);
    m_usbKick.setInterval(12000);
    connect(&m_usbKick, &QTimer::timeout, this, [this]() {
        if (!m_session || !qobject_cast<UsbAccessoryDevice *>(m_socket.data())) return;
        if (m_session->receivedBytes() > 0) return;
        if (m_lastReplug.isValid() && m_lastReplug.elapsed() < 25000) {
            m_usbKick.start();
            return;
        }
        if (m_autoReplugs >= 3) {
            if (m_autoReplugs++ == 3)
                appendLog(QStringLiteral("USB: Auto antwortet auch nach 3x neu verbinden nicht. Kabel/Buchse prüfen, "
                                         "Android Auto im Auto-Menü wählen, oder Kabel wirklich neu einstecken."));
            return;
        }
        ++m_autoReplugs;
        appendLog(QStringLiteral("USB: Auto sendet seit 12 s nichts -> USB neu verbinden (wie Kabel neu einstecken, %1/3)")
                      .arg(m_autoReplugs));
        replugUsb();
    });
    setStatus(QStringLiteral("Bereit"));
    appendLog(QStringLiteral("SailAuto Version %1").arg(version()));
}

Controller::~Controller()
{
    m_router.stop();
    delete m_trace;
    if (m_usbWanted && m_aoaRequested) requestAoa(QStringLiteral("off"));
    m_encoder.stop();
}

bool Controller::customCert() const
{
    return QFile::exists(m_certDir + QStringLiteral("/cert.pem")) && QFile::exists(m_certDir + QStringLiteral("/key.pem"));
}

QStringList Controller::addresses() const
{
    QStringList out;
    for (const QHostAddress &a : QNetworkInterface::allAddresses())
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
            out << a.toString();
    return out;
}

bool Controller::startListening()
{
    if (m_server.isListening()) return true;
    if (!m_server.listen(QHostAddress::AnyIPv4, static_cast<quint16>(m_port))) {
        appendLog(QStringLiteral("Port %1 nicht verfügbar: %2").arg(m_port).arg(m_server.errorString()));
        return false;
    }
    appendLog(QStringLiteral("Warte auf Head Unit auf %1:%2").arg(addresses().join(QStringLiteral(", "))).arg(m_port));
    setStatus(QStringLiteral("Warte auf Head Unit"));
    emit listeningChanged();
    return true;
}

void Controller::stopListening()
{
    disconnectHeadUnit();
    m_server.close();
    setStatus(QStringLiteral("Bereit"));
    emit listeningChanged();
}

void Controller::disconnectHeadUnit()
{
    if (m_session) m_session->requestShutdown();
}

void Controller::setSoundToCar(bool on)
{
    if (on == m_soundToCar) return;
    m_soundToCar = on;
    QSettings().setValue(QStringLiteral("soundToCar"), on);
    emit soundToCarChanged();
    if (on && m_session && m_encoder.running()) startRouter();
    else if (!on) stopRouter();
}

void Controller::startRouter()
{
    if (!m_soundToCar || m_router.running() || !m_session) return;
    if (m_session->audioChannel(3) < 0) return; // audioReady() starts us later
    QString err;
    if (!m_router.start(&err)) appendLog(QStringLiteral("Ton ins Auto nicht möglich: ") + err);
}

void Controller::stopRouter()
{
    stopCarAudio(QStringLiteral("Ton-Umleitung aus"));
    m_router.stop();
}

void Controller::stopCarAudio(const QString &why)
{
    if (!m_carAudioOn) return;
    m_carAudioOn = false;
    if (m_session) {
        m_session->stopAudio(m_carAudioChannel);
        m_session->requestAudioFocus(4 /*RELEASE*/);
    }
    appendLog(QStringLiteral("Ton: Stream ans Auto beendet (%1), %2 s gesendet%3")
                  .arg(why).arg(double(m_carFrames) / AudioRouter::Rate, 0, 'f', 1)
                  .arg(m_droppedChunks ? QStringLiteral(", %1 Pakete verworfen").arg(m_droppedChunks) : QString()));
}

void Controller::onPhoneAudio(const QByteArray &pcm)
{
    if (!m_session || m_audioTest.running()) return;
    if (callBlocksAudio()) return; // ringing or talking: the car's phone has the speakers
    const qint16 *s = reinterpret_cast<const qint16 *>(pcm.constData());
    const int samples = pcm.size() / 2;
    int peak = 0;
    for (int i = 0; i < samples; ++i) peak = qMax(peak, qAbs(static_cast<int>(s[i])));
    const bool loud = peak > 64;

    if (!m_carAudioOn) {
        if (!loud) return;
        m_carAudioChannel = m_session->audioChannel(3);
        if (m_carAudioChannel < 0) return;
        m_session->requestAudioFocus(1 /*GAIN*/);
        if (!m_session->startAudio(m_carAudioChannel)) return;
        m_carAudioOn = true;
        m_carFrames = 0;
        m_silentMs = 0;
        m_droppedChunks = 0;
        appendLog(QStringLiteral("Ton: Telefon spielt etwas ab (Spitze %1 von 32767) -> Stream ans Auto (Kanal %2)")
                      .arg(peak).arg(m_carAudioChannel));
        m_router.dumpNow(QStringLiteral("bei Tonbeginn"));
    }
    const int frames = samples / AudioRouter::Channels;
    if (m_session->canSendAudio(m_carAudioChannel)) {
        m_session->sendAudio(m_carAudioChannel, pcm, static_cast<quint64>(m_carFrames * 1000000 / AudioRouter::Rate));
    } else if (++m_droppedChunks == 50) {
        appendLog(QStringLiteral("Ton: Auto bestätigt Audiopakete zu langsam, verwerfe welche"));
    }
    m_carFrames += frames;
    m_silentMs = loud ? 0 : m_silentMs + frames * 1000 / AudioRouter::Rate;
    // Every 60 s of streaming: level, to tell music from noise in the log.
    if ((m_carFrames / AudioRouter::Rate) % 60 == 0 && (m_carFrames % AudioRouter::Rate) < frames && m_carFrames >= AudioRouter::Rate)
        appendLog(QStringLiteral("Ton: läuft seit %1 s, Spitze gerade %2").arg(m_carFrames / AudioRouter::Rate).arg(peak));
    // 3 s silence: give the speakers back to the car (radio etc.).
    if (m_silentMs >= 3000) stopCarAudio(QStringLiteral("3 s Stille"));
}

void Controller::onMediaCommand(const QString &cmd)
{
    if (cmd == QLatin1String("playpause")) m_mpris.playPause();
    else if (cmd == QLatin1String("next")) m_mpris.next();
    else if (cmd == QLatin1String("previous")) m_mpris.previous();
}

void Controller::startSeeking(int dir)
{
    m_seekDir = dir;
    m_seekSteps = 0;
    appendLog(dir > 0 ? QStringLiteral("Musik: spule vor") : QStringLiteral("Musik: spule zurück"));
    m_mpris.seek(dir * 10000);
    m_seekTimer.start();
}

void Controller::onCarKey(int code, bool pressed, bool longPress)
{
    // CALL / ENDCALL: answer, decline, hang up.
    if ((code == 5 || code == 6) && m_calls.call().state != CallInfo::None) {
        if (!pressed) return;
        if (code == 5 && m_calls.call().state == CallInfo::Incoming) onCallAction(QStringLiteral("answer"));
        else if (code == 6) onCallAction(QStringLiteral("hangup"));
        return;
    }
    // NEXT/PREVIOUS: short press = track, hold = spool (like a car radio).
    if (code == 87 || code == 88) {
        const int dir = code == 87 ? 1 : -1;
        if (pressed) {
            if (m_heldKey == code && (longPress || m_seekDir)) {
                if (!m_seekDir) { m_holdTimer.stop(); startSeeking(dir); }
                return; // car repeats the press while held
            }
            m_heldKey = code;
            m_seekDir = 0;
            if (longPress) startSeeking(dir);
            else m_holdTimer.start();
            return;
        }
        // release
        const bool wasSeeking = m_seekDir != 0;
        m_holdTimer.stop();
        m_seekTimer.stop();
        m_seekDir = 0;
        if (m_heldKey == code && !wasSeeking) {
            if (code == 87) m_mpris.next();
            else m_mpris.previous();
        }
        m_heldKey = 0;
        return;
    }
    // MEDIA_REWIND / MEDIA_FAST_FORWARD: spool while held.
    if (code == 89 || code == 90) {
        if (pressed) {
            if (!m_seekDir) startSeeking(code == 90 ? 1 : -1);
        } else {
            m_seekTimer.stop();
            m_seekDir = 0;
        }
        return;
    }
    if (!pressed) return; // act on press, ignore release
    switch (code) {
    case 85: case 79: m_mpris.playPause(); break; // PLAY_PAUSE, HEADSETHOOK
    case 126: m_mpris.play(); break;
    case 127: m_mpris.pause(); break;
    default: break;
    }
}

void Controller::soundTest(const QString &kind)
{
    stopCarAudio(QStringLiteral("Tontest"));
    QString err;
    if (!m_audioTest.start(m_session, kind == QLatin1String("guidance") ? AudioTest::Guidance : AudioTest::Media, &err))
        appendLog(QStringLiteral("Tontest nicht möglich: ") + err);
}

void Controller::stopSoundTest()
{
    m_audioTest.stop();
}

void Controller::clearLog()
{
    m_log.clear();
    emit logChanged();
}

void Controller::onNewConnection()
{
    while (QTcpSocket *s = m_server.nextPendingConnection()) {
        if (m_session) {
            appendLog(QStringLiteral("Zweite Verbindung abgelehnt"));
            s->close();
            s->deleteLater();
            continue;
        }
        s->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        connect(s, &QTcpSocket::disconnected, this, [this]() {
            if (m_session) onSessionClosed(QStringLiteral("Verbindung getrennt"));
        });
        attachTransport(s, QStringLiteral("Head Unit verbunden: %1").arg(s->peerAddress().toString()));
    }
}

namespace {
const char kAoaRequest[] = "/run/harbour-sailauto-req/request";
const char kAoaState[] = "/run/harbour-sailauto/state";
const char kAoaLog[] = "/run/harbour-sailauto/last.log";
}

bool Controller::requestAoa(const QString &cmd)
{
    QFile f(QString::fromLatin1(kAoaRequest));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        appendLog(QStringLiteral("USB: Root-Dienst nicht erreichbar (%1). Manuell: devel-su sh "
                                 "/usr/share/harbour-sailauto/aoa-gadget.sh %2")
                      .arg(f.errorString(), cmd));
        return false;
    }
    f.write(cmd.toLatin1() + '\n');
    return true;
}

QString Controller::aoaState() const
{
    QFile f(QString::fromLatin1(kAoaState));
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromLatin1(f.readAll()).trimmed();
}

bool Controller::startUsb()
{
    m_usbWanted = true;
    emit listeningChanged();
    setStatus(QStringLiteral("USB wird umgeschaltet"));
    m_aoaRequested = requestAoa(QStringLiteral("on"));
    m_aoaWait.start();
    m_aoaReported = false;
    m_autoReplugs = 0;
    m_usbStartedAt = QDateTime::currentDateTime();
    appendLog(QStringLiteral("USB: schalte auf Android-Accessory-Modus"));
    m_usbRetry.start(500);
    return true;
}

void Controller::stopUsb()
{
    const bool was = m_usbWanted;
    m_usbWanted = false;
    m_usbRetry.stop();
    m_usbKick.stop();
    if (m_session && qobject_cast<UsbAccessoryDevice *>(m_socket.data()))
        disconnectHeadUnit();
    if (was && m_aoaRequested) {
        requestAoa(QStringLiteral("off"));
        appendLog(QStringLiteral("USB: zurück auf normalen Modus"));
    }
    m_aoaRequested = false;
    emit listeningChanged();
    setStatus(listening() ? QStringLiteral("Warte auf Head Unit") : QStringLiteral("Bereit"));
}

void Controller::setAutostart(bool on)
{
    if (on == m_autostart) return;
    m_autostart = on;
    QSettings().setValue(QStringLiteral("autostart"), on);
    emit autostartChanged();
    if (on) m_carPoll.start(); else m_carPoll.stop();
    appendLog(on ? QStringLiteral("Autostart: an (Root-Dienst wird eingeschaltet)")
                 : QStringLiteral("Autostart: aus"));
    if (!requestAoa(on ? QStringLiteral("autoon") : QStringLiteral("autooff"))) return;
    QTimer::singleShot(4000, this, [this, on]() {
        QFile f(QStringLiteral("/run/harbour-sailauto/autostart"));
        const QString st = f.open(QIODevice::ReadOnly) ? QString::fromLatin1(f.readAll()).trimmed() : QString();
        if (st == (on ? QLatin1String("on") : QLatin1String("off"))) {
            QFile c(m_certDir + QStringLiteral("/cars"));
            const QString cars = c.open(QIODevice::ReadOnly)
                ? QString::fromLatin1(c.readAll()).trimmed().replace(QLatin1Char('\n'), QLatin1Char(' ')) : QString();
            appendLog(!on ? QStringLiteral("Autostart: Root-Dienst aus")
                          : QStringLiteral("Autostart: Root-Dienst läuft. Bekannte Autos (Bluetooth): %1")
                                .arg(cars.isEmpty() ? QStringLiteral("noch keins – einmal per USB verbinden") : cars));
        } else {
            QFile l(QStringLiteral("/run/harbour-sailauto/last.log"));
            l.open(QIODevice::ReadOnly);
            appendLog(QStringLiteral("Autostart: Root-Dienst meldet \"%1\" %2").arg(st, QString::fromUtf8(l.readAll()).trimmed()));
        }
    });
}

QVariantList Controller::installedApps()
{
    m_apps.reload();
    return m_apps.model();
}

void Controller::setCarApp(const QString &id, bool on)
{
    m_apps.setSelected(id, on);
}

void Controller::pushAppsToScreen()
{
    QVector<CarScreen::CarApp> list;
    for (const AppEntry &a : m_apps.selected()) {
        QImage icon;
        if (!a.iconPath.isEmpty() && icon.load(a.iconPath))
            icon = icon.scaled(96, 96, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        list.append({a.id, a.name, icon, a.media});
    }
    m_screen.setApps(list);
}

void Controller::setNotifyToCar(bool on)
{
    if (on == m_notifyToCar) return;
    m_notifyToCar = on;
    QSettings().setValue(QStringLiteral("notifyToCar"), on);
    emit notifyToCarChanged();
    if (!on) m_notify.stop();
    else if (m_encoder.running()) m_notify.start();
}

void Controller::updateParked()
{
    // Text of notifications etc. only when standing: no restrictions, or gear P.
    m_screen.setParked(m_drivingStatus == 0 || m_gear == 101);
}

QString Controller::parkedTime() const
{
    if (!m_parked.valid) return QString();
    const QDateTime t = m_parked.time.toLocalTime();
    return t.date() == QDate::currentDate() ? t.toString(QStringLiteral("HH:mm"))
                                            : t.toString(QStringLiteral("dd.MM. HH:mm"));
}

void Controller::onCarLocation(double lat, double lon, double acc, double bearing, double speed)
{
    m_carFixClock.start();
    if (m_mapEnabled) m_map.updateFix(lat, lon, bearing, speed);
    if (m_weatherEnabled) m_weather.setPosition(lat, lon);
    const bool first = !m_carFix.valid;
    m_carFix.valid = true;
    m_carFix.lat = lat;
    m_carFix.lon = lon;
    m_carFix.acc = acc;
    m_carFix.time = QDateTime::currentDateTimeUtc();
    if (first) {
        // No coordinates in the log (it may be shared for diagnosis).
        appendLog(QStringLiteral("Auto-GPS: Position empfangen (Genauigkeit %1)")
                      .arg(acc < 0 ? QStringLiteral("?") : QString::number(qRound(acc)) + QStringLiteral(" m")));
    }
    // Save now and then, so a crash or dead battery still leaves a position.
    if (first || !m_fixSaved.isValid() || m_fixSaved.elapsed() > 30000) saveParked(m_carFix, false);
}

void Controller::saveParked(const Fix &f, bool final)
{
    if (!f.valid) return;
    m_parked = f;
    if (final) m_parked.time = QDateTime::currentDateTimeUtc();
    QSettings st;
    st.setValue(QStringLiteral("parked/valid"), true);
    st.setValue(QStringLiteral("parked/lat"), m_parked.lat);
    st.setValue(QStringLiteral("parked/lon"), m_parked.lon);
    st.setValue(QStringLiteral("parked/accuracy"), m_parked.acc);
    st.setValue(QStringLiteral("parked/time"), m_parked.time);
    m_fixSaved.start();
    emit parkedChanged();
    if (final)
        appendLog(QStringLiteral("Parkplatz gemerkt (letzte Auto-Position, Genauigkeit %1)")
                      .arg(m_parked.acc < 0 ? QStringLiteral("?") : QString::number(qRound(m_parked.acc)) + QStringLiteral(" m")));
}

bool Controller::callBlocksAudio() const
{
    const CallInfo &c = m_calls.call();
    return (c.state == CallInfo::Incoming && !m_callSilenced) || c.state == CallInfo::Active
           || c.state == CallInfo::Outgoing || c.state == CallInfo::Held;
}

void Controller::onCallChanged()
{
    const CallInfo c = m_calls.call();
    const QString who = c.name.isEmpty() ? (c.number.isEmpty() ? QStringLiteral("Unbekannt") : c.number) : c.name;
    if (c.state != m_lastCall.state || c.id != m_lastCall.id) {
        static const char *names[] = {"keiner", "eingehend", "aktiv", "ausgehend", "gehalten"};
        // Log the state, not the number (the log may be shared).
        appendLog(QStringLiteral("Anruf: %1%2").arg(QLatin1String(names[c.state]))
                      .arg(c.state == CallInfo::Incoming ? (c.name.isEmpty() ? QStringLiteral(" (unbekannt)") : QStringLiteral(" (Kontakt)"))
                                                         : QString()));
    }
    if (c.state == CallInfo::Incoming && (m_lastCall.state != CallInfo::Incoming || c.id != m_lastCall.id)) {
        // New ringing call: music off, speakers free for the car's phone.
        m_callSilenced = false;
        m_callHandled = false;
        if (m_mpris.playing()) {
            m_mpris.pause();
            m_pausedForCall = true;
        }
        stopCarAudio(QStringLiteral("Anruf"));
    }
    if ((c.state == CallInfo::Active || c.state == CallInfo::Outgoing) && m_carAudioOn)
        stopCarAudio(QStringLiteral("Gespräch"));
    if (c.state == CallInfo::None && m_lastCall.state != CallInfo::None) {
        QTimer::singleShot(3000, &m_phonebook, &PhoneBook::load); // refresh "Zuletzt"
        if (m_lastCall.state == CallInfo::Incoming && !m_callHandled) {
            const QString lastWho = m_lastCall.name.isEmpty() ? m_lastCall.number : m_lastCall.name;
            m_screen.showNotification(QStringLiteral("Telefon"), QStringLiteral("Verpasster Anruf"), lastWho);
        }
        if (m_pausedForCall) {
            m_pausedForCall = false;
            m_mpris.play();
            appendLog(QStringLiteral("Anruf vorbei: Musik läuft weiter"));
        }
        m_callSilenced = false;
    }
    Q_UNUSED(who)
    m_lastCall = c;
    m_screen.setCall(c, m_callSilenced);
}

void Controller::onCallAction(const QString &action)
{
    if (action == QLatin1String("answer")) {
        m_callHandled = true;
        m_calls.answer();
    } else if (action == QLatin1String("decline") || action == QLatin1String("hangup")) {
        m_callHandled = true;
        m_calls.hangup();
    } else if (action == QLatin1String("silence") || action == QLatin1String("silence_music")) {
        m_calls.silenceRingtone();
        m_callSilenced = true;
        if (action == QLatin1String("silence_music") && m_pausedForCall) {
            m_pausedForCall = false;
            m_mpris.play();
            appendLog(QStringLiteral("Anruf stumm: Musik läuft weiter"));
        }
        m_screen.setCall(m_calls.call(), true);
    }
}

void Controller::repairAudio()
{
    if (m_router.running()) {
        appendLog(QStringLiteral("Ton reparieren: geht erst, wenn das Auto getrennt ist"));
        return;
    }
    const int n = m_router.repairSilentStreams();
    if (n == 0) appendLog(QStringLiteral("Ton reparieren: keine stumme Wiedergabe gefunden"));
}

void Controller::startPhonePosition()
{
    if (m_phonePos || (!m_mapEnabled && !m_weatherEnabled)) return;
    m_phonePos = QGeoPositionInfoSource::createDefaultSource(this);
    if (!m_phonePos) {
        appendLog(QStringLiteral("Telefon-GPS: keine Positionsquelle (Ortung aus?)"));
        return;
    }
    m_phonePos->setUpdateInterval(1000);
    connect(m_phonePos, &QGeoPositionInfoSource::positionUpdated, this, [this](const QGeoPositionInfo &info) {
        if (!info.isValid()) return;
        if (!m_phonePosLogged) {
            m_phonePosLogged = true;
            appendLog(QStringLiteral("Telefon-GPS: Position da"));
        }
        // The car's GPS wins while it is fresh.
        if (m_carFixClock.isValid() && m_carFixClock.elapsed() < 3000) return;
        const QGeoCoordinate c = info.coordinate();
        const double dir = info.hasAttribute(QGeoPositionInfo::Direction) ? info.attribute(QGeoPositionInfo::Direction) : -1;
        const double speed = info.hasAttribute(QGeoPositionInfo::GroundSpeed) ? info.attribute(QGeoPositionInfo::GroundSpeed) : -1;
        if (m_mapEnabled) m_map.updateFix(c.latitude(), c.longitude(), dir, speed);
        if (m_weatherEnabled) m_weather.setPosition(c.latitude(), c.longitude());
    });
    m_phonePos->startUpdates();
}

void Controller::stopPhonePosition()
{
    if (!m_phonePos) return;
    m_phonePos->stopUpdates();
    m_phonePos->deleteLater();
    m_phonePos = nullptr;
    m_phonePosLogged = false;
}

void Controller::setCloseIdlePlayers(bool on)
{
    if (on == m_closeIdle) return;
    m_closeIdle = on;
    QSettings().setValue(QStringLiteral("music/closeIdle"), on);
    emit closeIdleChanged();
}

void Controller::setWeatherEnabled(bool on)
{
    if (on == m_weatherEnabled) return;
    m_weatherEnabled = on;
    QSettings().setValue(QStringLiteral("weather/enabled"), on);
    if (!on) m_screen.setWeather(WeatherData());
    emit weatherChanged();
}

void Controller::setMapEnabled(bool on)
{
    if (on == m_mapEnabled) return;
    m_mapEnabled = on;
    QSettings().setValue(QStringLiteral("map/enabled"), on);
    m_screen.setMap(on ? &m_map : nullptr);
    appendLog(on ? QStringLiteral("Karte im Auto: an") : QStringLiteral("Karte im Auto: aus"));
    if (on && m_carFix.valid) m_map.updateFix(m_carFix.lat, m_carFix.lon, -1, -1);
    if (on && m_session) startPhonePosition();
    emit mapChanged();
}

void Controller::setMapUrl(const QString &u)
{
    if (u.trimmed().isEmpty() || u == m_map.tileUrl()) return;
    m_map.setTileUrl(u);
    QSettings().setValue(QStringLiteral("map/url"), u.trimmed());
    emit mapChanged();
}

void Controller::pushDestinations()
{
    m_screen.setDestinations(m_home, m_work, m_recentDest);
}

void Controller::setHomeAddress(const QString &a)
{
    if (a == m_home) return;
    m_home = a.trimmed();
    QSettings().setValue(QStringLiteral("nav/home"), m_home);
    pushDestinations();
    emit destinationsChanged();
}

void Controller::setWorkAddress(const QString &a)
{
    if (a == m_work) return;
    m_work = a.trimmed();
    QSettings().setValue(QStringLiteral("nav/work"), m_work);
    pushDestinations();
    emit destinationsChanged();
}

void Controller::onNavSearch(const QString &text)
{
    // Remember (not home/work), newest first, max 5.
    if (text != m_home && text != m_work) {
        m_recentDest.removeAll(text);
        m_recentDest.prepend(text);
        while (m_recentDest.size() > 5) m_recentDest.removeLast();
        QSettings().setValue(QStringLiteral("nav/recent"), m_recentDest);
        pushDestinations();
    }
    // Results in the car via OSM Scout Server; fallback: Pure Maps search on the phone.
    if (m_carFix.valid) m_geo.setBias(m_carFix.lat, m_carFix.lon);
    else if (m_parked.valid) m_geo.setBias(m_parked.lat, m_parked.lon);
    m_geo.search(text);
}

void Controller::sendToPureMaps(const QString &text)
{
    if (m_nav.state().app) {
        m_nav.search(text);
        return;
    }
    m_pendingSearch = text;
    QString err;
    if (m_apps.launch(QStringLiteral("harbour-pure-maps"), &err) == AppCatalog::Failed)
        appendLog(QStringLiteral("Navigation: Pure Maps nicht startbar (%1)").arg(err));
    else
        appendLog(QStringLiteral("Navigation: starte Pure Maps für die Zielsuche"));
}

void Controller::startedByCar()
{
    appendLog(QStringLiteral("Autostart: vom Auto gestartet"));
    if (!m_usbWanted) startUsb();
}

void Controller::checkCarSignal()
{
    const QDateTime t = QFileInfo(QStringLiteral("/run/harbour-sailauto/car")).lastModified();
    if (!t.isValid() || t == m_lastCarSeen) return;
    m_lastCarSeen = t;
    if (m_usbWanted) return;
    appendLog(QStringLiteral("Autostart: Auto erkannt, schalte USB ein"));
    startUsb();
}

void Controller::rememberCar(const QString &mac)
{
    const QString m = mac.trimmed().toUpper();
    if (!QRegularExpression(QStringLiteral("^([0-9A-F]{2}:){5}[0-9A-F]{2}$")).match(m).hasMatch()) return;
    const QString path = m_certDir + QStringLiteral("/cars");
    QFile f(path);
    QStringList cars;
    if (f.open(QIODevice::ReadOnly)) {
        cars = QString::fromLatin1(f.readAll()).split(QLatin1Char('\n'), QString::SkipEmptyParts);
        f.close();
    }
    if (cars.contains(m)) return;
    cars << m;
    QDir().mkpath(m_certDir);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        f.write(cars.join(QLatin1Char('\n')).toLatin1() + '\n');
        appendLog(QStringLiteral("Autostart: Auto-Bluetooth %1 gemerkt (startet SailAuto, wenn sich das Telefon "
                                 "damit verbindet)").arg(m));
    }
}

void Controller::replugUsb()
{
    if (!m_usbWanted || !m_aoaRequested) {
        appendLog(QStringLiteral("USB: neu verbinden geht nur, wenn der USB-Schalter an ist"));
        return;
    }
    m_lastReplug.start();
    m_usbKick.stop();
    appendLog(QStringLiteral("USB: verbinde neu (Gerät kurz ab- und wieder anmelden)"));
    if (!requestAoa(QStringLiteral("replug"))) return;
    // The open session gets a read error ("USB getrennt"), then tryOpenUsb reopens.
    m_usbRetry.start(2000);
}

void Controller::tryOpenUsb()
{
    if (!m_usbWanted || m_session) return;
    // Autostart watcher switched USB back to normal (car gone for 60 s).
    if (m_aoaRequested && m_aoaReported && aoaState() == QLatin1String("off")
        && QFileInfo(QString::fromLatin1(kAoaState)).lastModified() > m_usbStartedAt) {
        appendLog(QStringLiteral("USB: Auto weg, Root-Dienst hat USB auf normal gestellt"));
        m_usbWanted = false;
        m_aoaRequested = false;
        m_usbRetry.stop();
        m_usbKick.stop();
        emit listeningChanged();
        setStatus(QStringLiteral("Bereit"));
        return;
    }
    if (m_aoaRequested && !m_aoaReported) {
        const QString st = aoaState();
        if (st == QLatin1String("error")) {
            QFile f(QString::fromLatin1(kAoaLog));
            f.open(QIODevice::ReadOnly);
            appendLog(QStringLiteral("USB-Umschaltung fehlgeschlagen: ") + QString::fromUtf8(f.readAll()).trimmed());
            m_aoaReported = true;
            stopUsb();
            return;
        }
        if (st != QLatin1String("on")) {
            if (m_aoaWait.elapsed() < 6000) return; // keep waiting for the root service
            appendLog(QStringLiteral("USB: Root-Dienst meldet nichts (Status \"%1\"). Versuche trotzdem. "
                                     "Manuell: devel-su sh /usr/share/harbour-sailauto/aoa-gadget.sh on").arg(st));
        } else {
            appendLog(QStringLiteral("USB: Accessory-Modus an (18d1:2d00). Kabel ans Auto / an den PC."));
        }
        m_aoaReported = true;
        m_usbRetry.setInterval(2000);
        setStatus(QStringLiteral("Warte auf USB"));
    }
    QString path = QString::fromLocal8Bit(qgetenv("SAILAUTO_USB_DEVICE"));
    if (path.isEmpty()) path = QStringLiteral("/dev/usb_accessory");
    // Open only once the car has (re)configured the USB port. Opening while
    // the cable is out gives a stale connection the car never talks to
    // (reconnect took 15+ s). The UDC state is world-readable.
    if (qEnvironmentVariableIsEmpty("SAILAUTO_USB_DEVICE")) {
        const QDir udc(QStringLiteral("/sys/class/udc"));
        const QStringList ctrls = udc.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
        if (!ctrls.isEmpty()) {
            QFile st(udc.filePath(ctrls.first() + QStringLiteral("/state")));
            const QString state = st.open(QIODevice::ReadOnly) ? QString::fromLatin1(st.readAll()).trimmed() : QString();
            if (!state.isEmpty() && state != QLatin1String("configured")) {
                if (state != m_lastUdcState) {
                    m_lastUdcState = state;
                    appendLog(QStringLiteral("USB: warte auf das Auto (Anschluss: %1)").arg(state));
                    setStatus(QStringLiteral("Warte auf Kabel/Auto"));
                }
                m_usbRetry.start(1000);
                return;
            }
            if (!m_lastUdcState.isEmpty() && state != m_lastUdcState)
                appendLog(QStringLiteral("USB: Auto verbunden (Anschluss: %1)").arg(state));
            m_lastUdcState = state;
        }
    }
    auto *dev = new UsbAccessoryDevice(path, this);
    if (!dev->open(QIODevice::ReadWrite)) {
        static QString lastError;
        if (dev->errorString() != lastError) {
            lastError = dev->errorString();
            QString who;
            if (dev->errorString().contains(QStringLiteral("busy"), Qt::CaseInsensitive)) {
                // Who holds it? (only our own user's processes are visible)
                const QDir proc(QStringLiteral("/proc"));
                for (const QString &pid : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                    bool num = false;
                    pid.toInt(&num);
                    if (!num || pid.toLongLong() == QCoreApplication::applicationPid()) continue;
                    const QDir fds(proc.filePath(pid + QStringLiteral("/fd")));
                    for (const QString &fd : fds.entryList(QDir::System | QDir::NoDotAndDotDot)) {
                        if (QFile::symLinkTarget(fds.filePath(fd)) != path) continue;
                        QFile cmd(proc.filePath(pid + QStringLiteral("/cmdline")));
                        cmd.open(QIODevice::ReadOnly);
                        who += QStringLiteral(" PID %1 (%2)").arg(pid, QString::fromLocal8Bit(cmd.readAll()).replace(QLatin1Char('\0'), QLatin1Char(' ')).trimmed());
                    }
                }
                if (who.isEmpty()) who = QStringLiteral(" (unbekannt; Root- oder fremder Prozess, oder alte Instanz hängt)");
            }
            appendLog(who.isEmpty()
                          ? QStringLiteral("USB: /dev/usb_accessory nicht offen (%1). Ist aoa-gadget.sh on gelaufen?").arg(lastError)
                          : QStringLiteral("USB: /dev/usb_accessory ist belegt von%1. Andere SailAuto-Instanz schließen "
                                           "oder Telefon neu starten.").arg(who));
        }
        delete dev;
        m_usbRetry.start();
        return;
    }
    m_usbRetry.stop();
    connect(dev, &UsbAccessoryDevice::disconnected, this, [this]() {
        if (m_session) onSessionClosed(QStringLiteral("USB getrennt"));
    });
    attachTransport(dev, QStringLiteral("USB-Accessory geöffnet, warte auf Head Unit"));
    if (m_session) m_usbKick.start();
}

QString Controller::phoneBluetoothAddress(QString *why) const
{
    const QByteArray forced = qgetenv("SAILAUTO_BT_ADDRESS");
    if (!forced.isEmpty()) return QString::fromLatin1(forced);
    QStringList reasons;
    const QString cacheFile = m_certDir + QStringLiteral("/bt-address");

    // BlueZ 5: ask the object manager for every adapter (hci0 is not always the name).
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        reasons << QStringLiteral("kein System-D-Bus: ") + bus.lastError().message();
    } else {
        QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), QStringLiteral("/"),
                                                           QStringLiteral("org.freedesktop.DBus.ObjectManager"),
                                                           QStringLiteral("GetManagedObjects"));
        QDBusMessage reply = bus.call(call, QDBus::Block, 3000);
        if (reply.type() == QDBusMessage::ErrorMessage) {
            reasons << QStringLiteral("BlueZ antwortet nicht (%1)").arg(reply.errorMessage());
        } else {
            QStringList paths;
            const QDBusArgument arg = reply.arguments().value(0).value<QDBusArgument>();
            arg.beginMap();
            while (!arg.atEnd()) {
                QDBusObjectPath path;
                QVariantMap dummy;
                arg.beginMapEntry();
                arg >> path;
                // interfaces -> properties; only the keys matter here
                arg.beginMap();
                bool isAdapter = false;
                while (!arg.atEnd()) {
                    QString iface;
                    QVariantMap props;
                    arg.beginMapEntry();
                    arg >> iface >> props;
                    arg.endMapEntry();
                    if (iface == QLatin1String("org.bluez.Adapter1")) isAdapter = true;
                }
                arg.endMap();
                arg.endMapEntry();
                if (isAdapter) paths << path.path();
            }
            arg.endMap();
            if (paths.isEmpty()) reasons << QStringLiteral("BlueZ hat keinen Adapter (Bluetooth aus?)");
            for (const QString &p : paths) {
                QDBusMessage get = QDBusMessage::createMethodCall(QStringLiteral("org.bluez"), p,
                                                                  QStringLiteral("org.freedesktop.DBus.Properties"),
                                                                  QStringLiteral("Get"));
                get << QStringLiteral("org.bluez.Adapter1") << QStringLiteral("Address");
                QDBusMessage r = bus.call(get, QDBus::Block, 3000);
                const QString addr = r.type() == QDBusMessage::ReplyMessage
                                         ? r.arguments().value(0).value<QDBusVariant>().variant().toString() : QString();
                if (!addr.isEmpty()) {
                    QFile c(cacheFile);
                    if (c.open(QIODevice::WriteOnly)) c.write(addr.toLatin1());
                    return addr;
                }
                reasons << QStringLiteral("%1: %2").arg(p, r.errorMessage());
            }
        }
    }
    QFile f(QStringLiteral("/sys/class/bluetooth/hci0/address"));
    if (f.open(QIODevice::ReadOnly)) {
        const QString a = QString::fromLatin1(f.readAll()).trimmed().toUpper();
        if (!a.isEmpty()) return a;
    }
    // Bluetooth off: use the address seen last time.
    QFile c(cacheFile);
    if (c.open(QIODevice::ReadOnly)) {
        const QString a = QString::fromLatin1(c.readAll()).trimmed();
        if (!a.isEmpty()) {
            reasons << QStringLiteral("nehme gespeicherte Adresse");
            if (why) *why = reasons.join(QStringLiteral("; "));
            return a;
        }
    }
    if (why) *why = reasons.join(QStringLiteral("; "));
    return QString();
}

void Controller::attachTransport(QIODevice *io, const QString &label)
{
    m_socket = io;
    delete m_trace;
    m_trace = new aap::Trace;
    if (m_trace->open(m_traceBase)) {
        m_lastTraceDir = m_trace->dir();
    } else {
        delete m_trace;
        m_trace = nullptr;
    }
    appendLog(label);
    appendLog(QStringLiteral("SailAuto Version %1").arg(version()));
    if (m_trace) appendLog(QStringLiteral("Diagnose-Aufzeichnung: ") + m_lastTraceDir);
    else appendLog(QStringLiteral("Diagnose-Aufzeichnung nicht möglich in ") + m_traceBase);

    m_session = new aap::Session(io, m_certDir + QStringLiteral("/cert.pem"),
                                 m_certDir + QStringLiteral("/key.pem"), this);
    connect(m_session, &aap::Session::log, this, &Controller::appendLog);
    connect(m_session, &aap::Session::stateChanged, this, &Controller::onSessionState);
    connect(m_session, &aap::Session::videoStart, this, &Controller::onVideoStart);
    connect(m_session, &aap::Session::videoStop, this, &Controller::onVideoStop);
    connect(m_session, &aap::Session::videoCanSend, this, &Controller::flush);
    connect(m_session, &aap::Session::touch, &m_screen, &CarScreen::touch);
    connect(m_session, &aap::Session::key, this, &Controller::onCarKey);
    // The car may finish the media channel setup after the video started.
    connect(m_session, &aap::Session::audioReady, this, [this](int type) {
        if (type == 3 && m_encoder.running()) startRouter();
    });
    connect(m_session, &aap::Session::nightModeChanged, this, [this](bool n) {
        m_screen.setNight(n);
        m_map.setNight(n);
        appendLog(n ? QStringLiteral("Nachtmodus an") : QStringLiteral("Nachtmodus aus"));
    });
    connect(m_session, &aap::Session::closed, this, &Controller::onSessionClosed);
    connect(m_session, &aap::Session::locationChanged, this, &Controller::onCarLocation);
    connect(m_session, &aap::Session::carSilent, this, [this]() { m_replugOnClose = true; });
    connect(m_session, &aap::Session::drivingStatusChanged, this, [this](int st) {
        // 0 = UNRESTRICTED (parked); any bit set = driving restrictions.
        if (st == m_drivingStatus) return;
        m_drivingStatus = st;
        appendLog(st == 0 ? QStringLiteral("Fahrstatus: steht (keine Einschränkungen)")
                          : QStringLiteral("Fahrstatus: fährt (Einschränkungen %1)").arg(st));
        updateParked();
    });
    connect(m_session, &aap::Session::gearChanged, this, [this](int g) {
        if (g == m_gear) return;
        m_gear = g;
        appendLog(QStringLiteral("Gang: %1").arg(g == 101 ? QStringLiteral("P") : g == 102 ? QStringLiteral("R")
                                                  : g == 100 ? QStringLiteral("D") : g == 0 ? QStringLiteral("N")
                                                  : QString::number(g)));
        updateParked();
    });
    connect(m_session, &aap::Session::audioFocusChanged, &m_audioTest, &AudioTest::onFocus);
    connect(m_session, &aap::Session::audioFocusChanged, this, [this](int st) {
        if (m_carAudioOn && (st == 3 || st == 5)) stopCarAudio(QStringLiteral("Auto nimmt Audio-Fokus weg"));
    });
    connect(m_session, &aap::Session::silent, this, [this]() {
        if (auto *usb = qobject_cast<UsbAccessoryDevice *>(m_socket.data())) {
            appendLog(QStringLiteral("USB-Handshake vom Auto (Kernel): ") + usb->handshakeInfo());
            appendLog(QStringLiteral("USB-Modus laut Root-Dienst: \"%1\". Leere Texte = das Auto hat (noch) keinen "
                                     "Accessory-Handshake geschickt (falsche Buchse? Gadget nicht an?).").arg(aoaState()));
            QFile f(QStringLiteral("/run/harbour-sailauto/watch.log"));
            if (f.open(QIODevice::ReadOnly))
                appendLog(QStringLiteral("Root-Wächter: ") + QString::fromUtf8(f.readAll()).trimmed().right(600));
        }
    });
    m_session->setTrace(m_trace);
    QString btWhy;
    const QString bt = phoneBluetoothAddress(&btWhy);
    m_session->setPhoneBluetoothAddress(bt);
    appendLog(QStringLiteral("Bluetooth-Adresse Telefon: %1%2")
                  .arg(bt.isEmpty() ? QStringLiteral("unbekannt") : bt)
                  .arg(btWhy.isEmpty() ? QString() : QStringLiteral(" (") + btWhy + QLatin1Char(')')));
    appendLog(QStringLiteral("Zertifikat: %1").arg(customCert() ? QStringLiteral("eigenes") : QStringLiteral("Entwickler (selbstsigniert)")));

    QString err;
    if (!m_session->start(&err)) {
        appendLog(err);
        onSessionClosed(err);
    }
    emit statusChanged();
}

void Controller::onSessionState(aap::Session::State s)
{
    switch (s) {
    case aap::Session::Versioning: setStatus(QStringLiteral("Verbinde")); break;
    case aap::Session::Handshaking: setStatus(QStringLiteral("TLS-Handshake")); break;
    case aap::Session::Discovering: setStatus(QStringLiteral("Dienste abfragen")); break;
    case aap::Session::OpeningChannels: setStatus(QStringLiteral("Kanäle öffnen")); break;
    case aap::Session::Streaming:
        setStatus(QStringLiteral("Verbunden"));
        if (m_session && qobject_cast<UsbAccessoryDevice *>(m_socket.data()))
            rememberCar(m_session->carBluetoothAddress());
        break;
    default: break;
    }
}

void Controller::onVideoStart(const aap::VideoConfig &cfg)
{
    m_screen.setGeometry(cfg.width, cfg.height, cfg.marginWidth, cfg.marginHeight);
    const int kbps = cfg.height >= 1080 ? 8000 : cfg.height >= 720 ? 4000 : 2000;
    QString err;
    if (!m_encoder.start(cfg.width, cfg.height, cfg.fps, kbps, &err)) {
        appendLog(err);
        disconnectHeadUnit();
        return;
    }
    m_screen.setInfo(m_session ? m_session->headUnitName() : QString(), m_encoder.encoderName());
    m_screen.setVersion(version());
    m_queue.clear();
    m_clock.start();
    m_statClock.start();
    m_statFrames = 0;
    m_statBytes = 0;
    m_frameTimer.start(1000 / qMax(1, cfg.fps));
    appendLog(QStringLiteral("Video läuft (%1)").arg(m_encoder.encoderName()));
    startRouter();
    m_mpris.start();
    m_phoneStatus.start();
    m_idleTimer.start();
    startPhonePosition();
    if (m_weatherEnabled && m_parked.valid) m_weather.setPosition(m_parked.lat, m_parked.lon);
    m_nav.start();
    m_calls.start();
    QTimer::singleShot(2000, &m_phonebook, &PhoneBook::load); // after the picture is up
    if (m_notifyToCar) m_notify.start();
    setStatus(QStringLiteral("Projiziert auf %1").arg(m_session ? m_session->headUnitName() : QStringLiteral("Head Unit")));
}

void Controller::onVideoStop()
{
    m_frameTimer.stop();
    m_encoder.stop();
    m_queue.clear();
    emit statusChanged();
}

void Controller::tick()
{
    if (!m_session || !m_encoder.running()) return;
    flush();
    // Back-pressure: the HU acks every frame (OpenAuto allows 1 in flight).
    // Only render when the pipe is nearly empty so latency stays low.
    if (!m_queue.isEmpty() || m_encoder.pending() >= 3) return;
    m_encoder.push(m_screen.render(), static_cast<quint64>(m_clock.nsecsElapsed() / 1000));
}

void Controller::onEncoded(const QByteArray &data, quint64 ts, bool config)
{
    m_queue.enqueue({data, ts, config});
    flush();
}

void Controller::flush()
{
    while (m_session && !m_queue.isEmpty() && m_session->canSendVideo()) {
        Packet p = m_queue.dequeue();
        if (p.config) m_session->sendVideoConfig(p.data);
        else {
            m_session->sendVideoFrame(p.data, p.ts);
            ++m_statFrames;
        }
        m_statBytes += p.data.size();
    }
    if (m_statClock.isValid() && m_statClock.elapsed() >= 2000) {
        const double s = m_statClock.elapsed() / 1000.0;
        m_fps = m_statFrames / s;
        m_kbps = m_statBytes * 8 / 1000.0 / s;
        m_screen.setStats(m_fps, m_kbps);
        m_statFrames = 0;
        m_statBytes = 0;
        m_statClock.restart();
        emit statsChanged();
    }
}

void Controller::onSessionClosed(const QString &reason)
{
    if (!m_session) return;
    m_audioTest.stop();
    stopRouter();
    m_mpris.stop();
    m_phoneStatus.stop();
    m_idleTimer.stop();
    stopPhonePosition();
    m_nav.stop();
    m_calls.stop();
    m_screen.setCall(CallInfo(), false);
    m_lastCall = CallInfo();
    m_pausedForCall = false;
    m_notify.stop();
    m_drivingStatus = -1;
    m_gear = -1;
    m_screen.setParked(false);
    appendLog(QStringLiteral("Sitzung beendet: ") + reason);
    if (m_carFix.valid) saveParked(m_carFix, true);
    m_carFix = Fix();
    if (m_session->receivedBytes() > 0) m_autoReplugs = 0;
    m_usbKick.stop();
    if (m_session->state() != aap::Session::Closed) appendLog(m_session->summary());
    onVideoStop();
    m_session->disconnect(this);
    m_session->deleteLater();
    m_session = nullptr;
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->close();
        m_socket->deleteLater();
    }
    m_fps = m_kbps = 0;
    emit statsChanged();
    if (m_trace) {
        m_trace->text(QStringLiteral("--- Ende ---"));
        delete m_trace;
        m_trace = nullptr;
    }
    setStatus(m_server.isListening() ? QStringLiteral("Warte auf Head Unit")
              : m_usbWanted ? QStringLiteral("Warte auf USB") : QStringLiteral("Bereit"));
    const bool replug = m_replugOnClose;
    m_replugOnClose = false;
    if (m_usbWanted) {
        m_usbRetry.start(); // car may come back (ignition, replug)
        // Car went silent without a USB error: re-register so it sees a fresh device.
        if (replug && m_aoaRequested) {
            m_lastReplug.start();
            appendLog(QStringLiteral("USB: verbinde neu (Gerät kurz ab- und wieder anmelden)"));
            requestAoa(QStringLiteral("replug"));
        }
    }
}

void Controller::appendLog(const QString &line)
{
    const QString l = QTime::currentTime().toString(QStringLiteral("HH:mm:ss ")) + line;
    qInfo("%s", qPrintable(l));
    if (m_trace) m_trace->text(line);
    m_log.append(l);
    while (m_log.size() > 200) m_log.removeFirst();
    emit logChanged();
}

void Controller::setStatus(const QString &s)
{
    m_status = s;
    emit statusChanged();
}
