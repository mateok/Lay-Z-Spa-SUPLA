#include "SuplaSpaBridge.h"

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <LittleFS.h>
#include <math.h>
#include <new>
#include <string.h>

#include <SuplaDevice.h>
#include <supla/channels/channel.h>
#include <supla/clock/clock.h>
#include <supla/control/hvac_base.h>
#include <supla/control/remote_output_interface.h>
#include <supla/control/virtual_relay.h>
#include <supla/network/network.h>
#include <supla/protocol/supla_srpc.h>
#include <supla/sensor/general_purpose_measurement.h>
#include <supla/sensor/virtual_thermometer.h>
#include <supla/storage/key_value.h>
#include <supla/storage/storage.h>
#include <supla/tools.h>

#include "bwc.h"
#include "BoundedSuplaClient.h"

namespace
{
constexpr uint32_t HVAC_PENDING_TIMEOUT_MS = 15000;
constexpr uint32_t SERVER_DISCOVERY_RETRY_MS = 60000;
constexpr size_t CONFIG_BUFFER_SIZE = 4096;
constexpr size_t FILE_BLOB_THRESHOLD = 32;

constexpr char CONFIG_FILE[] = "/supla-dev.cfg";
constexpr char CONFIG_BACKUP_FILE[] = "/supla-dev.bak";
constexpr char CONFIG_TEMP_FILE[] = "/supla-dev.tmp";
constexpr char BLOB_DIRECTORY[] = "/supla";

constexpr uint32_t CONFIG_MAGIC = 0x434C5053;  // "SPLC", little endian.
constexpr uint32_t BLOB_MAGIC = 0x424C5053;    // "SPLB", little endian.
constexpr uint16_t STORAGE_VERSION = 1;

/**
 * SUPLA Cloud sends the user's local civil time, not a UTC timestamp.  The
 * stock Clock converts that value with mktime() and writes it to the ESP
 * system clock.  ESP8266 runs with an UTC timezone, so this turns local time
 * into a false UTC timestamp and the web browser applies the timezone offset
 * for a second time.
 *
 * Keep the NTP-managed system clock in UTC.  SUPLA's local civil time is held
 * separately for HVAC schedules and refreshed by the cloud every five
 * minutes, which also makes DST and arbitrary account timezones work without
 * a hard-coded offset or an IANA timezone database on the ESP.
 */
class NtpPreservingSuplaClock final : public Supla::Clock
{
    public:
        void parseLocaltimeFromServer(TSDC_UserLocalTimeResult* result) override
        {
            if(result == nullptr || result->year < 1970 || result->month < 1 ||
               result->month > 12 || result->day < 1 || result->day > 31 ||
               result->hour > 23 || result->min > 59 || result->sec > 59)
                return;

            const int64_t days = daysFromCivil(result->year, result->month, result->day);
            localtime = static_cast<time_t>(days * 86400LL +
                                            result->hour * 3600LL +
                                            result->min * 60LL + result->sec);
            lastMillis = millis();
            isClockReady = true;
        }

        int getYear() override { return localParts().tm_year + 1900; }
        int getMonth() override { return localParts().tm_mon + 1; }
        int getDay() override { return localParts().tm_mday; }
        int getDayOfWeek() override { return localParts().tm_wday + 1; }
        Supla::DayOfWeek getHvacDayOfWeek() override
        {
            return static_cast<Supla::DayOfWeek>(getDayOfWeek() - 1);
        }
        int getHour() override { return localParts().tm_hour; }
        int getQuarter() override { return getMin() / 15; }
        int getMin() override { return localParts().tm_min; }
        int getSec() override { return localParts().tm_sec; }

        // Absolute timers and all SPA statistics use the NTP UTC timestamp.
        time_t getTimeStamp() override { return time(nullptr); }

