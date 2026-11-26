// Phone status for the car's top bar: battery, mobile network, Wi-Fi,
// Bluetooth. Sources: sysfs power_supply (statefs as fallback), oFono,
// ConnMan and BlueZ on the system D-Bus. Polled every 10 s; every source is
// optional (desktop test build has none of them).
#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

struct PhoneState {
    int battery = -1;          // percent, -1 unknown
    bool charging = false;
    int signal = -1;           // percent, -1 no modem / not registered
    QString operatorName, tech; // tech: "2G", "3G", "4G", "5G"
    bool roaming = false;
    bool wifi = false;
    int wifiStrength = -1;
    QString wifiName;
    bool mobileData = false;
    bool btOn = false;
    int btConnected = 0;
    QString btDevice;          // first connected device
    bool operator==(const PhoneState &o) const;
    bool operator!=(const PhoneState &o) const { return !(*this == o); }
    QString summary() const;
};

class PhoneStatus : public QObject
{
    Q_OBJECT
public:
    explicit PhoneStatus(QObject *parent = nullptr);
    void start();
    void stop();
    const PhoneState &state() const { return m_state; }

signals:
    void changed();
    void log(const QString &line);

private:
    void poll();
    void readBattery(PhoneState &s) const;
    void readModem(PhoneState &s) const;
    void readConnman(PhoneState &s) const;
    void readBluetooth(PhoneState &s) const;

    QTimer m_timer;
    PhoneState m_state;
    bool m_logged = false;
};
