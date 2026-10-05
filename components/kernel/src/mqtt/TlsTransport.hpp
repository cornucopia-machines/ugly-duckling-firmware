#pragma once

#include <Log.hpp>

#include <esp_crt_bundle.h>
#include <esp_transport.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace std::chrono_literals;

namespace cornucopia::ugly_duckling::kernel::mqtt {

LOGGING_TAG(TLS, "tls")

/**
 * @brief TLS over another esp_transport, for connections that don't go through lwIP sockets.
 *
 * esp-mqtt's own TLS support (esp-tls) opens the socket itself, so it can't run over the
 * cellular modem's AT socket transport. This does the same with mbedTLS directly, on top of
 * whatever transport it's given: same server certificate and optional client certificate as the
 * WiFi path, verification required, and the hostname checked against the certificate. The OTA
 * download uses it too, verifying against the CA bundle instead of a pinned certificate.
 */
class TlsTransport {
public:
    struct Credentials {
        // PEM; empty strings for no client certificate
        std::string serverCert;
        std::string clientCert;
        std::string clientKey;
    };

    /**
     * @param tlsCredentials the server certificate to pin, and the client certificate if any;
     * std::nullopt verifies the server against the CA bundle built into the firmware instead,
     * without a client certificate, for servers we don't pin a certificate for (the OTA image host)
     */
    TlsTransport(esp_transport_handle_t parent, std::optional<Credentials> tlsCredentials)
        : parent(parent)
        , credentials(std::move(tlsCredentials))
        , handle(esp_transport_init()) {
        if (handle == nullptr) {
            throw std::runtime_error("could not create TLS transport");
        }
        initConfig();
        mbedtls_ssl_init(&ssl);
        esp_transport_set_context_data(handle, this);
        esp_transport_set_func(handle, &TlsTransport::connect, &TlsTransport::read, &TlsTransport::write,
            &TlsTransport::close, &TlsTransport::pollRead, &TlsTransport::pollWrite, &TlsTransport::destroy);
        esp_transport_set_default_port(handle, 8883);
    }

    // The handle goes to esp-mqtt, which keeps it for as long as the client lives: forever
    TlsTransport(const TlsTransport&) = delete;
    TlsTransport& operator=(const TlsTransport&) = delete;

    esp_transport_handle_t getHandle() const {
        return handle;
    }

private:
    void initConfig() {
        mbedtls_ssl_config_init(&config);
        mbedtls_x509_crt_init(&serverCert);
        mbedtls_x509_crt_init(&clientCert);
        mbedtls_pk_init(&clientKey);

        check("mbedtls_ssl_config_defaults", mbedtls_ssl_config_defaults(&config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT));

        mbedtls_ssl_conf_authmode(&config, MBEDTLS_SSL_VERIFY_REQUIRED);

        if (!credentials) {
            // Looks the issuer up in the bundle during the handshake
            if (esp_crt_bundle_attach(&config) != ESP_OK) {
                throw std::runtime_error("TLS setup failed: attaching the CA bundle");
            }
            return;
        }

        // PEM parsing wants the terminating NUL counted in the length
        check("parsing the server certificate", mbedtls_x509_crt_parse(&serverCert, asBytes(credentials->serverCert), credentials->serverCert.size() + 1));
        mbedtls_ssl_conf_ca_chain(&config, &serverCert, nullptr);

        if (!credentials->clientCert.empty() && !credentials->clientKey.empty()) {
            check("parsing the client certificate", mbedtls_x509_crt_parse(&clientCert, asBytes(credentials->clientCert), credentials->clientCert.size() + 1));
            check("parsing the client key", mbedtls_pk_parse_key(&clientKey, asBytes(credentials->clientKey), credentials->clientKey.size() + 1, nullptr, 0));
            check("mbedtls_ssl_conf_own_cert", mbedtls_ssl_conf_own_cert(&config, &clientCert, &clientKey));
        }
    }

