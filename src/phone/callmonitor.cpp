#include "callmonitor.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusVariant>
#include <QUrl>
#include <QVariantMap>


namespace {
QString numberKey(const QString &n)
{
    QString d;
    for (const QChar ch : n)
        if (ch.isDigit()) d += ch;
    return d.right(8); // ignores +49 / 0 prefixes
}

const char kService[] = "org.nemomobile.voicecall";
const char kManagerIface[] = "org.nemomobile.voicecall.VoiceCallManager";
const char kCallIface[] = "org.nemomobile.voicecall.VoiceCall";

// voicecall AbstractVoiceCallHandler::VoiceCallStatus
enum { StNull, StActive, StHeld, StDialing, StAlerting, StIncoming, StWaiting, StDisconnected, StRejected, StIgnored };

QVariantMap getAll(const QString &path, const char *iface, QString *error = nullptr)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), path,
                                                    QStringLiteral("org.freedesktop.DBus.Properties"),
                                                    QStringLiteral("GetAll"));
    m << QLatin1String(iface);
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 400);
    QVariantMap out;
    if (r.type() == QDBusMessage::ReplyMessage && !r.arguments().isEmpty())
        r.arguments().first().value<QDBusArgument>() >> out;
    else if (error)
        *error = r.errorMessage();
    return out;
}

bool callMethod(const QString &path, const char *iface, const char *method, QString *error)
{
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), path, QLatin1String(iface),
                                                    QLatin1String(method));
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 2000);
    if (r.type() == QDBusMessage::ErrorMessage) {
        *error = r.errorMessage();
        return false;
    }
    // answer()/hangup() return bool
    if (!r.arguments().isEmpty() && r.arguments().first().type() == QVariant::Bool && !r.arguments().first().toBool()) {
        *error = QStringLiteral("abgelehnt");
        return false;
    }
    return true;
}
} // namespace

CallMonitor::CallMonitor(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(500);
    connect(&m_timer, &QTimer::timeout, this, &CallMonitor::poll);
}

void CallMonitor::start()
{
    poll();
    m_timer.start();
}

void CallMonitor::stop()
{
    m_timer.stop();
}

void CallMonitor::poll()
{
    QString err;
    const QVariantMap mgr = getAll(QStringLiteral("/"), kManagerIface, &err);
    if (mgr.isEmpty()) {
        if (!m_serviceErrorLogged) {
            m_serviceErrorLogged = true;
            emit log(QStringLiteral("Anrufe: voicecall-Dienst nicht erreichbar (%1)").arg(err));
        }
    } else if (m_serviceErrorLogged) {
        m_serviceErrorLogged = false;
    }

    // Pick the call that matters most: ringing > active > outgoing > held.
    CallInfo best;
    int bestRank = 0;
    for (const QString &id : mgr.value(QStringLiteral("voiceCalls")).toStringList()) {
        const QString path = QStringLiteral("/calls/") + id;
        const QVariantMap c = getAll(path, kCallIface);
        const int st = c.value(QStringLiteral("status")).toInt();
        CallInfo ci;
        int rank = 0;
        switch (st) {
        case StIncoming: case StWaiting: ci.state = CallInfo::Incoming; rank = 4; break;
        case StActive: ci.state = CallInfo::Active; rank = 3; break;
        case StDialing: case StAlerting: ci.state = CallInfo::Outgoing; rank = 2; break;
        case StHeld: ci.state = CallInfo::Held; rank = 1; break;
        default: break;
        }
        if (rank <= bestRank) continue;
        ci.id = id;
        ci.number = c.value(QStringLiteral("lineId")).toString();
        ci.startedAt = c.value(QStringLiteral("startedAt")).toDateTime();
        best = ci;
        bestRank = rank;
    }
    if (best.state != CallInfo::None) {
        if (best.number == m_call.number && best.id == m_call.id) {
            best.name = m_call.name;
            best.photo = m_call.photo;
        } else if (!m_dialKey.isEmpty() && numberKey(best.number) == m_dialKey) {
            best.name = m_dialName;
            best.photo = m_dialPhoto;
        } else {
            lookupContact(best.number, &best.name, &best.photo);
        }
        if (best.state == CallInfo::Active && !best.startedAt.isValid())
            best.startedAt = m_call.state == CallInfo::Active ? m_call.startedAt : QDateTime::currentDateTime();
    }
    if (!(best == m_call)) {
        m_call = best;
        emit changed();
    }
}

