// Phone calls through Sailfish's voicecall manager (session D-Bus,
// org.nemomobile.voicecall): see incoming/active calls, answer, hang up,
// silence the ringtone. Polls twice a second while started.
// Caller name and photo come from the address book (QtContacts) if allowed.
#pragma once

#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "phonebook.h"

struct CallInfo {
    enum State { None, Incoming, Active, Outgoing, Held };
    State state = None;
    QString id;          // voicecall handler id
    QString number;
    QString name;        // from the address book, empty if unknown
    QImage photo;        // from the address book, null if none
    QDateTime startedAt; // set when it becomes active
    bool operator==(const CallInfo &o) const
    {
        return state == o.state && id == o.id && number == o.number && name == o.name;
    }
};

class CallMonitor : public QObject
{
    Q_OBJECT
public:
    explicit CallMonitor(QObject *parent = nullptr);
    void start();
    void stop();
    const CallInfo &call() const { return m_call; }

    bool answer();
    bool hangup();
    bool silenceRingtone();
    // Outgoing call over the first cellular provider (SIM).
    bool dial(const QString &number, const QString &name = QString(), const QImage &photo = QImage());
    // Caller name/photo come from the phonebook (loaded by PhoneBook).
    void setContacts(const QVector<PhoneContact> &contacts);

signals:
    void changed();
    void log(const QString &line);

private:
    void poll();
    void lookupContact(const QString &number, QString *name, QImage *photo);

    QTimer m_timer;
    CallInfo m_call;
    QHash<QString, int> m_byNumber; // last 8 digits -> index in m_contacts
    QVector<PhoneContact> m_contacts;
    bool m_serviceErrorLogged = false;
    // Name/photo of the number we dialed (the line id may be formatted differently)
    QString m_dialKey, m_dialName;
    QImage m_dialPhoto;
};