    private:
        static int64_t daysFromCivil(int year, unsigned month, unsigned day)
        {
            year -= month <= 2;
            const int era = (year >= 0 ? year : year - 399) / 400;
            const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
            const unsigned dayOfYear =
                (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
            const unsigned dayOfEra =
                yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
            return era * 146097LL + static_cast<int64_t>(dayOfEra) - 719468LL;
        }

        tm localParts() const
        {
            tm parts = {};
            const time_t value = localtime;
            gmtime_r(&value, &parts);
            return parts;
        }
};

enum ChannelNumber : uint8_t
{
    WATER_TEMPERATURE = 0,
    SPA_THERMOSTAT = 1,
    SPA_POWER = 2,
    FILTER_PUMP = 3,
    AIRJET = 4,
    HYDROJET = 5,
    VIRTUAL_TEMPERATURE = 6,
    AMBIENT_TEMPERATURE = 7,
    FIRST_MEASUREMENT = 8,
    RESET_CHLORINE = 21,
    RESET_FILTER_REPLACE = 22,
    RESET_FILTER_CLEAN = 23,
    RESET_FILTER_RINSE = 24
};

enum MeasurementIndex : uint8_t
{
    CHLORINE_AGE,
    FILTER_REPLACE_AGE,
    FILTER_CLEAN_AGE,
    FILTER_RINSE_AGE,
    PUMP_RUNTIME,
    HEATING_RUNTIME,
    BUBBLES_RUNTIME,
    JETS_RUNTIME,
    ENERGY_TODAY,
    ENERGY_TOTAL,
    CURRENT_POWER,
    TIME_TO_READY,
    SPA_ERROR,
    MEASUREMENT_COUNT
};

#pragma pack(push, 1)
struct ConfigFileHeader
{
    uint32_t magic = CONFIG_MAGIC;
    uint16_t version = STORAGE_VERSION;
    uint16_t reserved = 0;
    uint32_t payloadSize = 0;
    uint32_t payloadCrc = 0;
};

struct BlobFileHeader
{
    uint32_t magic = BLOB_MAGIC;
    uint16_t version = STORAGE_VERSION;
    uint16_t reserved = 0;
    uint32_t payloadSize = 0;
    uint32_t payloadCrc = 0;
    char key[SUPLA_STORAGE_KEY_SIZE + 1] = {};
};
#pragma pack(pop)

uint32_t crc32(const uint8_t* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFUL;
    for(size_t i = 0; i < size; ++i)
    {
        crc ^= data[i];
        for(uint8_t bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
    return ~crc;
}

bool replaceFileAtomically(const char* temporary,
                           const char* destination,
                           const char* backup)
{
    LittleFS.remove(backup);
    const bool hadDestination = LittleFS.exists(destination);
    if(hadDestination && !LittleFS.rename(destination, backup))
    {
        LittleFS.remove(temporary);
        return false;
    }

    if(LittleFS.rename(temporary, destination)) return true;

    if(hadDestination) LittleFS.rename(backup, destination);
    LittleFS.remove(temporary);
    return false;
}

bool deadlineReached(uint32_t now, uint32_t deadline)
{
    return static_cast<int32_t>(now - deadline) >= 0;
}

bool bufferHasNonZeroByte(const uint8_t* buffer, size_t size)
{
    if(buffer == nullptr) return false;
    for(size_t i = 0; i < size; ++i)
        if(buffer[i] != 0) return true;
    return false;
}

/**
 * SUPLA configuration storage which deliberately does not own LittleFS.
 *
 * Small key/value entries live in one checksummed file.  Larger channel
 * configuration blobs (notably HVAC schedules) are separate, so they don't
 * permanently consume scarce ESP8266 heap.  LittleFS is mounted by the main
 * firmware and is never ended or formatted here.
 */
class SharedLittleFsConfig final : public Supla::KeyValue
{
    public:
        bool init() override
        {
            if(first != nullptr)
            {
                initResult = false;
                return false;
            }

            const bool primaryExists = LittleFS.exists(CONFIG_FILE);
            const bool backupExists = LittleFS.exists(CONFIG_BACKUP_FILE);
            if(loadConfigFile(CONFIG_FILE) || loadConfigFile(CONFIG_BACKUP_FILE))
            {
                initResult = true;
                return true;
            }

            // A missing or damaged SUPLA-only file must not prevent the SPA
            // controller from booting.  Credentials supplied by the main
            // firmware will repopulate this empty store before begin().
            if(primaryExists || backupExists)
            {
                removeAllMemory();
                LittleFS.remove(CONFIG_FILE);
                LittleFS.remove(CONFIG_BACKUP_FILE);
            }
            initResult = true;
            return true;
        }

        void commit() override
        {
            uint8_t* payload = new(std::nothrow) uint8_t[CONFIG_BUFFER_SIZE];
            if(payload == nullptr) return;

            size_t payloadSize = 0;
            for(auto* element = first; element != nullptr; element = element->getNext())
            {
                const size_t bytesWritten =
                    element->serialize(payload + payloadSize, CONFIG_BUFFER_SIZE - payloadSize);
                if(bytesWritten == 0)
                {
                    // Keep the last known-good file intact.  serializeToMemory()
                    // would merely log this condition and return a partial payload.
                    delete[] payload;
                    return;
                }
                payloadSize += bytesWritten;
            }

            ConfigFileHeader header;
            header.payloadSize = payloadSize;
            header.payloadCrc = crc32(payload, payloadSize);

            File file = LittleFS.open(CONFIG_TEMP_FILE, "w");
            if(!file)
            {
                delete[] payload;
                return;
            }

            const bool written =
                file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                file.write(payload, payloadSize) == payloadSize;
            file.flush();
            file.close();
            delete[] payload;

            if(!written)
            {
                LittleFS.remove(CONFIG_TEMP_FILE);
                return;
            }
            replaceFileAtomically(CONFIG_TEMP_FILE, CONFIG_FILE, CONFIG_BACKUP_FILE);
        }

        void removeAll() override
        {
            removeAllMemory();
            LittleFS.remove(CONFIG_FILE);
            LittleFS.remove(CONFIG_BACKUP_FILE);
            LittleFS.remove(CONFIG_TEMP_FILE);

            File directory = LittleFS.open(BLOB_DIRECTORY, "r");
            if(directory)
            {
                File file = directory.openNextFile();
                while(file)
                {
                    const String name = file.name();
                    const String path = name.startsWith("/") ? name
                                                             : String(BLOB_DIRECTORY) + "/" + name;
                    file.close();
                    LittleFS.remove(path);
                    file = directory.openNextFile();
                }
                directory.close();
            }
        }

        bool setBlob(const char* key, const char* value, size_t blobSize) override
        {
            if(key == nullptr || value == nullptr) return false;
            if(blobSize < FILE_BLOB_THRESHOLD)
            {
                removeBlobFiles(key);
                return Supla::KeyValue::setBlob(key, value, blobSize);
            }

            Supla::KeyValue::eraseKey(key);
            LittleFS.mkdir(BLOB_DIRECTORY);

            char path[48] = {};
            char temporary[48] = {};
            char backup[48] = {};
            makeBlobPaths(key, path, temporary, backup, sizeof(path));

            BlobFileHeader header;
            header.payloadSize = blobSize;
            header.payloadCrc = crc32(reinterpret_cast<const uint8_t*>(value), blobSize);
            strncpy(header.key, key, sizeof(header.key) - 1);

            File file = LittleFS.open(temporary, "w");
            if(!file) return false;
            const bool written =
                file.write(reinterpret_cast<const uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                file.write(reinterpret_cast<const uint8_t*>(value), blobSize) == blobSize;
            file.flush();
            file.close();

            if(!written)
            {
                LittleFS.remove(temporary);
                return false;
            }
            return replaceFileAtomically(temporary, path, backup);
        }

        bool getBlob(const char* key, char* value, size_t blobSize) override
        {
            if(key == nullptr || value == nullptr) return false;
            if(blobSize < FILE_BLOB_THRESHOLD)
                return Supla::KeyValue::getBlob(key, value, blobSize);

            char path[48] = {};
            char temporary[48] = {};
            char backup[48] = {};
            makeBlobPaths(key, path, temporary, backup, sizeof(path));
            return readBlobFile(path, key, value, blobSize) ||
                   readBlobFile(backup, key, value, blobSize);
        }

        bool eraseKey(const char* key) override
        {
            const bool memoryRemoved = Supla::KeyValue::eraseKey(key);
            const bool fileRemoved = removeBlobFiles(key);
            return memoryRemoved || fileRemoved;
        }

        bool isConfigModeSupported() override
        {
            // The main Lay-Z-Spa web UI is the sole provisioning interface.
            return false;
        }

        bool isMinimalConfigReady(bool showLogs = true) override
        {
            (void)showLogs;
            char email[SUPLA_EMAIL_MAXSIZE] = {};
            char guid[SUPLA_GUID_SIZE] = {};
            char authKey[SUPLA_AUTHKEY_SIZE] = {};
            return getEmail(email) && email[0] != '\0' &&
                   getGUID(guid) &&
                   getAuthKey(authKey) &&
                   bufferHasNonZeroByte(reinterpret_cast<uint8_t*>(guid), sizeof(guid)) &&
                   bufferHasNonZeroByte(reinterpret_cast<uint8_t*>(authKey), sizeof(authKey));
        }

    protected:
        int getBlobSize(const char* key) override
        {
            const int memorySize = Supla::KeyValue::getBlobSize(key);
            if(memorySize > 0) return memorySize;

            char path[48] = {};
            char temporary[48] = {};
            char backup[48] = {};
            makeBlobPaths(key, path, temporary, backup, sizeof(path));
            const int primarySize = readBlobSize(path, key);
            return primarySize >= 0 ? primarySize : readBlobSize(backup, key);
        }

    private:
        bool loadConfigFile(const char* path)
        {
            if(!LittleFS.exists(path)) return false;
            File file = LittleFS.open(path, "r");
            if(!file || file.size() < sizeof(ConfigFileHeader))
            {
                if(file) file.close();
                return false;
            }

            ConfigFileHeader header;
            if(file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
               header.magic != CONFIG_MAGIC ||
               header.version != STORAGE_VERSION ||
               header.payloadSize > CONFIG_BUFFER_SIZE ||
               file.size() != sizeof(header) + header.payloadSize)
            {
                file.close();
                return false;
            }

            if(header.payloadSize == 0)
            {
                file.close();
                return true;
            }

            uint8_t* payload = new(std::nothrow) uint8_t[header.payloadSize];
            if(payload == nullptr)
            {
                file.close();
                return false;
            }
            const bool readOk = file.read(payload, header.payloadSize) ==
                                static_cast<int>(header.payloadSize);
            file.close();
            if(!readOk || crc32(payload, header.payloadSize) != header.payloadCrc)
            {
                delete[] payload;
                return false;
            }

            removeAllMemory();
            const bool loaded = initFromMemory(payload, header.payloadSize);
            delete[] payload;
            if(!loaded) removeAllMemory();
            return loaded;
        }

        static void makeBlobPaths(const char* key,
                                  char* path,
                                  char* temporary,
                                  char* backup,
                                  size_t size)
        {
            char safeKey[SUPLA_STORAGE_KEY_SIZE + 1] = {};
            for(size_t i = 0; key != nullptr && key[i] != '\0' && i < sizeof(safeKey) - 1; ++i)
            {
                const char c = key[i];
                safeKey[i] = ((c >= 'a' && c <= 'z') ||
                              (c >= 'A' && c <= 'Z') ||
                              (c >= '0' && c <= '9') || c == '_' || c == '-') ? c : '_';
            }
            snprintf(path, size, "%s/%s.bin", BLOB_DIRECTORY, safeKey);
            snprintf(temporary, size, "%s/%s.tmp", BLOB_DIRECTORY, safeKey);
            snprintf(backup, size, "%s/%s.bak", BLOB_DIRECTORY, safeKey);
        }

        static bool readBlobFile(const char* path,
                                 const char* key,
                                 char* value,
                                 size_t expectedSize)
        {
            File file = LittleFS.open(path, "r");
            if(!file || file.size() < sizeof(BlobFileHeader))
            {
                if(file) file.close();
                return false;
            }

            BlobFileHeader header;
            if(file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) != sizeof(header) ||
               header.magic != BLOB_MAGIC ||
               header.version != STORAGE_VERSION ||
               header.payloadSize != expectedSize ||
               strncmp(header.key, key, sizeof(header.key)) != 0 ||
               file.size() != sizeof(header) + header.payloadSize)
            {
                file.close();
                return false;
            }

            const bool readOk = file.read(reinterpret_cast<uint8_t*>(value), expectedSize) ==
                                static_cast<int>(expectedSize);
            file.close();
            return readOk && crc32(reinterpret_cast<uint8_t*>(value), expectedSize) == header.payloadCrc;
        }

        static int readBlobSize(const char* path, const char* key)
        {
            File file = LittleFS.open(path, "r");
            if(!file || file.size() < sizeof(BlobFileHeader))
            {
                if(file) file.close();
                return -1;
            }
            BlobFileHeader header;
            const bool valid =
                file.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header) &&
                header.magic == BLOB_MAGIC &&
                header.version == STORAGE_VERSION &&
                strncmp(header.key, key, sizeof(header.key)) == 0 &&
                file.size() == sizeof(header) + header.payloadSize;
            file.close();
            return valid ? static_cast<int>(header.payloadSize) : -1;
        }

        static bool removeBlobFiles(const char* key)
        {
            if(key == nullptr) return false;
            char path[48] = {};
            char temporary[48] = {};
            char backup[48] = {};
            makeBlobPaths(key, path, temporary, backup, sizeof(path));
            bool removed = LittleFS.remove(path);
            removed = LittleFS.remove(temporary) || removed;
            removed = LittleFS.remove(backup) || removed;
            return removed;
        }
};

/** Network facade over the Wi-Fi connection managed by main.cpp. */
class ExistingWifiNetwork final : public Supla::Network
{
    public:
        ExistingWifiNetwork()
        {
            intfType = IntfType::WiFi;
        }

        void setup() override
        {
            // The first call is SUPLA's normal interface initialization.  The
            // main firmware already owns and has configured that connection,
            // so it must not be disrupted here.  Later calls are deliberate
            // network-layer restarts requested after lost/failed TLS sessions
            // (the ESP8266 client uses them to reclaim reconnect resources).
            // WiFi.reconnect() resets only the station side and therefore
            // preserves an active AP+STA provisioning window.
            if(!initialSetupComplete_)
            {
                initialSetupComplete_ = true;
                return;
            }
            WiFi.reconnect();
        }
        void disable() override {}
        bool isReady() override { return WiFi.status() == WL_CONNECTED; }
        bool isWifiConfigRequired() override { return false; }
        const char* getIntfName() const override { return "Lay-Z-Spa Wi-Fi"; }

        bool getMacAddr(uint8_t* output) override
        {
            if(output == nullptr) return false;
            WiFi.macAddress(output);
            return true;
        }

        uint32_t getIP() override
        {
            return WiFi.localIP();
        }

        Supla::Client* createClient() override
        {
            return makeBoundedSuplaClient();
        }

        void fillStateData(TDSC_ChannelState* state) override
        {
            if(state == nullptr) return;
            state->Fields |= SUPLA_CHANNELSTATE_FIELD_IPV4 |
                             SUPLA_CHANNELSTATE_FIELD_MAC |
                             SUPLA_CHANNELSTATE_FIELD_WIFIRSSI |
                             SUPLA_CHANNELSTATE_FIELD_WIFISIGNALSTRENGTH;
            state->IPv4 = WiFi.localIP();
            getMacAddr(state->MAC);
            const int rssi = WiFi.RSSI();
            state->WiFiRSSI = rssi;
            state->WiFiSignalStrength = Supla::rssiToSignalStrength(rssi);
        }

    private:
        bool initialSetupComplete_ = false;
};

class SpaMeasurement final : public Supla::Sensor::GeneralPurposeMeasurement
{
    public:
        SpaMeasurement(uint8_t channelNumber,
                       const char* caption,
                       const char* unit,
                       uint8_t precision)
            : Supla::Sensor::GeneralPurposeMeasurement(nullptr, false)
        {
            getChannel()->setChannelNumber(channelNumber);
            setInitialCaption(caption);
            setDefaultUnitAfterValue(unit);
            setDefaultValuePrecision(precision);
            setDefaultRefreshIntervalMs(10000);
        }

        double getValue() override { return value_; }

        void publish(double value, bool force)
        {
            const bool changed = (isnan(value_) != isnan(value)) ||
                                 (!isnan(value_) && !isnan(value) && fabs(value_ - value) > 0.0001);
            value_ = value;
            getChannel()->setStateOnline();
            if(force || changed) getChannel()->setNewValue(value_);
        }

    private:
        double value_ = NAN;
};

class SpaRelay final : public Supla::Control::VirtualRelay
{
    public:
        SpaRelay(BWC* spa,
                 Commands command,
                 uint8_t channelNumber,
                 const char* caption,
                 bool momentary = false)
            : Supla::Control::VirtualRelay(SUPLA_BIT_FUNC_POWERSWITCH),
              spa_(spa),
              command_(command),
              momentary_(momentary)
        {
            getChannel()->setChannelNumber(channelNumber);
            getChannel()->setDefaultFunction(SUPLA_CHANNELFNC_POWERSWITCH);
            getChannel()->unsetFlag(SUPLA_CHANNEL_FLAG_COUNTDOWN_TIMER_SUPPORTED);
            setInitialCaption(caption);
        }

        int32_t handleNewValueFromServer(TSD_SuplaChannelNewValue* newValue) override
        {
            if(newValue == nullptr) return 0;
            const bool requestedState = newValue->value[0] != 0;

            if(momentary_)
            {
                const bool queued = !requestedState ||
                                    (spa_ != nullptr && spa_->enqueueImmediate(command_, 0));
                publish(false, true);
                return queued ? 1 : 0;
            }

            if(!available_)
            {
                publish(false, true);
                return 0;
            }
            return spa_ != nullptr && spa_->enqueueImmediate(command_, requestedState ? 1 : 0) ? 1 : 0;
        }

        void setAvailable(bool available, bool force)
        {
            if(!available)
            {
                available_ = false;
                publish(false, force);
                getChannel()->setStateOnlineAndNotAvailable();
                return;
            }

            available_ = true;
            getChannel()->setStateOnline();
        }

        void publish(bool stateValue, bool force)
        {
            const bool changed = state != stateValue;
            state = stateValue;
            if(force || changed) getChannel()->setNewValue(state);
        }

    private:
        BWC* spa_ = nullptr;
        Commands command_ = SETPOWER;
        bool momentary_ = false;
        bool available_ = false;
};

class SpaHvac final : public Supla::Control::HvacBase
{
    public:
        SpaHvac(BWC* spa, Supla::Control::RemoteOutputInterface* output)
            : Supla::Control::HvacBase(output), spa_(spa), output_(output)
        {
            getChannel()->setChannelNumber(SPA_THERMOSTAT);
            getChannel()->setDefaultFunction(SUPLA_CHANNELFNC_HVAC_THERMOSTAT);
            setInitialCaption("Termostat SPA");
            setSubfunction(SUPLA_HVAC_SUBFUNCTION_HEAT);
            setTemperatureRoomMin(2000);
            setTemperatureRoomMax(4000);
            setDefaultTemperatureRoomMin(SUPLA_CHANNELFNC_HVAC_THERMOSTAT, 2000);
            setDefaultTemperatureRoomMax(SUPLA_CHANNELFNC_HVAC_THERMOSTAT, 4000);
            setButtonTemperatureStep(100);
            setTemperatureSetpointChangeSwitchesToManualMode(true);

            // The SPA controller has exactly one authoritative temperature
            // input.  Do not let an old cloud-side HVAC configuration replace
            // it with the HVAC channel itself ("no thermometer"), which makes
            // the mobile thermostat view wait indefinitely for a reading.
            parameterFlags.MainThermometerChannelNoReadonly = 1;
            parameterFlags.MainThermometerChannelNoHidden = 1;
            parameterFlags.AuxThermometerChannelNoReadonly = 1;
            parameterFlags.AuxThermometerChannelNoHidden = 1;
            parameterFlags.AuxThermometerTypeReadonly = 1;
            parameterFlags.AuxThermometerTypeHidden = 1;
            parameterFlags.BinarySensorChannelNoReadonly = 1;
            parameterFlags.BinarySensorChannelNoHidden = 1;
            parameterFlags.SubfunctionReadonly = 1;
            parameterFlags.SubfunctionHidden = 1;
            setMainThermometerChannelNo(WATER_TEMPERATURE);
        }

        void iterateAlways() override
        {
            const int previousMode = getMode();
            const int previousTargetC = roundedTargetC();

            // HvacBase executes the downloaded weekly program and expires
            // thermostat countdown timers locally.  RemoteOutputInterface
            // deliberately ignores HvacBase's relay output because the spa
            // has its own thermostat.  Bridge changes of the requested HVAC
            // mode/setpoint to the two commands understood by the spa.
            Supla::Control::HvacBase::iterateAlways();

            if(!available_) return;
            const int requestedMode = getMode();
            const int requestedTargetC = roundedTargetC();
            if(requestedMode != previousMode || requestedTargetC != previousTargetC)
                requestSpaState(false);
        }

        int32_t handleNewValueFromServer(TSD_SuplaChannelNewValue* newValue) override
        {
            if(!available_ || newValue == nullptr) return 0;
            const int32_t result = Supla::Control::HvacBase::handleNewValueFromServer(newValue);
            if(result == 0) return 0;

            return requestSpaState(true) ? result : 0;
        }

        void syncPhysical(const SpaSnapshot& snapshot, bool force)
        {
            // Repair configuration persisted by an earlier firmware version.
            // Once channels have been initialized, the setter validates that
            // channel 0 really is a thermometer and schedules the corrected
            // HVAC configuration for upload to SUPLA Cloud.
            if(getMainThermometerChannelNo() != WATER_TEMPERATURE)
                setMainThermometerChannelNo(WATER_TEMPERATURE);

            const uint32_t now = millis();
            if(targetPending_ &&
               (fabs(snapshot.targetTemperatureC - pendingTargetC_) < 0.51f ||
                deadlineReached(now, targetDeadline_)))
                targetPending_ = false;
            if(heatPending_ &&
               (snapshot.heaterEnabled == pendingHeatEnabled_ || deadlineReached(now, heatDeadline_)))
                heatPending_ = false;

            const int snapshotTargetC =
                constrain(static_cast<int>(lround(snapshot.targetTemperatureC)), 20, 40);
            const bool physicalChanged =
                !physicalStateKnown_ || physicalTargetC_ != snapshotTargetC ||
                physicalHeatEnabled_ != snapshot.heaterEnabled;

            available_ = snapshot.capabilities.hasHeater;
            physicalStateKnown_ = true;
            physicalTargetC_ = snapshotTargetC;
            physicalHeatEnabled_ = snapshot.heaterEnabled;
            if(!available_)
            {
                targetPending_ = false;
                heatPending_ = false;
                output_->setOutputValueFromRemote(0);
                getChannel()->setStateOnlineAndNotAvailable();
                return;
            }

            getChannel()->setStateOnline();
            output_->setOutputValueFromRemote(snapshot.heaterActive ? 1 : 0);

            const bool localAutomationActive =
                isWeeklyScheduleEnabled() || isCountdownEnabled();
            if(localAutomationActive)
            {
                // A weekly program and HVAC countdown are authoritative while
                // active.  Do not let the periodic physical-state snapshot
                // cancel them.  Assert the requested state once after boot or
                // after an actual change reported by the spa; normal scheduled
                // transitions are bridged immediately by iterateAlways().
                const bool targetMismatch = roundedTargetC() != physicalTargetC_;
                const bool heatMismatch =
                    (getMode() != SUPLA_HVAC_MODE_OFF) != physicalHeatEnabled_;
                if(physicalChanged &&
                   ((!targetPending_ && targetMismatch) ||
                    (!heatPending_ && heatMismatch)))
                    requestSpaState(false);
                return;
            }

            if(!targetPending_)
            {
                const int target = constrain(static_cast<int>(lround(snapshot.targetTemperatureC * 100.0f)),
                                             2000,
                                             4000);
                if(force || getTemperatureSetpointHeat() != target)
                    setTemperatureSetpointHeat(target);
            }
            if(!heatPending_)
            {
                const int mode = snapshot.heaterEnabled ? SUPLA_HVAC_MODE_HEAT : SUPLA_HVAC_MODE_OFF;
                if(force || getMode() != mode) setTargetMode(mode, false);
            }
        }

    private:
        int roundedTargetC()
        {
            const int target = getTemperatureSetpointHeat();
            if(target == INT16_MIN)
                return physicalStateKnown_ ? physicalTargetC_ : 20;
            return constrain(static_cast<int>(lround(target / 100.0)), 20, 40);
        }

        bool requestSpaState(bool restoreUiOnFailure)
        {
            const int targetC = roundedTargetC();
            bool targetQueued = true;
            if(getTemperatureSetpointHeat() != INT16_MIN)
            {
                targetQueued = spa_ != nullptr && spa_->enqueueImmediate(SETTARGET, targetC);
                if(targetQueued)
                {
                    // The stock SUPLA app exposes HVAC setpoints in 0.1 C
                    // increments, while Lay-Z-Spa controllers accept whole
                    // degrees only.  Publish the value accepted by the SPA
                    // immediately so the cloud UI snaps to the real target
                    // instead of displaying a fractional value temporarily.
                    setTemperatureSetpointHeat(targetC * 100);
                    targetPending_ = true;
                    pendingTargetC_ = targetC;
                    targetDeadline_ = millis() + HVAC_PENDING_TIMEOUT_MS;
                }
            }

            const bool heatEnabled = getMode() != SUPLA_HVAC_MODE_OFF;
            const bool heatQueued = spa_ != nullptr &&
                                    spa_->enqueueImmediate(SETHEATER, heatEnabled ? 1 : 0);
            if(heatQueued)
            {
                heatPending_ = true;
                pendingHeatEnabled_ = heatEnabled;
                heatDeadline_ = millis() + HVAC_PENDING_TIMEOUT_MS;
            }

            if(!targetQueued || !heatQueued)
            {
                targetPending_ = false;
                heatPending_ = false;
                if(physicalStateKnown_)
                {
                    // Coalescing these command types also cancels a partial
                    // request if only one of the two queue operations failed.
                    spa_->enqueueImmediate(SETTARGET, physicalTargetC_);
                    spa_->enqueueImmediate(SETHEATER, physicalHeatEnabled_ ? 1 : 0);
                    if(restoreUiOnFailure)
                    {
                        setTemperatureSetpointHeat(physicalTargetC_ * 100);
                        setTargetMode(physicalHeatEnabled_ ? SUPLA_HVAC_MODE_HEAT
                                                           : SUPLA_HVAC_MODE_OFF,
                                      false);
                    }
                }
                return false;
            }
            return true;
        }

        BWC* spa_ = nullptr;
        Supla::Control::RemoteOutputInterface* output_ = nullptr;
        uint32_t targetDeadline_ = 0;
        uint32_t heatDeadline_ = 0;
        int pendingTargetC_ = 20;
        bool pendingHeatEnabled_ = false;
        bool targetPending_ = false;
        bool heatPending_ = false;
        bool available_ = false;
        bool physicalStateKnown_ = false;
        bool physicalHeatEnabled_ = false;
        int physicalTargetC_ = 20;
};

struct MeasurementSpec
{
    const char* caption;
    const char* unit;
    uint8_t precision;
};

const MeasurementSpec MEASUREMENT_SPECS[MEASUREMENT_COUNT] = {
    {"Wiek chloru", "dni", 1},
    {"Wiek wkładu filtra", "dni", 1},
    {"Od czyszczenia filtra", "dni", 1},
    {"Od płukania filtra", "dni", 1},
    {"Czas pracy pompy", "h", 1},
    {"Czas grzania", "h", 1},
    {"Czas AirJet", "h", 1},
    {"Czas HydroJet", "h", 1},
    {"Energia dzisiaj (szac.)", "kWh", 2},
    {"Energia całkowita (szac.)", "kWh", 2},
    {"Aktualna moc", "W", 0},
    {"Czas do kąpieli", "h", 1},
    {"Kod błędu", "", 0}
};

const char* statusText(int status)
{
    switch(status)
    {
        case STATUS_INITIALIZED: return "initialized";
        case STATUS_SERVER_DISCONNECTED: return "server disconnected";
        case STATUS_ITERATE_FAIL: return "communication error";
        case STATUS_NETWORK_DISCONNECTED: return "network disconnected";
        case STATUS_REGISTER_IN_PROGRESS: return "registering";
        case STATUS_REGISTERED_AND_READY: return "connected";
        case STATUS_TEMPORARILY_UNAVAILABLE: return "temporarily unavailable";
        case STATUS_BAD_CREDENTIALS: return "bad credentials";
        case STATUS_DEVICE_IS_DISABLED: return "device disabled";
        case STATUS_REGISTRATION_DISABLED: return "registration disabled";
        case STATUS_MISSING_CREDENTIALS: return "missing credentials";
        case STATUS_INVALID_GUID: return "invalid GUID";
        case STATUS_INVALID_AUTHKEY: return "invalid auth key";
        case STATUS_UNKNOWN_SERVER_ADDRESS: return "discovering server";
        case STATUS_DEVICE_LIMIT_EXCEEDED: return "device limit exceeded";
        case STATUS_CHANNEL_LIMIT_EXCEEDED: return "channel limit exceeded";
        case STATUS_PROTOCOL_VERSION_ERROR: return "protocol version error";
        default: return "waiting";
    }
}
}  // namespace

struct SuplaSpaBridge::Impl
{
    Impl(BWC* spaInstance,
         const uint8_t guid[SUPLA_GUID_SIZE],
         const uint8_t authKey[SUPLA_AUTHKEY_SIZE],
         const char* requestedServer,
         const char* email)
        : spa(spaInstance)
    {
        SpaSnapshot initialSnapshot;
        if(spa == nullptr || !spa->getSnapshot(initialSnapshot)) return;
        capabilities = initialSnapshot.capabilities;

        // Validate and persist storage before registering process-lifetime
        // SUPLA Elements.  A preflight failure can then be cleaned up and
        // retried safely without leaving dangling global channel callbacks.
        configReady = prepareConfig(guid, authKey, requestedServer, email);
        if(!configReady) return;

        water = new Supla::Sensor::VirtualThermometer;
        water->getChannel()->setChannelNumber(WATER_TEMPERATURE);
        water->setInitialCaption("Temperatura wody");

        if(capabilities.hasHeater)
        {
            heatOutput = new Supla::Control::RemoteOutputInterface(true);
            thermostat = new SpaHvac(spa, heatOutput);
        }

        if(capabilities.supportsPowerControl)
            power = new SpaRelay(spa, SETPOWER, SPA_POWER, "Zasilanie SPA");
        if(capabilities.hasFilterPump)
            pump = new SpaRelay(spa, SETPUMP, FILTER_PUMP, "Pompa filtrująca");
        if(capabilities.hasBubbles)
            bubbles = new SpaRelay(spa, SETBUBBLES, AIRJET, "Bąbelki AirJet");
        if(capabilities.hasJets)
            jets = new SpaRelay(spa, SETJETS, HYDROJET, "HydroJet");

        virtualTemperature = new Supla::Sensor::VirtualThermometer;
        virtualTemperature->getChannel()->setChannelNumber(VIRTUAL_TEMPERATURE);
        virtualTemperature->setInitialCaption("Temperatura wirtualna");

        if(capabilities.hasAmbientTemperatureSensor)
        {
            ambientTemperature = new Supla::Sensor::VirtualThermometer;
            ambientTemperature->getChannel()->setChannelNumber(AMBIENT_TEMPERATURE);
            ambientTemperature->setInitialCaption("Temperatura otoczenia");
        }

        for(uint8_t i = 0; i < MEASUREMENT_COUNT; ++i)
        {
            if(!isMeasurementSupported(i)) continue;
            const MeasurementSpec& spec = MEASUREMENT_SPECS[i];
            measurements[i] = new SpaMeasurement(FIRST_MEASUREMENT + i,
                                                 spec.caption,
                                                 spec.unit,
                                                 spec.precision);
        }

        resetChlorine = new SpaRelay(spa, RESETCLTIMER, RESET_CHLORINE,
                                     "Reset wieku chloru", true);
        resetFilterReplace = new SpaRelay(spa, RESETFREPLACETIMER, RESET_FILTER_REPLACE,
                                         "Reset wieku wkładu filtra", true);
        resetFilterClean = new SpaRelay(spa, RESETFCLEANTIMER, RESET_FILTER_CLEAN,
                                       "Reset czasu czyszczenia filtra", true);
        resetFilterRinse = new SpaRelay(spa, RESETFRINSETIMER, RESET_FILTER_RINSE,
                                       "Reset czasu płukania filtra", true);
    }

