#include "BoundedSuplaClient.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <memory>
#include <new>
#include <string.h>
#include <time.h>

#include <supla/network/client.h>

namespace
{
// WiFiClientSecure in ESP8266 Arduino core 3.1.2 ignores the Stream timeout
// while resolving a hostname and instead uses DNSDefaultTimeoutMs (10 s).
// A bounded lookup immediately before connect() populates lwIP's DNS cache;
// the client's second lookup is then synchronous while preserving hostname
// verification and SNI during the TLS handshake.
constexpr uint16_t DNS_TIMEOUT_MS = 250;
// Match supla-device's intended ESP client timeout for the CPU-intensive
// initial TLS handshake.  Normal SRPC I/O uses the shorter bound below.
constexpr uint16_t TLS_HANDSHAKE_TIMEOUT_MS = 3000;
constexpr uint16_t TRANSPORT_TIMEOUT_MS = 1200;
constexpr uint16_t AUTODISCOVERY_REQUEST_TIMEOUT_MS = 3000;
// SUPLA's reference autodiscovery implementation allows up to three seconds
// for the complete HTTP reply.  The reply is usually immediate, but the
// ESP8266 can receive it noticeably later while the radio services other
// traffic after the CPU-intensive TLS handshake.
constexpr uint16_t AUTODISCOVERY_RESPONSE_TIMEOUT_MS = 3000;
constexpr time_t MINIMUM_CERTIFICATE_TIME = 1609459200;  // 2021-01-01 UTC.

constexpr char AUTODISCOVERY_HOST[] = "iot.autodiscover.supla.org";

class BoundedSecureClient final : public BearSSL::WiFiClientSecure
{
    public:
        void setOperationTimeout(uint16_t timeoutMs)
        {
            // WiFiClientSecure is a wrapper around an owned
            // WiFiClientSecureCtx in ESP8266 core 3.1.2.  Stream::setTimeout()
            // is not virtual, so setting it only on the wrapper does not
            // affect TCP or the BearSSL handshake.  Apply it to both objects.
            setTimeout(timeoutMs);
            if(_owned != nullptr) _owned->setTimeout(timeoutMs);
        }
};

class BoundedEspClient final : public Supla::Client
{
    public:
        BoundedEspClient()
        {
            // Reuse a negotiated TLS session when the server supports it.
            // This reduces both reconnection latency and CPU load.
            secureClient_.setSession(&tlsSession_);
        }

        ~BoundedEspClient() override
        {
            if(client_ != nullptr) client_->stop();
        }

        int available() override { return client_ != nullptr ? client_->available() : 0; }
        void stop() override
        {
            if(client_ != nullptr) client_->stop();
        }
        uint8_t connected() override
        {
            return client_ != nullptr && client_->connected();
        }
        void setTimeoutMs(uint16_t timeoutMs) override
        {
            // Never allow a library/user setting to restore multi-second
            // stalls in the SPA controller's single cooperative loop.
            transportTimeoutMs_ = constrain(timeoutMs, 100U, TRANSPORT_TIMEOUT_MS);
        }

