#include "notificationwatcher.h"

#include <QMetaObject>
#include <functional>
#include <dbus/dbus.h>

namespace {
class WatchThread : public QThread
{
public:
    explicit WatchThread(std::function<void()> fn) : m_fn(std::move(fn)) {}
protected:
    void run() override { m_fn(); }
private:
    std::function<void()> m_fn;
};

const char kRule[] = "type='method_call',interface='org.freedesktop.Notifications',member='Notify'";

QString nextString(DBusMessageIter *it)
{
    QString s;
    if (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_STRING) {
        const char *v = nullptr;
        dbus_message_iter_get_basic(it, &v);
        s = QString::fromUtf8(v);
    }
    dbus_message_iter_next(it);
    return s;
}

DBusHandlerResult filter(DBusConnection *, DBusMessage *msg, void *data)
{
    auto *self = static_cast<NotificationWatcher *>(data);
    if (dbus_message_is_method_call(msg, "org.freedesktop.Notifications", "Notify")) {
        // Notify(s app_name, u replaces_id, s app_icon, s summary, s body, as actions, a{sv} hints, i timeout)
        DBusMessageIter it;
        if (dbus_message_iter_init(msg, &it)) {
            const QString app = nextString(&it);
            dbus_message_iter_next(&it); // replaces_id
            dbus_message_iter_next(&it); // app_icon
            QString summary = nextString(&it);
            QString body = nextString(&it);
            dbus_message_iter_next(&it); // actions
            QString category, previewSummary, previewBody, displayName;
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
                DBusMessageIter dict;
                dbus_message_iter_recurse(&it, &dict);
                while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
                    DBusMessageIter entry, var;
                    dbus_message_iter_recurse(&dict, &entry);
                    const QString key = nextString(&entry);
                    if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                        dbus_message_iter_recurse(&entry, &var);
                        if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_STRING) {
                            const char *v = nullptr;
                            dbus_message_iter_get_basic(&var, &v);
                            const QString val = QString::fromUtf8(v);
                            if (key == QLatin1String("category")) category = val;
                            else if (key == QLatin1String("x-nemo-preview-summary")) previewSummary = val;
                            else if (key == QLatin1String("x-nemo-preview-body")) previewBody = val;
                            else if (key == QLatin1String("x-nemo-display-name")) displayName = val;
                        }
                    }
                    dbus_message_iter_next(&dict);
                }
            }
            // Sailfish apps put the popup text into the preview hints.
            if (!previewSummary.isEmpty() || !previewBody.isEmpty()) {
                summary = previewSummary;
                body = previewBody;
            }
            if (!summary.isEmpty() || !body.isEmpty())
                self->deliver(displayName.isEmpty() ? app : displayName, summary, body, category);
        }
    }
    // A monitor must never answer anything: claim every message.
    return DBUS_HANDLER_RESULT_HANDLED;
}
} // namespace

NotificationWatcher::NotificationWatcher(QObject *parent) : QObject(parent) {}

NotificationWatcher::~NotificationWatcher()
{
    stop();
}

void NotificationWatcher::start()
{
    if (m_thread) return;
    m_stop = false;
    m_thread = new WatchThread([this]() { run(); });
    m_thread->start();
}

void NotificationWatcher::stop()
{
    if (!m_thread) return;
    m_stop = true;
    m_thread->wait(2000);
    delete m_thread;
    m_thread = nullptr;
}

void NotificationWatcher::deliver(const QString &app, const QString &title, const QString &body, const QString &category)
{
    QMetaObject::invokeMethod(this, "notification", Qt::QueuedConnection, Q_ARG(QString, app),
                              Q_ARG(QString, title), Q_ARG(QString, body), Q_ARG(QString, category));
}

void NotificationWatcher::report(const QString &line)
{
    QMetaObject::invokeMethod(this, "log", Qt::QueuedConnection, Q_ARG(QString, line));
}

void NotificationWatcher::run()
{
    DBusError err;
    dbus_error_init(&err);
    DBusConnection *conn = dbus_bus_get_private(DBUS_BUS_SESSION, &err);
    if (!conn) {
        report(QStringLiteral("Benachrichtigungen: kein Session-D-Bus (%1)").arg(QString::fromUtf8(err.message)));
        dbus_error_free(&err);
        return;
    }
    dbus_connection_set_exit_on_disconnect(conn, FALSE);

    // dbus >= 1.9.10: BecomeMonitor. Older: eavesdropping match rule.
    bool ok = false;
    DBusMessage *call = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
                                                     "org.freedesktop.DBus.Monitoring", "BecomeMonitor");
    const char *rule = kRule;
    const char **rules = &rule;
    dbus_uint32_t flags = 0;
    dbus_message_append_args(call, DBUS_TYPE_ARRAY, DBUS_TYPE_STRING, &rules, 1, DBUS_TYPE_UINT32, &flags,
                             DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, call, 3000, &err);
    dbus_message_unref(call);
    if (reply) {
        dbus_message_unref(reply);
        ok = true;
        report(QStringLiteral("Benachrichtigungen: beobachte (D-Bus-Monitor)"));
    } else {
        const QString why = QString::fromUtf8(err.message);
        dbus_error_free(&err);
        dbus_bus_add_match(conn, (QByteArray("eavesdrop=true,") + kRule).constData(), &err);
        if (dbus_error_is_set(&err)) {
            report(QStringLiteral("Benachrichtigungen: Beobachten nicht erlaubt (%1; %2)")
                       .arg(why, QString::fromUtf8(err.message)));
            dbus_error_free(&err);
        } else {
            ok = true;
            report(QStringLiteral("Benachrichtigungen: beobachte (eavesdrop, %1)").arg(why));
        }
    }
    if (ok) {
        dbus_connection_add_filter(conn, filter, this, nullptr);
        while (!m_stop && dbus_connection_read_write_dispatch(conn, 200)) {}
        dbus_connection_remove_filter(conn, filter, this);
        if (!m_stop) report(QStringLiteral("Benachrichtigungen: D-Bus-Verbindung verloren"));
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
}
