#pragma once

#include <stdint.h>

class BWC;
struct SpaSnapshot;

/**
 * Native SUPLA Cloud bridge for the Lay-Z-Spa controller.
 *
 * Wi-Fi and the local HTTP server remain owned by the main firmware.  The
 * bridge only adds the SUPLA SRPC/TLS client and cloud channels.  A bridge is
 * intended to be started at most once during a boot; changing its credentials
 * requires a restart, just like changing the main Wi-Fi configuration.
 */
class SuplaSpaBridge
{
    public:
        SuplaSpaBridge();
        ~SuplaSpaBridge();

        SuplaSpaBridge(const SuplaSpaBridge&) = delete;
        SuplaSpaBridge& operator=(const SuplaSpaBridge&) = delete;

        bool begin(BWC* spa,
                   const uint8_t guid[16],
                   const uint8_t authKey[16],
                   const char* server,
                   const char* email);
        void iterate();
        void sync(const SpaSnapshot& snapshot, bool force = false);
        void stop();
        bool isRunning() const;
        const char* status() const;

    private:
        struct Impl;
        Impl* impl_ = nullptr;
};
