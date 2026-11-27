// Address book for the car's phone page: contacts with phone numbers,
// photos and favorites. Sailfish: QtContacts (qtcontacts-sqlite).
// Test builds without QtContacts: SAILAUTO_FAKE_CONTACTS=<json file>
// [{"name": "...", "favorite": true, "photo": "/path.png",
//   "numbers": [{"number": "+49...", "label": "Mobil"}]}]
#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

struct PhoneContact {
    QString name;
    QStringList numbers;
    QStringList labels;   // "Mobil", "Privat", "Arbeit", ... per number
    QStringList addresses; // postal addresses, one line each (for navigation)
    QImage photo;         // small (72 px), null if none
    bool favorite = false;
};

struct RecentCall {
    QString number;
    qint64 time = 0;      // seconds since epoch
    bool outgoing = false;
    bool missed = false;
};

class QProcess;

class PhoneBook : public QObject
{
    Q_OBJECT
public:
    explicit PhoneBook(QObject *parent = nullptr);
    // Reads the address book (contacts with at least one number), sorted by
    // name. On Sailfish the contacts database is only readable for the group
    // "privileged", so a setgid copy of this binary reads it
    // (/usr/libexec/harbour-sailauto/contacts-helper --dump-contacts).
    void load();
    // Entry point of that helper: prints the contacts as JSON, returns exit code.
    static int dumpMain(int argc, char *argv[]);
    const QVector<PhoneContact> &contacts() const { return m_contacts; }
    const QVector<RecentCall> &recentCalls() const { return m_calls; }

signals:
    void loaded();
    void log(const QString &line);

private:
    static QVector<PhoneContact> loadDirect(QString *error);
    static QVector<PhoneContact> fromJson(const QByteArray &json, QVector<RecentCall> *calls);
    static QVector<RecentCall> loadCalls(QString *error);
    void finish(QVector<PhoneContact> list, qint64 ms, const QString &source);
    QVector<PhoneContact> m_contacts;
    QVector<RecentCall> m_calls;
    QProcess *m_helper = nullptr;
};