bool CallMonitor::answer()
{
    if (m_call.id.isEmpty()) return false;
    QString err;
    const bool ok = callMethod(QStringLiteral("/calls/") + m_call.id, kCallIface, "answer", &err);
    emit log(ok ? QStringLiteral("Anruf: angenommen") : QStringLiteral("Anruf: Annehmen fehlgeschlagen (%1)").arg(err));
    QTimer::singleShot(200, this, &CallMonitor::poll);
    return ok;
}

bool CallMonitor::hangup()
{
    if (m_call.id.isEmpty()) return false;
    QString err;
    const bool ok = callMethod(QStringLiteral("/calls/") + m_call.id, kCallIface, "hangup", &err);
    emit log(ok ? QStringLiteral("Anruf: aufgelegt/abgelehnt") : QStringLiteral("Anruf: Auflegen fehlgeschlagen (%1)").arg(err));
    QTimer::singleShot(200, this, &CallMonitor::poll);
    return ok;
}

bool CallMonitor::silenceRingtone()
{
    QString err;
    const bool ok = callMethod(QStringLiteral("/"), kManagerIface, "silenceRingtone", &err);
    emit log(ok ? QStringLiteral("Anruf: Klingelton stumm") : QStringLiteral("Anruf: Stumm fehlgeschlagen (%1)").arg(err));
    return ok;
}

bool CallMonitor::dial(const QString &number, const QString &name, const QImage &photo)
{
    m_dialKey = numberKey(number);
    m_dialName = name;
    m_dialPhoto = photo;
    QString err;
    const QVariantMap mgr = getAll(QStringLiteral("/"), kManagerIface, &err);
    // providers: "providerId:providerType", e.g. "ril_0:cellular"
    const QStringList providers = mgr.value(QStringLiteral("providers")).toStringList();
    QString provider;
    for (const QString &pr : providers)
        if (pr.endsWith(QLatin1String(":cellular"))) { provider = pr.section(QLatin1Char(':'), 0, -2); break; }
    if (provider.isEmpty() && !providers.isEmpty()) provider = providers.first().section(QLatin1Char(':'), 0, -2);
    if (provider.isEmpty()) {
        emit log(QStringLiteral("Anruf: kein Anbieter/SIM gefunden (%1)").arg(err.isEmpty() ? providers.join(QLatin1Char(',')) : err));
        return false;
    }
    QDBusMessage m = QDBusMessage::createMethodCall(QLatin1String(kService), QStringLiteral("/"),
                                                    QLatin1String(kManagerIface), QStringLiteral("dial"));
    m << provider << number;
    const QDBusMessage r = QDBusConnection::sessionBus().call(m, QDBus::Block, 5000);
    const bool ok = r.type() == QDBusMessage::ReplyMessage && (r.arguments().isEmpty() || r.arguments().first().toBool());
    emit log(ok ? QStringLiteral("Anruf: wähle (über %1)").arg(provider)
                : QStringLiteral("Anruf: Wählen fehlgeschlagen (%1)").arg(r.errorMessage().isEmpty() ? QStringLiteral("abgelehnt") : r.errorMessage()));
    QTimer::singleShot(300, this, &CallMonitor::poll);
    return ok;
}

void CallMonitor::setContacts(const QVector<PhoneContact> &contacts)
{
    m_contacts = contacts;
    m_byNumber.clear();
    for (int i = 0; i < m_contacts.size(); ++i)
        for (const QString &n : m_contacts[i].numbers) {
            const QString k = numberKey(n);
            if (k.size() >= 6) m_byNumber.insert(k, i);
        }
}

void CallMonitor::lookupContact(const QString &number, QString *name, QImage *photo)
{
    const int i = m_byNumber.value(numberKey(number), -1);
    if (i < 0) return;
    *name = m_contacts[i].name;
    *photo = m_contacts[i].photo;
}
