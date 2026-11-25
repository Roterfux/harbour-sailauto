#include "cryptor.h"

#include <QFile>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace aap {

namespace {

QString opensslErrors()
{
    QString s;
    unsigned long e;
    char buf[256];
    while ((e = ERR_get_error()) != 0) {
        ERR_error_string_n(e, buf, sizeof(buf));
        if (!s.isEmpty()) s += QStringLiteral("; ");
        s += QString::fromLatin1(buf);
    }
    return s;
}

bool generateSelfSigned(EVP_PKEY **outKey, X509 **outCert)
{
    EVP_PKEY *key = nullptr;
    EVP_PKEY_CTX *kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (!kctx || EVP_PKEY_keygen_init(kctx) <= 0
        || EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) <= 0
        || EVP_PKEY_keygen(kctx, &key) <= 0) {
        EVP_PKEY_CTX_free(kctx);
        return false;
    }
    EVP_PKEY_CTX_free(kctx);

    X509 *cert = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(cert), 1);
    X509_gmtime_adj(X509_get_notBefore(cert), -24 * 3600);
    X509_gmtime_adj(X509_get_notAfter(cert), 10L * 365 * 24 * 3600);
    X509_set_pubkey(cert, key);
    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char *>("SailAuto development"), -1, -1, 0);
    X509_set_issuer_name(cert, name);
    if (!X509_sign(cert, key, EVP_sha256())) {
        X509_free(cert);
        EVP_PKEY_free(key);
        return false;
    }
    *outKey = key;
    *outCert = cert;
    return true;
}

} // namespace

Cryptor::Cryptor() = default;

Cryptor::~Cryptor()
{
    if (m_ssl) SSL_free(m_ssl); // also frees the BIOs
    if (m_ctx) SSL_CTX_free(m_ctx);
}

bool Cryptor::init(const QString &certPath, const QString &keyPath, QString *error)
{
#if OPENSSL_VERSION_NUMBER < 0x10100000L
    SSL_library_init();
    SSL_load_error_strings();
    m_ctx = SSL_CTX_new(SSLv23_server_method());
#else
    m_ctx = SSL_CTX_new(TLS_server_method());
#endif
    if (!m_ctx) {
        if (error) *error = QStringLiteral("SSL_CTX_new: ") + opensslErrors();
        return false;
    }
    // aasdk/OpenAuto still negotiates old cipher suites; keep the server lenient.
    SSL_CTX_set_cipher_list(m_ctx, "DEFAULT:@SECLEVEL=0");
    // Android Auto uses TLS 1.2. With TLS 1.3 the server sends session
    // tickets after the client already finished; aasdk then answers with a
    // second AUTH_COMPLETE. Stay on 1.2 and send no tickets.
    SSL_CTX_set_options(m_ctx, SSL_OP_NO_TICKET);
#if OPENSSL_VERSION_NUMBER >= 0x10100000L
    SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
#endif

    EVP_PKEY *key = nullptr;
    X509 *cert = nullptr;
    if (!certPath.isEmpty() && QFile::exists(certPath) && QFile::exists(keyPath)) {
        QFile cf(certPath), kf(keyPath);
        cf.open(QIODevice::ReadOnly);
        kf.open(QIODevice::ReadOnly);
        QByteArray cpem = cf.readAll(), kpem = kf.readAll();
        BIO *cb = BIO_new_mem_buf(cpem.constData(), cpem.size());
        BIO *kb = BIO_new_mem_buf(kpem.constData(), kpem.size());
        cert = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr);
        key = PEM_read_bio_PrivateKey(kb, nullptr, nullptr, nullptr);
        // Optional chain certificates following the leaf in the same file.
        if (cert) {
            X509 *extra;
            while ((extra = PEM_read_bio_X509(cb, nullptr, nullptr, nullptr)) != nullptr)
                SSL_CTX_add_extra_chain_cert(m_ctx, extra); // ctx takes ownership
            ERR_clear_error();
        }
        BIO_free(cb);
        BIO_free(kb);
        if (!cert || !key) {
            if (error) *error = QStringLiteral("Zertifikat/Schlüssel nicht lesbar: ") + opensslErrors();
            return false;
        }
        m_certInfo = QStringLiteral("eigenes Zertifikat (%1)").arg(certPath);
    } else {
        if (!generateSelfSigned(&key, &cert)) {
            if (error) *error = QStringLiteral("Schlüsselerzeugung: ") + opensslErrors();
            return false;
        }
        m_certInfo = QStringLiteral("selbstsigniertes Entwicklerzertifikat");
    }

    bool ok = SSL_CTX_use_certificate(m_ctx, cert) == 1 && SSL_CTX_use_PrivateKey(m_ctx, key) == 1
              && SSL_CTX_check_private_key(m_ctx) == 1;
    X509_free(cert);
    EVP_PKEY_free(key);
    if (!ok) {
        if (error) *error = QStringLiteral("Zertifikat passt nicht zum Schlüssel: ") + opensslErrors();
        return false;
    }

    m_ssl = SSL_new(m_ctx);
    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    SSL_set_bio(m_ssl, m_rbio, m_wbio);
    SSL_set_accept_state(m_ssl);
    return true;
}

QByteArray Cryptor::drainWriteBio()
{
    QByteArray out;
    char buf[4096];
    int n;
    while ((n = BIO_read(m_wbio, buf, sizeof(buf))) > 0)
        out.append(buf, n);
    return out;
}

QByteArray Cryptor::handshake(const QByteArray &in)
{
    if (!in.isEmpty())
        BIO_write(m_rbio, in.constData(), in.size());
    int r = SSL_do_handshake(m_ssl);
    if (r != 1) {
        int e = SSL_get_error(m_ssl, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE)
            qWarning("[aap] TLS handshake error %d: %s", e, qPrintable(opensslErrors()));
    }
    return drainWriteBio();
}

bool Cryptor::handshakeDone() const
{
    return m_ssl && SSL_is_init_finished(m_ssl);
}

QByteArray Cryptor::encrypt(const QByteArray &plain)
{
    int off = 0;
    while (off < plain.size()) {
        int n = SSL_write(m_ssl, plain.constData() + off, plain.size() - off);
        if (n <= 0) {
            qWarning("[aap] SSL_write failed: %s", qPrintable(opensslErrors()));
            break;
        }
        off += n;
    }
    return drainWriteBio();
}

QByteArray Cryptor::decrypt(const QByteArray &cipher, bool *ok)
{
    BIO_write(m_rbio, cipher.constData(), cipher.size());
    QByteArray out;
    char buf[16384];
    for (;;) {
        int n = SSL_read(m_ssl, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, n);
            continue;
        }
        int e = SSL_get_error(m_ssl, n);
        if (ok) *ok = (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_ZERO_RETURN);
        if (ok && !*ok) qWarning("[aap] SSL_read failed: %s", qPrintable(opensslErrors()));
        break;
    }
    return out;
}

QString Cryptor::lastError() const
{
    return opensslErrors();
}

} // namespace aap
