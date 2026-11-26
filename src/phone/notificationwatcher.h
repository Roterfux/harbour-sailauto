// Sees the phone's notifications (calls to org.freedesktop.Notifications.Notify
// on the session bus) by becoming a D-Bus monitor, the way dbus-monitor does.
// Uses libdbus directly on its own thread: a QtDBus connection would answer
// the monitored calls itself and get thrown off the bus.
#pragma once

#include <QObject>
#include <QString>
#include <QThread>
#include <atomic>

class NotificationWatcher : public QObject
{
    Q_OBJECT
public:
    explicit NotificationWatcher(QObject *parent = nullptr);
    ~NotificationWatcher() override;

    void start();
    void stop();
    bool running() const { return m_thread != nullptr; }

    // Called on the watcher thread; forwards to the signal on the main thread.
    void deliver(const QString &app, const QString &title, const QString &body, const QString &category);
    void report(const QString &line);

signals:
    void notification(const QString &app, const QString &title, const QString &body, const QString &category);
    void log(const QString &line);

private:
    void run();
    QThread *m_thread = nullptr;
    std::atomic<bool> m_stop{false};
};