    bool isMeasurementSupported(uint8_t index) const
    {
        switch(index)
        {
            case PUMP_RUNTIME: return capabilities.hasFilterPump;
            case HEATING_RUNTIME:
            case TIME_TO_READY: return capabilities.hasHeater;
            case BUBBLES_RUNTIME: return capabilities.hasBubbles;
            case JETS_RUNTIME: return capabilities.hasJets;
            default: return true;
        }
    }

    bool prepareConfig(const uint8_t guid[SUPLA_GUID_SIZE],
                       const uint8_t authKey[SUPLA_AUTHKEY_SIZE],
                       const char* requestedServer,
                       const char* email)
    {
        auto* config = Supla::Storage::ConfigInstance();
        if(config == nullptr || !config->getInitResult()) return false;

        char previousGuid[SUPLA_GUID_SIZE] = {};
        char previousAuthKey[SUPLA_AUTHKEY_SIZE] = {};
        char previousEmail[SUPLA_EMAIL_MAXSIZE] = {};
        const bool associationUnchanged =
            config->getGUID(previousGuid) &&
            config->getAuthKey(previousAuthKey) &&
            config->getEmail(previousEmail) &&
            memcmp(previousGuid, guid, sizeof(previousGuid)) == 0 &&
            memcmp(previousAuthKey, authKey, sizeof(previousAuthKey)) == 0 &&
            strcmp(previousEmail, email) == 0;

        bool ok = config->setGUID(reinterpret_cast<const char*>(guid));
        ok = config->setAuthKey(reinterpret_cast<const char*>(authKey)) && ok;
        ok = config->setEmail(email) && ok;
        strlcpy(discoveryEmail, email, sizeof(discoveryEmail));
        memcpy(discoveryGuid, guid, sizeof(discoveryGuid));
        ok = config->setSuplaCommProtocolEnabled(true) && ok;
        ok = config->setDeviceName("Lay-Z-Spa") && ok;
        ok = config->setDeviceMode(Supla::DEVICE_MODE_NORMAL) && ok;

        // Autodiscovery saves the selected server in Config.  Keep that value
        // on later boots when the EEPROM setting intentionally stays empty.
        if(requestedServer[0] != '\0')
        {
            ok = config->setSuplaServer(requestedServer) && ok;
            serverDiscoveryPending = false;
        }
        else
        {
            char storedServer[SUPLA_SERVER_NAME_MAXSIZE] = {};
            if(!associationUnchanged || !config->getSuplaServer(storedServer))
            {
                ok = config->setSuplaServer("") && ok;
                storedServer[0] = '\0';
            }
            serverDiscoveryPending = storedServer[0] == '\0';
        }

        config->commit();
        return ok;
    }

