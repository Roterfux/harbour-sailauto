#include "usbaccessory.h"

#include <QMetaObject>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

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