    static TlsTransport& from(esp_transport_handle_t transport) {
        return *static_cast<TlsTransport*>(esp_transport_get_context_data(transport));
    }

    static int connect(esp_transport_handle_t transport, const char* host, int port, int timeoutMs) {
        return from(transport).connect(host, port, milliseconds(timeoutMs));
    }

    static int read(esp_transport_handle_t transport, char* buffer, int length, int timeoutMs) {
        return from(transport).read(reinterpret_cast<unsigned char*>(buffer), static_cast<size_t>(length), timeoutMs);
    }

    static int write(esp_transport_handle_t transport, const char* buffer, int length, int timeoutMs) {
        return from(transport).write(reinterpret_cast<const unsigned char*>(buffer), static_cast<size_t>(length), timeoutMs);
    }

    static int close(esp_transport_handle_t transport) {
        auto& tls = from(transport);
        mbedtls_ssl_free(&tls.ssl);
        mbedtls_ssl_init(&tls.ssl);
        return esp_transport_close(tls.parent);
    }

    static int pollRead(esp_transport_handle_t transport, int timeoutMs) {
        auto& tls = from(transport);
        // Data mbedTLS has already decrypted doesn't show up on the parent
        if (mbedtls_ssl_get_bytes_avail(&tls.ssl) > 0) {
            return 1;
        }
        return esp_transport_poll_read(tls.parent, timeoutMs);
    }

    static int pollWrite(esp_transport_handle_t transport, int timeoutMs) {
        return esp_transport_poll_write(from(transport).parent, timeoutMs);
    }

    static int destroy(esp_transport_handle_t /*transport*/) {
        return 0;
    }

    int connect(const char* host, int port, milliseconds timeout) {
        if (esp_transport_connect(parent, host, port, static_cast<int>(timeout.count())) < 0) {
            return -1;
        }

        // A fresh session for every connection
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_init(&ssl);
        int ret = mbedtls_ssl_setup(&ssl, &config);
        if (ret == 0) {
            ret = mbedtls_ssl_set_hostname(&ssl, host);
        }
        if (ret != 0) {
            logError("Setting up TLS", ret);
            esp_transport_close(parent);
            return -1;
        }
        mbedtls_ssl_set_bio(&ssl, this, &TlsTransport::sendToParent, &TlsTransport::receiveFromParent, nullptr);

        // The handshake takes several round trips, each of which can take seconds over NB-IoT
        auto deadline = steady_clock::now() + HANDSHAKE_TIMEOUT;
        auto startedAt = steady_clock::now();
        bytesSent = 0;
        bytesReceived = 0;
        receiveTimeoutMs = static_cast<int>(duration_cast<milliseconds>(HANDSHAKE_ROUND_TRIP_TIMEOUT).count());
        while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
            if ((ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) || steady_clock::now() > deadline) {
                logHandshakeFailure(host, ret);
                esp_transport_close(parent);
                return -1;
            }
        }
        // Worth knowing: the handshake is a large part of what connecting costs over NB-IoT
        LOGTI(TLS, "Handshake with %s done in %lld ms, %zu bytes sent, %zu received, %s", host,
            static_cast<long long>(duration_cast<milliseconds>(steady_clock::now() - startedAt).count()),
            bytesSent, bytesReceived, mbedtls_ssl_get_ciphersuite(&ssl));
        return 0;
    }

