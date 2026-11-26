#include "usbaccessory.h"

#include <QMetaObject>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

// From the Android kernel's include/uapi/linux/usb/f_accessory.h
#define ACCESSORY_GET_STRING_MANUFACTURER _IOW('M', 1, char[256])
#define ACCESSORY_GET_STRING_MODEL _IOW('M', 2, char[256])
#define ACCESSORY_GET_STRING_DESCRIPTION _IOW('M', 3, char[256])
#define ACCESSORY_GET_STRING_VERSION _IOW('M', 4, char[256])
#define ACCESSORY_GET_STRING_URI _IOW('M', 5, char[256])
#define ACCESSORY_GET_STRING_SERIAL _IOW('M', 6, char[256])
#define ACCESSORY_IS_START_REQUESTED _IO('M', 7)

namespace {
// Qt 5.6 has no QThread::create().
class ReaderThread : public QThread
{
public:
    explicit ReaderThread(UsbAccessoryDevice *dev) : m_dev(dev) {}
protected:
    void run() override { m_dev->runReader(); }
private:
    UsbAccessoryDevice *m_dev;
};
}

void UsbAccessoryDevice::runReader()
{
    readerLoop();
}

UsbAccessoryDevice::UsbAccessoryDevice(const QString &path, QObject *parent)
    : QIODevice(parent), m_path(path)
{
}

UsbAccessoryDevice::~UsbAccessoryDevice()
{
    close();
}

bool UsbAccessoryDevice::open(OpenMode mode)
{
    m_fd = ::open(m_path.toLocal8Bit().constData(), O_RDWR);
    if (m_fd < 0) {
        setErrorString(QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    QIODevice::open(mode | QIODevice::Unbuffered);
    m_stop = false;
    m_reader = new ReaderThread(this);
    m_reader->start();
    return true;
}

void UsbAccessoryDevice::close()
{
    if (m_fd < 0) return;
    m_stop = true;
    // Closing the fd makes the blocking read() return.
    ::close(m_fd);
    m_fd = -1;
    if (m_reader) {
        if (!m_reader->wait(2000)) {
            // Driver did not wake the blocked read(); last resort.
            m_reader->terminate();
            m_reader->wait(500);
        }
        delete m_reader;
        m_reader = nullptr;
    }
    QIODevice::close();
}

void UsbAccessoryDevice::readerLoop()
{
    // f_accessory wants reads of at least one bulk packet; 16 KiB covers
    // a whole AAP frame.
    QByteArray buf(16384, Qt::Uninitialized);
    while (!m_stop) {
        const ssize_t n = ::read(m_fd, buf.data(), static_cast<size_t>(buf.size()));
        if (n > 0) {
            {
                QMutexLocker l(&m_lock);
                m_buffer.append(buf.constData(), static_cast<int>(n));
            }
            QMetaObject::invokeMethod(this, "readyRead", Qt::QueuedConnection);
            continue;
        }
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            QThread::msleep(5);
            continue;
        }
        // 0 / ENODEV / ESHUTDOWN: host went away (cable pulled, car off).
        if (!m_stop) QMetaObject::invokeMethod(this, "disconnected", Qt::QueuedConnection);
        break;
    }
}

QString UsbAccessoryDevice::handshakeInfo() const
{
    if (m_fd < 0) return QStringLiteral("Gerät nicht offen");
    auto get = [this](unsigned long req) {
        char buf[256];
        std::memset(buf, 0, sizeof(buf));
        if (::ioctl(m_fd, req, buf) < 0) return QStringLiteral("-");
        return QString::fromUtf8(buf);
    };
    const int start = ::ioctl(m_fd, ACCESSORY_IS_START_REQUESTED);
    return QStringLiteral("Hersteller=\"%1\" Modell=\"%2\" Beschreibung=\"%3\" Version=\"%4\" URI=\"%5\" Serie=\"%6\" START=%7")
        .arg(get(ACCESSORY_GET_STRING_MANUFACTURER), get(ACCESSORY_GET_STRING_MODEL),
             get(ACCESSORY_GET_STRING_DESCRIPTION), get(ACCESSORY_GET_STRING_VERSION),
             get(ACCESSORY_GET_STRING_URI), get(ACCESSORY_GET_STRING_SERIAL))
        .arg(start < 0 ? QStringLiteral("?") : start ? QStringLiteral("ja") : QStringLiteral("nein"));
}

qint64 UsbAccessoryDevice::bytesAvailable() const
{
    QMutexLocker l(&m_lock);
    return m_buffer.size() + QIODevice::bytesAvailable();
}

qint64 UsbAccessoryDevice::readData(char *data, qint64 maxlen)
{
    QMutexLocker l(&m_lock);
    const int n = static_cast<int>(qMin<qint64>(maxlen, m_buffer.size()));
    std::memcpy(data, m_buffer.constData(), static_cast<size_t>(n));
    m_buffer.remove(0, n);
    return n;
}

qint64 UsbAccessoryDevice::writeData(const char *data, qint64 len)
{
    if (m_fd < 0) return -1;
    qint64 done = 0;
    while (done < len) {
        const ssize_t n = ::write(m_fd, data + done, static_cast<size_t>(len - done));
        if (n < 0) {
            if (errno == EINTR) continue;
            setErrorString(QString::fromLocal8Bit(strerror(errno)));
            return done ? done : -1;
        }
        done += n;
    }
    return done;
}
