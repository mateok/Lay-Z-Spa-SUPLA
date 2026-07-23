#pragma once

#include <stddef.h>
#include <stdint.h>

namespace Supla
{
class Client;
}

/**
 * Creates an ESP8266 SUPLA client whose DNS, TCP and TLS waits are bounded.
 *
 * The caller owns the returned client.  TLS certificate validation remains
 * controlled through Supla::Client::setCACert(), exactly like in the upstream
 * supla-device client.
 */
Supla::Client* makeBoundedSuplaClient();

/**
 * Resolves the user's SUPLA server through the official HTTPS autodiscovery
 * endpoint.  Every network wait uses the same bounded transport as SRPC.
 */
bool discoverSuplaServerBounded(const char* email,
                                const uint8_t guid[16],
                                const char* caCertificate,
                                char* server,
                                size_t serverSize);
