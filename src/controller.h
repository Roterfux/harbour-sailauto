// Glue between network, protocol session, car UI renderer and encoder.
// Exposed to QML as "controller".
#pragma once

#include "aap/session.h"
#include "aap/trace.h"
#include "audio/audiotest.h"
#include "audio/audiorouter.h"
#include "media/mprisclient.h"
#include "phone/phonestatus.h"
#include "phone/notificationwatcher.h"
#include "phone/callmonitor.h"
#include "phone/phonebook.h"
#include "apps/appcatalog.h"
#include "nav/puremaps.h"
#include "video/carscreen.h"
#include "video/h264encoder.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QStringList>
#include <QTcpServer>
#include <QTimer>

class QTcpSocket;

class Controller : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool listening READ listening NOTIFY listeningChanged)
    Q_PROPERTY(bool connected READ connected NOTIFY statusChanged)
    Q_PROPERTY(bool streaming READ streaming NOTIFY statusChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString headUnit READ headUnit NOTIFY statusChanged)
    Q_PROPERTY(QStringList addresses READ addresses NOTIFY listeningChanged)
    Q_PROPERTY(int port READ port CONSTANT)
    Q_PROPERTY(QString certDir READ certDir CONSTANT)
    Q_PROPERTY(bool customCert READ customCert NOTIFY listeningChanged)
    Q_PROPERTY(QStringList logLines READ logLines NOTIFY logChanged)
    Q_PROPERTY(double fps READ fps NOTIFY statsChanged)
    Q_PROPERTY(double kbps READ kbps NOTIFY statsChanged)
    Q_PROPERTY(bool usbActive READ usbActive NOTIFY listeningChanged)
    Q_PROPERTY(QString traceDir READ traceDir NOTIFY statusChanged)
    Q_PROPERTY(QString version READ version CONSTANT)
    Q_PROPERTY(bool soundToCar READ soundToCar WRITE setSoundToCar NOTIFY soundToCarChanged)
    Q_PROPERTY(bool autostart READ autostart WRITE setAutostart NOTIFY autostartChanged)
    Q_PROPERTY(bool notifyToCar READ notifyToCar WRITE setNotifyToCar NOTIFY notifyToCarChanged)
    // Where the car was parked (last car GPS fix when the connection ended)
    Q_PROPERTY(bool parkedValid READ parkedValid NOTIFY parkedChanged)
    Q_PROPERTY(double parkedLat READ parkedLat NOTIFY parkedChanged)
    Q_PROPERTY(double parkedLon READ parkedLon NOTIFY parkedChanged)
    Q_PROPERTY(double parkedAccuracy READ parkedAccuracy NOTIFY parkedChanged)
    Q_PROPERTY(QString parkedTime READ parkedTime NOTIFY parkedChanged)

public:
    explicit Controller(QObject *parent = nullptr);
    ~Controller() override;

    Q_INVOKABLE bool startListening();
    Q_INVOKABLE void stopListening();
    Q_INVOKABLE void disconnectHeadUnit();
    Q_INVOKABLE void clearLog();
    // Sound test: "media" (melody, 60 s or until stopped) or "guidance" (3 beeps).
    Q_INVOKABLE void soundTest(const QString &kind);
    Q_INVOKABLE void stopSoundTest();
    // USB: Android Open Accessory via /dev/usb_accessory (needs tools/aoa-gadget.sh on).
    Q_INVOKABLE bool startUsb();
    Q_INVOKABLE void stopUsb();
    // Like re-plugging the cable (car stays silent on a reused connection).
    Q_INVOKABLE void replugUsb();
    // Phone silent after SailAuto? Move streams stuck on the silent sink back.
    Q_INVOKABLE void repairAudio();
    // App was launched by the autostart watcher (argument --autostart).
    void startedByCar();
    // Apps for the car screen (phone page "Apps fürs Auto")
    Q_INVOKABLE QVariantList installedApps();
    Q_INVOKABLE void setCarApp(const QString &id, bool on);

    bool listening() const { return m_server.isListening(); }
    bool usbActive() const { return m_usbWanted; }
    QString traceDir() const { return m_lastTraceDir; }
    bool soundToCar() const { return m_soundToCar; }
    QString version() const { return QStringLiteral(APP_VERSION); }
    void setSoundToCar(bool on);
    bool autostart() const { return m_autostart; }
    void setAutostart(bool on);
    bool notifyToCar() const { return m_notifyToCar; }
    bool parkedValid() const { return m_parked.valid; }
    double parkedLat() const { return m_parked.lat; }
    double parkedLon() const { return m_parked.lon; }
    double parkedAccuracy() const { return m_parked.acc; }
    QString parkedTime() const;
    void setNotifyToCar(bool on);
    bool connected() const { return m_session != nullptr; }
    bool streaming() const { return m_encoder.running(); }
    QString status() const { return m_status; }
    QString headUnit() const { return m_session ? m_session->headUnitName() : QString(); }
    QStringList addresses() const;
    int port() const { return m_port; }
    QString certDir() const { return m_certDir; }
    bool customCert() const;
    QStringList logLines() const { return m_log; }
    double fps() const { return m_fps; }
    double kbps() const { return m_kbps; }

