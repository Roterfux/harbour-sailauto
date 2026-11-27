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
    QImage photo;         // small (72 px), null if none
    bool favorite = false;
};

class PhoneBook : public QObject
{
    Q_OBJECT
public:
    explicit PhoneBook(QObject *parent = nullptr);
    // Reads the address book (contacts with at least one number), sorted by name.
    void load();
    const QVector<PhoneContact> &contacts() const { return m_contacts; }

signals:
    void loaded();
    void log(const QString &line);

private:
    QVector<PhoneContact> m_contacts;
};
