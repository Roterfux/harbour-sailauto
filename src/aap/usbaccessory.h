// QIODevice over /dev/usb_accessory (Android Open Accessory function of the
// kernel's USB gadget). The car / OpenAuto is the USB host; the AAP byte
// stream is the same as over TCP. Reads run in a thread because the device
// only supports blocking I/O.
#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QMutex>
#include <QThread>
#include <atomic>

class UsbAccessoryDevice : public QIODevice
{
    Q_OBJECT
public:
    explicit UsbAccessoryDevice(const QString &path = QStringLiteral("/dev/usb_accessory"),
                                QObject *parent = nullptr);
    ~UsbAccessoryDevice() override;

    bool open(OpenMode mode) override;
    void close() override;
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override;
    QString path() const { return m_path; }
    void runReader(); // reader thread entry

signals:
    void disconnected();

protected:
    qint64 readData(char *data, qint64 maxlen) override;
    qint64 writeData(const char *data, qint64 len) override;

private:
    void readerLoop();

    QString m_path;
    int m_fd = -1;
    QThread *m_reader = nullptr;
    std::atomic<bool> m_stop{false};
    mutable QMutex m_lock;
    QByteArray m_buffer;
};