signals:
    void listeningChanged();
    void statusChanged();
    void logChanged();
    void statsChanged();
    void soundToCarChanged();
    void autostartChanged();
    void notifyToCarChanged();
    void parkedChanged();

private:
    void onNewConnection();
    void attachTransport(QIODevice *io, const QString &label);
    void tryOpenUsb();
    bool requestAoa(const QString &cmd);
    QString aoaState() const;
    QString phoneBluetoothAddress(QString *why = nullptr) const;
    void onSessionState(aap::Session::State s);
    void onVideoStart(const aap::VideoConfig &cfg);
    void onVideoStop();
    void onEncoded(const QByteArray &data, quint64 ts, bool config);
    void onSessionClosed(const QString &reason);
    void tick();
    void flush();
    void appendLog(const QString &line);
    void setStatus(const QString &s);

    struct Packet { QByteArray data; quint64 ts; bool config; };

    QTcpServer m_server;
    QPointer<QIODevice> m_socket; // TCP socket or USB accessory device
    bool m_usbWanted = false;
    QTimer m_usbRetry;
    bool m_aoaRequested = false;
    bool m_aoaReported = false;
    QElapsedTimer m_aoaWait;
    QTimer m_usbKick;          // car silent after USB open -> replug
    QElapsedTimer m_lastReplug;
    int m_autoReplugs = 0;
    QString m_lastUdcState;    // /sys/class/udc/*/state seen last
    QDateTime m_usbStartedAt;  // root state older than this is stale
    // Autostart
    bool m_autostart = false;
    QTimer m_carPoll;          // /run/harbour-sailauto/car written by the watcher
    QDateTime m_lastCarSeen;
    void checkCarSignal();
    void rememberCar(const QString &mac);
    aap::Session *m_session = nullptr;
    aap::Trace *m_trace = nullptr;
    QString m_traceBase, m_lastTraceDir;
    CarScreen m_screen;
    H264Encoder m_encoder;
    QTimer m_frameTimer;
    AudioTest m_audioTest;
    // Phone sound -> car media channel
    AudioRouter m_router;
    MprisClient m_mpris;
    PhoneStatus m_phoneStatus;
    AppCatalog m_apps;
    NotificationWatcher m_notify;
    PureMapsClient m_nav;
    // Calls
    CallMonitor m_calls;
    PhoneBook m_phonebook;
    CallInfo m_lastCall;
    bool m_callSilenced = false;
    bool m_pausedForCall = false;   // we paused the player; resume when the call is over
    bool m_callHandled = false;     // answered/declined by us (else: missed)
    void onCallChanged();
    void onCallAction(const QString &action);
    bool callBlocksAudio() const;
    bool m_notifyToCar = true;
    int m_drivingStatus = -1;
    // Car GPS / parked position
    struct Fix { bool valid = false; double lat = 0, lon = 0, acc = -1; QDateTime time; };
    Fix m_carFix;           // latest from the car this session
    Fix m_parked;           // saved (QSettings "parked/...")
    QElapsedTimer m_fixSaved;
    void onCarLocation(double lat, double lon, double acc);
    void saveParked(const Fix &f, bool final);
    void pushAppsToScreen();
    void onCarKey(int code, bool pressed, bool longPress);
    void onMediaCommand(const QString &cmd);
    // Spulen: hold NEXT/PREVIOUS (or REWIND/FAST_FORWARD) -> seek in steps.
    QTimer m_holdTimer;   // short press vs. hold on NEXT/PREVIOUS
    QTimer m_seekTimer;   // repeats the seek step while held
    int m_heldKey = 0;
    int m_seekDir = 0;
    int m_seekSteps = 0;
    void startSeeking(int dir);
    bool m_soundToCar = true;
    bool m_carAudioOn = false;
    int m_carAudioChannel = -1;
    qint64 m_carFrames = 0;
    int m_silentMs = 0;
    int m_droppedChunks = 0;
    void startRouter();
    void stopRouter();
    void onPhoneAudio(const QByteArray &pcm);
    void stopCarAudio(const QString &why);
    QElapsedTimer m_clock;
    QQueue<Packet> m_queue;

    int m_port = 5277;
    QString m_certDir;
    QString m_status;
    QStringList m_log;

    int m_statFrames = 0;
    qint64 m_statBytes = 0;
    QElapsedTimer m_statClock;
    double m_fps = 0, m_kbps = 0;
};