    int read(unsigned char* buffer, size_t length, int timeoutMs) {
        if (mbedtls_ssl_get_bytes_avail(&ssl) == 0) {
            int ready = esp_transport_poll_read(parent, timeoutMs);
            if (ready < 0) {
                return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
            }
            if (ready == 0) {
                return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
            }
        }
        // Give the rest of a record a moment to arrive once part of it has
        receiveTimeoutMs = std::max(timeoutMs, static_cast<int>(duration_cast<milliseconds>(RECORD_TIMEOUT).count()));
        int ret = mbedtls_ssl_read(&ssl, buffer, length);
        if (ret > 0) {
            return ret;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            return ERR_TCP_TRANSPORT_CONNECTION_CLOSED_BY_FIN;
        }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT;
        }
        logError("Reading", ret);
        return ERR_TCP_TRANSPORT_CONNECTION_FAILED;
    }

    int write(const unsigned char* buffer, size_t length, int /*timeoutMs*/) {
        size_t written = 0;
        while (written < length) {
            int ret = mbedtls_ssl_write(&ssl, buffer + written, length - written);
            if (ret > 0) {
                written += static_cast<size_t>(ret);
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                logError("Writing", ret);
                return -1;
            }
        }
        return static_cast<int>(written);
    }

    // mbedTLS's I/O callbacks (mbedtls_ssl_set_bio), translating to and from esp_transport's results
    static int sendToParent(void* context, const unsigned char* buffer, size_t length) {
        auto* tls = static_cast<TlsTransport*>(context);
        int ret = esp_transport_write(tls->parent, reinterpret_cast<const char*>(buffer), static_cast<int>(length), 0);
        if (ret < 0) {
            return MBEDTLS_ERR_NET_SEND_FAILED;
        }
        tls->bytesSent += static_cast<size_t>(ret);
        return ret;
    }

    static int receiveFromParent(void* context, unsigned char* buffer, size_t length) {
        auto* tls = static_cast<TlsTransport*>(context);
        int ret = esp_transport_read(tls->parent, reinterpret_cast<char*>(buffer), static_cast<int>(length), tls->receiveTimeoutMs);
        if (ret == ERR_TCP_TRANSPORT_CONNECTION_TIMEOUT) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        if (ret < 0) {
            return MBEDTLS_ERR_NET_RECV_FAILED;
        }
        tls->bytesReceived += static_cast<size_t>(ret);
        return ret;
    }

    void logHandshakeFailure(const char* host, int ret) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOGTE(TLS, "Handshake with %s timed out after %lld s", host,
                static_cast<long long>(duration_cast<seconds>(HANDSHAKE_TIMEOUT).count()));
            return;
        }
        logError("Handshake", ret);
        uint32_t flags = mbedtls_ssl_get_verify_result(&ssl);
        if (ret == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != 0) {
            char reason[256];
            mbedtls_x509_crt_verify_info(reason, sizeof(reason), "", flags);
            LOGTE(TLS, "Certificate of %s rejected: %s", host, reason);
        }
    }

    static void logError(const char* what, int ret) {
        char message[128];
        mbedtls_strerror(ret, message, sizeof(message));
        LOGTE(TLS, "%s failed: -0x%04x %s", what, static_cast<unsigned>(-ret), message);
    }

    static void check(const char* what, int ret) {
        if (ret != 0) {
            logError(what, ret);
            throw std::runtime_error(std::string("TLS setup failed: ") + what);
        }
    }

    static const unsigned char* asBytes(const std::string& text) {
        return reinterpret_cast<const unsigned char*>(text.c_str());
    }

    static constexpr milliseconds HANDSHAKE_TIMEOUT = 90s;
    static constexpr milliseconds HANDSHAKE_ROUND_TRIP_TIMEOUT = 10s;
    static constexpr milliseconds RECORD_TIMEOUT = 5s;

    esp_transport_handle_t parent;
    const std::optional<Credentials> credentials;
    esp_transport_handle_t handle;

    mbedtls_ssl_config config {};
    mbedtls_x509_crt serverCert {};
    mbedtls_x509_crt clientCert {};
    mbedtls_pk_context clientKey {};
    mbedtls_ssl_context ssl {};

    // How long a read from the parent may wait, set before each call into mbedTLS
    int receiveTimeoutMs = 0;

    // Bytes through the parent since the handshake started, records and all: what TLS costs
    // to set up over a metered link
    size_t bytesSent = 0;
    size_t bytesReceived = 0;
};

}    // namespace cornucopia::ugly_duckling::kernel::mqtt
