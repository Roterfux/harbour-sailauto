// Glue between network, protocol session, car UI renderer and encoder.
// Exposed to QML as "controller".
#pragma once

#include "aap/session.h"
#include "video/carscreen.h"
#include "video/h264encoder.h"

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

public:
    explicit Controller(QObject *parent = nullptr);
    ~Controller() override;

    Q_INVOKABLE bool startListening();
    Q_INVOKABLE void stopListening();
    Q_INVOKABLE void disconnectHeadUnit();
    Q_INVOKABLE void clearLog();

    bool listening() const { return m_server.isListening(); }
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

private:
    void onNewConnection();
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
    QPointer<QTcpSocket> m_socket;
    aap::Session *m_session = nullptr;
    CarScreen m_screen;
    H264Encoder m_encoder;
    QTimer m_frameTimer;
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