    bool discoverServerIfDue()
    {
        if(!serverDiscoveryPending) return true;
        const uint32_t now = millis();
        if(nextServerDiscoveryAttempt != 0 &&
           !deadlineReached(now, nextServerDiscoveryAttempt))
            return false;

        char discoveredServer[SUPLA_SERVER_NAME_MAXSIZE] = {};
        if(!discoverSuplaServerBounded(discoveryEmail,
                                       discoveryGuid,
                                       SuplaDevice.getSuplaCACert(),
                                       discoveredServer,
                                       sizeof(discoveredServer)))
        {
            nextServerDiscoveryAttempt = millis() + SERVER_DISCOVERY_RETRY_MS;
            return false;
        }

        auto* config = Supla::Storage::ConfigInstance();
        if(config == nullptr || !config->setSuplaServer(discoveredServer))
        {
            nextServerDiscoveryAttempt = millis() + SERVER_DISCOVERY_RETRY_MS;
            return false;
        }
        config->commit();
        SuplaDevice.setServer(discoveredServer);
        auto* srpc = SuplaDevice.getSrpcLayer();
        if(srpc != nullptr) srpc->onLoadConfig();
        serverDiscoveryPending = false;
        return true;
    }

    void synchronize(const SpaSnapshot& snapshot, bool force)
    {
        water->getChannel()->setStateOnline();
        water->setValue(snapshot.waterTemperatureC);

        if(thermostat != nullptr) thermostat->syncPhysical(snapshot, force);

        if(power != nullptr)
        {
            power->setAvailable(true, force);
            power->publish(snapshot.power, force);
        }
        if(pump != nullptr)
        {
            pump->setAvailable(true, force);
            pump->publish(snapshot.filterPump, force);
        }
        if(bubbles != nullptr)
        {
            bubbles->setAvailable(true, force);
            bubbles->publish(snapshot.bubbles, force);
        }
        if(jets != nullptr)
        {
            jets->setAvailable(true, force);
            jets->publish(snapshot.jets, force);
        }

        virtualTemperature->getChannel()->setStateOnline();
        virtualTemperature->setValue(snapshot.virtualTemperatureC);

        if(ambientTemperature != nullptr)
        {
            ambientTemperature->getChannel()->setStateOnline();
            ambientTemperature->setValue(snapshot.ambientTemperatureC);
        }

        measurements[CHLORINE_AGE]->publish(snapshot.chlorineAgeDays, force);
        measurements[FILTER_REPLACE_AGE]->publish(snapshot.filterReplaceAgeDays, force);
        measurements[FILTER_CLEAN_AGE]->publish(snapshot.filterCleanAgeDays, force);
        measurements[FILTER_RINSE_AGE]->publish(snapshot.filterRinseAgeDays, force);
        if(measurements[PUMP_RUNTIME] != nullptr)
            measurements[PUMP_RUNTIME]->publish(snapshot.filterPumpTimeSeconds / 3600.0, force);
        if(measurements[HEATING_RUNTIME] != nullptr)
            measurements[HEATING_RUNTIME]->publish(snapshot.heatingTimeSeconds / 3600.0, force);
        if(measurements[BUBBLES_RUNTIME] != nullptr)
            measurements[BUBBLES_RUNTIME]->publish(snapshot.bubblesTimeSeconds / 3600.0, force);
        if(measurements[JETS_RUNTIME] != nullptr)
            measurements[JETS_RUNTIME]->publish(snapshot.jetsTimeSeconds / 3600.0, force);
        measurements[ENERGY_TODAY]->publish(snapshot.energyTodayKWh, force);
        measurements[ENERGY_TOTAL]->publish(snapshot.energyTotalKWh, force);
        measurements[CURRENT_POWER]->publish(snapshot.powerWatts, force);

        double timeToReady = NAN;
        if(snapshot.readyState == SpaReadyState::READY) timeToReady = 0.0;
        else if(snapshot.readyState == SpaReadyState::NOT_READY && snapshot.timeToReadyHours >= 0.0f)
            timeToReady = snapshot.timeToReadyHours;
        if(measurements[TIME_TO_READY] != nullptr)
            measurements[TIME_TO_READY]->publish(timeToReady, force);
        measurements[SPA_ERROR]->publish(snapshot.error, force);

        resetChlorine->setAvailable(true, force);
        resetChlorine->publish(false, force);
        resetFilterReplace->setAvailable(true, force);
        resetFilterReplace->publish(false, force);
        resetFilterClean->setAvailable(true, force);
        resetFilterClean->publish(false, force);
        resetFilterRinse->setAvailable(true, force);
        resetFilterRinse->publish(false, force);
    }

