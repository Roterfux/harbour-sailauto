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

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QTimer>

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
    State state() const { return m_state; }
    QString headUnitName() const { return m_huName; }
    VideoConfig videoConfig() const { return m_video; }

    // Video sink. Returns false if the HU has too many frames unacknowledged.
    bool canSendVideo() const;
    void sendVideoConfig(const QByteArray &spsPps);
    void sendVideoFrame(const QByteArray &accessUnit, quint64 timestampUs);

    void requestShutdown();

signals:
    void stateChanged(aap::Session::State state);
    void log(const QString &line);
    void videoStart(const aap::VideoConfig &config);
    void videoStop();
    void videoCanSend();
    void touch(int action, int x, int y);
    void nightModeChanged(bool night);
    void drivingStatusChanged(int status);
    void closed(const QString &reason);

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

    int m_videoSession = 1;
    int m_maxUnacked = 1;
    int m_unacked = 0;
    bool m_videoFocused = false;
    QTimer m_focusFallback;
    QTimer m_silence;
    qint64 m_rxTotal = 0;
};

} // namespace aap

Q_DECLARE_METATYPE(aap::VideoConfig)