    protected:
        int connectImp(const char* host, uint16_t port) override
        {
            if(host == nullptr || host[0] == '\0') return 0;
            stop();

            IPAddress resolvedAddress;
            if(!WiFi.hostByName(host, resolvedAddress, DNS_TIMEOUT_MS)) return 0;

            if(sslEnabled)
            {
                if(rootCACert != nullptr && time(nullptr) < MINIMUM_CERTIFICATE_TIME)
                    return 0;

                secureClient_.setOperationTimeout(TLS_HANDSHAKE_TIMEOUT_MS);
                // The public SUPLA endpoint currently sends a certificate
                // flight larger than 1 KiB and does not guarantee MFLN.
                // Four KiB is sufficient for that handshake while remaining
                // far below BearSSL's 16 KiB default receive allocation.
                secureClient_.setBufferSizes(4096, 512);

                if(rootCACert != nullptr)
                {
                    trustAnchor_.reset(new(std::nothrow) BearSSL::X509List(rootCACert));
                    if(!trustAnchor_) return 0;
                    secureClient_.setTrustAnchors(trustAnchor_.get());
                }
                else
                {
                    // This is the upstream fallback.  Production SRPC and
                    // autodiscovery always provide the official SUPLA CA.
                    secureClient_.setInsecure();
                }

                client_ = &secureClient_;
                // Do not connect to resolvedAddress directly: BearSSL needs
                // the hostname for SNI and certificate hostname validation.
                // The immediately preceding lookup is now in lwIP's cache.
                const int connected = secureClient_.connect(host, port);
                if(!connected)
                {
                    char tlsError[96] = {};
                    const int errorCode =
                        secureClient_.getLastSSLError(tlsError, sizeof(tlsError));
                    Serial.printf("SUPLA TLS > connection failed (%d): %s\n",
                                  errorCode,
                                  tlsError[0] != '\0' ? tlsError : "unknown error");
                }
                secureClient_.setOperationTimeout(transportTimeoutMs_);
                return connected;
            }

            plainClient_.setTimeout(transportTimeoutMs_);
            client_ = &plainClient_;
            return plainClient_.connect(resolvedAddress, port);
        }

        int readImp(uint8_t* buffer, size_t count) override
        {
            if(client_ == nullptr || client_->available() <= 0) return -1;
            return client_->read(buffer, count);
        }

        size_t writeImp(const uint8_t* buffer, size_t count) override
        {
            return client_ != nullptr ? client_->write(buffer, count) : 0;
        }

    private:
        WiFiClient plainClient_;
        BearSSL::Session tlsSession_;
        // WiFiClientSecure stores a non-owning trust-anchor pointer.  Keep the
        // parsed CA alive for the complete connection, not only the handshake
        // call stack.
        std::unique_ptr<BearSSL::X509List> trustAnchor_;
        BoundedSecureClient secureClient_;
        WiFiClient* client_ = nullptr;
        uint16_t transportTimeoutMs_ = TRANSPORT_TIMEOUT_MS;
};

bool isValidServerName(const char* server)
{
    if(server == nullptr || server[0] == '\0') return false;

    bool labelHasCharacter = false;
    bool previousWasHyphen = false;
    for(const char* cursor = server; *cursor != '\0'; ++cursor)
    {
        const char character = *cursor;
        if(character == '.')
        {
            if(!labelHasCharacter || previousWasHyphen) return false;
            labelHasCharacter = false;
            previousWasHyphen = false;
            continue;
        }
        const bool alphaNumeric = (character >= 'a' && character <= 'z') ||
                                  (character >= 'A' && character <= 'Z') ||
                                  (character >= '0' && character <= '9');
        if(!alphaNumeric && character != '-') return false;
        if(!labelHasCharacter && character == '-') return false;
        labelHasCharacter = true;
        previousWasHyphen = character == '-';
    }
    return labelHasCharacter && !previousWasHyphen;
}
}  // namespace

Supla::Client* makeBoundedSuplaClient()
{
    return new(std::nothrow) BoundedEspClient();
}