    BWC* spa = nullptr;
    SpaCapabilities capabilities;
    NtpPreservingSuplaClock clock;
    SharedLittleFsConfig config;
    ExistingWifiNetwork network;

    Supla::Sensor::VirtualThermometer* water = nullptr;
    Supla::Control::RemoteOutputInterface* heatOutput = nullptr;
    SpaHvac* thermostat = nullptr;
    SpaRelay* power = nullptr;
    SpaRelay* pump = nullptr;
    SpaRelay* bubbles = nullptr;
    SpaRelay* jets = nullptr;
    Supla::Sensor::VirtualThermometer* virtualTemperature = nullptr;
    Supla::Sensor::VirtualThermometer* ambientTemperature = nullptr;
    SpaMeasurement* measurements[MEASUREMENT_COUNT] = {};
    SpaRelay* resetChlorine = nullptr;
    SpaRelay* resetFilterReplace = nullptr;
    SpaRelay* resetFilterClean = nullptr;
    SpaRelay* resetFilterRinse = nullptr;
    bool running = false;
    bool configReady = false;
    bool serverDiscoveryPending = false;
    uint32_t nextServerDiscoveryAttempt = 0;
    char discoveryEmail[SUPLA_EMAIL_MAXSIZE] = {};
    uint8_t discoveryGuid[SUPLA_GUID_SIZE] = {};
};

SuplaSpaBridge::SuplaSpaBridge() = default;

SuplaSpaBridge::~SuplaSpaBridge()
{
    stop();
    // supla-device keeps its Elements in process-lifetime linked lists and has
    // no supported deinitialization API.  Impl therefore intentionally lives
    // until the imminent ESP restart instead of leaving dangling callbacks.
}

bool SuplaSpaBridge::begin(BWC* spa,
                           const uint8_t guid[16],
                           const uint8_t authKey[16],
                           const char* server,
                           const char* email)
{
    if(impl_ != nullptr || spa == nullptr || guid == nullptr || authKey == nullptr ||
       server == nullptr || email == nullptr || email[0] == '\0' ||
       strlen(server) >= SUPLA_SERVER_NAME_MAXSIZE ||
       strlen(email) >= SUPLA_EMAIL_MAXSIZE ||
       !bufferHasNonZeroByte(guid, SUPLA_GUID_SIZE) ||
       !bufferHasNonZeroByte(authKey, SUPLA_AUTHKEY_SIZE))
        return false;

    // The global SuplaDevice cannot be initialized twice in one boot.
    static bool bridgeCreated = false;
    if(bridgeCreated) return false;

    impl_ = new(std::nothrow) Impl(spa, guid, authKey, server, email);
    if(impl_ == nullptr) return false;
    if(!impl_->configReady)
    {
        delete impl_;
        impl_ = nullptr;
        return false;
    }
    bridgeCreated = true;

    Supla::Network::setSSLEnabled(true);
    SuplaDevice.setName("Lay-Z-Spa");
    SuplaDevice.setSwVersion(FW_VERSION);
    SuplaDevice.setInitialMode(Supla::InitialMode::StartOffline);
    SuplaDevice.setShowUptimeInChannelState(false);
    SuplaDevice.setProtoVerboseLog(false);

    impl_->running = SuplaDevice.begin(reinterpret_cast<const char*>(guid),
                                       server,
                                       email,
                                       reinterpret_cast<const char*>(authKey),
                                       SUPLA_PROTO_VERSION);
    return impl_->running;
}

void SuplaSpaBridge::iterate()
{
    if(impl_ == nullptr || !impl_->running) return;
    // supla-device's built-in autodiscovery constructs the platform client
    // directly, bypassing Network::createClient(), and can block for several
    // seconds.  Resolve it here through the same bounded TLS transport first.
    if(!impl_->discoverServerIfDue()) return;
    SuplaDevice.iterate();
}

void SuplaSpaBridge::sync(const SpaSnapshot& snapshot, bool force)
{
    if(impl_ == nullptr || !impl_->running) return;
    impl_->synchronize(snapshot, force);
}

void SuplaSpaBridge::stop()
{
    if(impl_ == nullptr || !impl_->running) return;
    Supla::Network::DisconnectProtocols();
    SuplaDevice.disableNetwork();
    impl_->running = false;
}

bool SuplaSpaBridge::isRunning() const
{
    return impl_ != nullptr && impl_->running;
}

const char* SuplaSpaBridge::status() const
{
    if(impl_ == nullptr) return "not started";
    if(!impl_->running) return "stopped";
    if(impl_->serverDiscoveryPending) return "discovering server";
    return statusText(SuplaDevice.getCurrentStatus());
}
