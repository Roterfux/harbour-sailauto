// TLS layer of AAP. The head unit (HU) is the TLS client, the phone is the
// TLS server. TLS records travel inside AAP messages, so OpenSSL runs on
// memory BIOs instead of a socket.
#pragma once

#include <QByteArray>
#include <QString>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;
typedef struct bio_st BIO;

namespace aap {

class Cryptor
{
public:
    Cryptor();
    ~Cryptor();

    // Loads PEM certificate + key. If both paths are empty or missing, a
    // self-signed development certificate is generated (enough for OpenAuto,
    // which does not verify the phone; a real car does).
    bool init(const QString &certPath, const QString &keyPath, QString *error);

    // Feed handshake bytes from the HU; returns bytes to send back (may be empty).
    QByteArray handshake(const QByteArray &in);
    bool handshakeDone() const;

    QByteArray encrypt(const QByteArray &plain);
    QByteArray decrypt(const QByteArray &cipher, bool *ok);

    QString certificateInfo() const { return m_certInfo; }
    // Last handshake failure (OpenSSL reason, alert); empty if none.
    QString handshakeError() const { return m_handshakeError; }
    // After the handshake: protocol, cipher, the HU's certificate subject/issuer.
    QString sessionInfo() const;
    QString lastError() const;

private:
    QByteArray drainWriteBio();

    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;
    BIO *m_rbio = nullptr; // network -> SSL
    BIO *m_wbio = nullptr; // SSL -> network
    QString m_certInfo;
    QString m_handshakeError;
};

} // namespace aap
