// One Android Auto projection session, phone ("mobile device") side.
//
// Flow (HU = head unit, here OpenAuto):
//   HU  VERSION_REQUEST            -> phone VERSION_RESPONSE
//   HU  SSL_HANDSHAKE <-> phone SSL_HANDSHAKE   (HU is TLS client)
//   HU  AUTH_COMPLETE
//   phone SERVICE_DISCOVERY_REQUEST -> HU SERVICE_DISCOVERY_RESPONSE (channels)
//   phone CHANNEL_OPEN_REQUEST per channel -> HU CHANNEL_OPEN_RESPONSE
//   video: SETUP_REQUEST -> SETUP_RESPONSE, START_INDICATION, VIDEO_FOCUS_REQUEST
//          -> HU VIDEO_FOCUS_INDICATION, then H.264 media + HU MEDIA_ACK
//   input: BINDING_REQUEST -> BINDING_RESPONSE, then HU INPUT_EVENT_INDICATION (touch)
//   sensor: SENSOR_START_REQUEST(night, driving) -> SENSOR_EVENT_INDICATION
//   HU PING_REQUEST every 5 s -> phone PING_RESPONSE (or the HU drops us)
#pragma once

#include "cryptor.h"
#include "protocol.h"
#include "trace.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QElapsedTimer>

class QIODevice;

namespace aap {

struct VideoConfig
{
    int width = 800;
    int height = 480;
    int marginWidth = 0;
    int marginHeight = 0;
    int dpi = 140;
    int fps = 30;
    int touchWidth = 800;
    int touchHeight = 480;
};

class Session : public QObject
{
    Q_OBJECT
public:
    enum State { Idle, Versioning, Handshaking, Discovering, OpeningChannels, Streaming, Closed };
    Q_ENUM(State)

    Session(QIODevice *transport, const QString &certPath, const QString &keyPath,
            QObject *parent = nullptr);

    bool start(QString *error);
    // Optional recorder (owned by the caller), see trace.h.
    void setTrace(Trace *trace) { m_trace = trace; }
    // Phone Bluetooth address for the car's pairing (empty = skip).
    void setPhoneBluetoothAddress(const QString &a) { m_phoneBt = a; }
    QString summary() const;
    qint64 receivedBytes() const { return m_rxTotal; }
    QString carBluetoothAddress() const { return m_carBt; }
    State state() const { return m_state; }
    QString headUnitName() const { return m_huName; }
    VideoConfig videoConfig() const { return m_video; }

    // Video sink. Returns false if the HU has too many frames unacknowledged.
    bool canSendVideo() const;
    void sendVideoConfig(const QByteArray &spsPps);
    void sendVideoFrame(const QByteArray &accessUnit, quint64 timestampUs);

    void requestShutdown();

    // ---- Audio out (phone -> car). streamType: 1 guidance, 2 system, 3 media.
    // Returns the channel id, or -1 if the car has no such (set-up) channel.
    int audioChannel(int streamType) const;
    void requestAudioFocus(int request); // 1 GAIN, 2 TRANSIENT, 3 TRANSIENT_MAY_DUCK, 4 RELEASE
    void audioFormat(int channel, int *rate, int *channels) const; // 16-bit PCM
    bool startAudio(int channel);
    void stopAudio(int channel);
    bool canSendAudio(int channel) const;
    void sendAudio(int channel, const QByteArray &pcm, quint64 timestampUs);

signals:
    void stateChanged(aap::Session::State state);
    void log(const QString &line);
    void videoStart(const aap::VideoConfig &config);
    void videoStop();
    void videoCanSend();
    void touch(int action, int x, int y);
    // Hardware key from the car (Android keycodes, e.g. 85 play/pause, 87 next, 88 previous).
    void key(int code, bool pressed, bool longPress);
    void nightModeChanged(bool night);
    void drivingStatusChanged(int status);
    // Car GPS (sensor LOCATION): degrees, accuracy in m (-1 unknown)
    void locationChanged(double lat, double lon, double accuracyM, double bearingDeg, double speedMs);
    // GearData: 0 neutral, 1..10 gears, 100 drive, 101 park, 102 reverse
    void gearChanged(int gear);
    void speedChanged(double metersPerSecond); // car's speed sensor (wheels)
    // Streaming, but the car sent nothing for ~20 s (switched off / stale USB)
    void carSilent();
    // Audio focus from the car; unsolicited = the car decided (not an answer to us)
    void audioFocusNotified(int state, bool unsolicited);
    // AudioFocusStateType from the car: 1 GAIN, 2 GAIN_TRANSIENT, 3 LOSS, 4 LOSS_CAN_DUCK, 5 LOSS_TRANSIENT, ...
    void audioFocusChanged(int state);
    // An audio channel finished its setup (streamType: 1 guidance, 2 system, 3 media).
    void audioReady(int streamType);
    void closed(const QString &reason);
    // No byte from the HU 5 s after the transport came up.
    void silent();

private slots:
    void onReadyRead();

private:
    struct Partial
    {
        QByteArray data;
        quint32 total = 0;
        quint8 flags = 0;
    };

    void setState(State s);
    void fail(const QString &reason);
    void handleMessage(quint8 channel, quint8 flags, const QByteArray &msg);
    void handleControl(quint16 id, const QByteArray &payload);
    void handleVideo(quint16 id, const QByteArray &payload);
    void handleInput(quint16 id, const QByteArray &payload);
    void handleSensor(quint16 id, const QByteArray &payload);
    void handleAudio(quint8 channel, quint16 id, const QByteArray &payload);
    void handleBluetooth(quint16 id, const QByteArray &payload);
    void handleServiceDiscovery(const QByteArray &payload);
    void onChannelOpened(quint8 channel, bool ok);

    void send(quint8 channel, quint16 id, const QByteArray &payload, bool encrypted,
              bool controlType = false);
    void sendRaw(quint8 channel, quint8 flags, const QByteArray &msg);

    QPointer<QIODevice> m_io;
    QString m_certPath, m_keyPath;
    Cryptor m_crypto;
    State m_state = Idle;
    QByteArray m_rx;
    QHash<quint8, Partial> m_partials;

    QString m_huName;
    VideoConfig m_video;
    int m_videoChannel = -1;
    int m_inputChannel = -1;
    int m_sensorChannel = -1;
    int m_pendingOpens = 0;
    // Every channel the HU offered: id -> kind; audio channels -> stream type
    QHash<int, Trace::Kind> m_kinds;
    QHash<int, int> m_audioType;
    struct AudioState { bool setupOk = false; int maxUnacked = 1; int unacked = 0; int session = 0; bool started = false;
                       int rate = 48000; int channels = 2; };
    QHash<int, AudioState> m_audio;
    QString m_carBt;
    QList<int> m_btMethods;
    QString m_phoneBt;
    Trace *m_trace = nullptr;
    QTimer m_watchdog;       // logs when the handshake hangs in one state
    qint64 m_txTotal = 0;
    int m_framesSent = 0;
    QElapsedTimer m_age;
    bool m_touchInVideoSpace = false;
    QList<int> m_keycodes;

    int m_videoSession = 1;
    int m_maxUnacked = 1;
    int m_unacked = 0;
    bool m_videoFocused = false;
    QTimer m_focusFallback;
    QTimer m_silence;
    qint64 m_rxTotal = 0;
    qint64 m_rxAtCheck = -1;  // liveness check while streaming
    int m_silentChecks = 0;
    int m_heldLongKey = -1;
    QList<int> m_sensorTypes; // offered by the car
};

} // namespace aap

Q_DECLARE_METATYPE(aap::VideoConfig)