bool discoverSuplaServerBounded(const char* email,
                                const uint8_t guid[16],
                                const char* caCertificate,
                                char* server,
                                size_t serverSize)
{
    if(email == nullptr || email[0] == '\0' || guid == nullptr || caCertificate == nullptr ||
       server == nullptr || serverSize < 2)
        return false;

    server[0] = '\0';
    std::unique_ptr<Supla::Client> client(makeBoundedSuplaClient());
    if(!client) return false;

    client->setSSLEnabled(true);
    client->setCACert(caCertificate);
    client->setTimeoutMs(TRANSPORT_TIMEOUT_MS);
    if(client->connect(AUTODISCOVERY_HOST, 443) != 1) return false;

    char guidHex[33] = {};
    static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
    for(size_t i = 0; i < 16; ++i)
    {
        guidHex[i * 2] = HEX_DIGITS[guid[i] >> 4];
        guidHex[i * 2 + 1] = HEX_DIGITS[guid[i] & 0x0F];
    }

    // Send one complete HTTP request and explicitly handle a partial socket
    // write.  Several small TLS writes can otherwise leave the final header
    // fragment buffered on a busy ESP8266, so the HTTP server keeps waiting
    // and never starts its response.
    char request[512] = {};
    const int requestLength = snprintf(request,
                                       sizeof(request),
                                       "GET /users/%s HTTP/1.1\r\n"
                                       "Host: %s\r\n"
                                       "User-Agent: Lay-Z-Spa-SUPLA\r\n"
                                       "Accept: application/json\r\n"
                                       "X-GUID: %s\r\n"
                                       "Connection: close\r\n\r\n",
                                       email,
                                       AUTODISCOVERY_HOST,
                                       guidHex);
    if(requestLength <= 0 || static_cast<size_t>(requestLength) >= sizeof(request))
        return false;

    size_t requestSent = 0;
    const uint32_t requestDeadline = millis() + AUTODISCOVERY_REQUEST_TIMEOUT_MS;
    while(requestSent < static_cast<size_t>(requestLength) && client->connected() &&
          static_cast<int32_t>(millis() - requestDeadline) < 0)
    {
        const size_t written = client->write(
            reinterpret_cast<const uint8_t*>(request) + requestSent,
            static_cast<size_t>(requestLength) - requestSent);
        if(written > 0)
            requestSent += written;
        else
            delay(1);
    }
    memset(request, 0, sizeof(request));
    if(requestSent != static_cast<size_t>(requestLength))
    {
        Serial.printf("SUPLA AD > request incomplete (%u/%u bytes)\n",
                      static_cast<unsigned>(requestSent),
                      static_cast<unsigned>(requestLength));
        client->stop();
        return false;
    }

    char response[512] = {};
    size_t responseSize = 0;
    const uint32_t deadline = millis() + AUTODISCOVERY_RESPONSE_TIMEOUT_MS;
    while(responseSize < sizeof(response) - 1 &&
          static_cast<int32_t>(millis() - deadline) < 0)
    {
        const int bytesRead = client->read(
            reinterpret_cast<uint8_t*>(response) + responseSize,
            sizeof(response) - 1 - responseSize);
        if(bytesRead > 0)
        {
            responseSize += static_cast<size_t>(bytesRead);
            response[responseSize] = '\0';
        }
        else
        {
            // HTTP/1.1 Connection: close lets the peer close TCP as soon as
            // the reply has been sent.  Try the read before this check so
            // bytes already buffered by BearSSL are not discarded merely
            // because the socket is no longer marked connected.
            if(!client->connected()) break;
            delay(1);
        }
    }
    client->stop();

    if(strncmp(response, "HTTP/1.1 200", 12) != 0)
    {
        Serial.printf("SUPLA AD > invalid response (%u bytes, status %.12s)\n",
                      static_cast<unsigned>(responseSize),
                      responseSize >= 12 ? response : "no response");
        return false;
    }
    constexpr char SERVER_KEY[] = "\"server\":\"";
    const char* value = strstr(response, SERVER_KEY);
    if(value == nullptr)
    {
        Serial.printf("SUPLA AD > server missing in response (%u bytes)\n",
                      static_cast<unsigned>(responseSize));
        return false;
    }
    value += sizeof(SERVER_KEY) - 1;
    const char* end = strchr(value, '"');
    if(end == nullptr) return false;

    const size_t length = static_cast<size_t>(end - value);
    if(length == 0 || length >= serverSize) return false;
    memcpy(server, value, length);
    server[length] = '\0';
    if(!isValidServerName(server))
    {
        server[0] = '\0';
        return false;
    }
    return true;
}
