#include "main.h"
#include "crash_summary.h"
#include "ports.h"
#include "web_update_page.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <Updater.h>
#include <flash_hal.h>

#define WS_PERIOD 4.0
// initial stack
char *stack_start;
uint32_t heap_water_mark;

namespace
{
constexpr size_t SUPLA_CSRF_TOKEN_BYTES = 16;
constexpr time_t MIN_VALID_UNIX_TIME = 1609459200; // 2021-01-01 UTC
constexpr uint32_t SUPLA_FIRST_CLAIM_WINDOW_MS = 15UL * 60UL * 1000UL;
constexpr uint32_t SUPLA_RECOVERY_WINDOW_MS = 2UL * 60UL * 1000UL;
char suplaCsrfToken[SUPLA_CSRF_TOKEN_BYTES * 2 + 1] = {};
uint32_t suplaProvisioningDeadlineMs = 0;
uint32_t deviceRestartDeadlineMs = 0;
bool factoryResetRequested = false;
bool wifiConfigurationLoaded = false;
volatile uint32_t wifiGotIpCount = 0;
volatile uint32_t wifiDisconnectCount = 0;
volatile uint32_t wifiLastDisconnectMillis = 0;
volatile uint8_t wifiLastDisconnectReason = 0;

constexpr uint32_t MIN_FIRMWARE_UPDATE_BYTES = 64UL * 1024UL;
constexpr uint32_t WEB_UPDATE_STALL_TIMEOUT_MS = 30UL * 1000UL;
constexpr uint32_t POST_FS_RESTART_RTC_OFFSET = 32; // after eboot's 128-byte command area
constexpr uint32_t POST_FS_RESTART_MAGIC = 0x4C5A5350UL; // "LZSP"
constexpr uint32_t POST_FS_RESTART_DELAY_MS = 120UL * 1000UL;

enum class WebUpdateTarget : uint8_t
{
    NONE,
    FIRMWARE,
    FILESYSTEM
};

struct WebUpdateState
{
    WebUpdateTarget target = WebUpdateTarget::NONE;
    bool started = false;
    bool finished = false;
    bool success = false;
    uint32_t expectedSize = 0;
    uint32_t receivedSize = 0;
    char error[128] = {};
};

WebUpdateState webUpdateState;
bool webUpdateInProgress = false;
uint32_t webUpdateLastActivityMs = 0;
bool postFilesystemRestartPending = false;
uint32_t postFilesystemRestartEarliestMs = 0;

class CountingPrint final : public Print
{
    public:
        size_t write(uint8_t) override
        {
            ++count_;
            return 1;
        }

        size_t write(const uint8_t*, size_t size) override
        {
            count_ += size;
            return size;
        }

        size_t count() const { return count_; }

    private:
        size_t count_ = 0;
};

class BufferedPrint final : public Print
{
    public:
        explicit BufferedPrint(Print& destination) : destination_(destination) {}

        size_t write(uint8_t value) override
        {
            return write(&value, 1);
        }

        size_t write(const uint8_t* data, size_t size) override
        {
            if(data == nullptr && size != 0)
            {
                setWriteError();
                return 0;
            }

            size_t consumed = 0;
            while(consumed < size)
            {
                const size_t available = sizeof(buffer_) - used_;
                const size_t chunk = std::min(available, size - consumed);
                memcpy(buffer_ + used_, data + consumed, chunk);
                used_ += chunk;
                consumed += chunk;
                if(used_ == sizeof(buffer_) && !flushBuffer())
                    return consumed;
            }
            return consumed;
        }

        bool finish()
        {
            return flushBuffer() && getWriteError() == 0;
        }

    private:
        bool flushBuffer()
        {
            if(used_ == 0) return getWriteError() == 0;
            const size_t written = destination_.write(buffer_, used_);
            if(written != used_)
            {
                setWriteError();
                return false;
            }
            used_ = 0;
            return true;
        }

        Print& destination_;
        uint8_t buffer_[192] = {};
        size_t used_ = 0;
};

bool hasValidUnixTime()
{
    return time(nullptr) >= MIN_VALID_UNIX_TIME;
}

bool hasHomeNetworkConfiguration()
{
    // A valid web configuration is the normal case. Keep compatibility with
    // older installations that rely only on credentials persisted by the
    // ESP8266 SDK and are already connected.
    return wifiConfigurationLoaded || WiFi.status() == WL_CONNECTED;
}

bool isSuplaProvisioningOpen()
{
    return suplaProvisioningDeadlineMs != 0 &&
           static_cast<int32_t>(suplaProvisioningDeadlineMs - millis()) > 0;
}

uint32_t suplaProvisioningSecondsRemaining()
{
    if(!isSuplaProvisioningOpen()) return 0;
    return (suplaProvisioningDeadlineMs - millis() + 999UL) / 1000UL;
}

void openSuplaProvisioningWindow()
{
    // Saving SUPLA before the home-network credentials must not turn an
    // unfinished first-time setup into a two-minute recovery session.
    const uint32_t duration =
        supla_info->configured && wifiConfigurationLoaded
        ? SUPLA_RECOVERY_WINDOW_MS
        : SUPLA_FIRST_CLAIM_WINDOW_MS;
    suplaProvisioningDeadlineMs = millis() + duration;
}

bool requestArrivedViaSoftAp()
{
    if(server == nullptr || (WiFi.getMode() & WIFI_AP) == 0) return false;
    const IPAddress softApAddress = WiFi.softAPIP();

    // The accepted socket normally identifies the ingress interface by its
    // local address. Fail closed if the home network assigned the station
    // exactly the same address as the setup AP: in that rare case a LAN
    // request cannot be distinguished safely from an AP request.
    if(WiFi.status() == WL_CONNECTED && WiFi.localIP() == softApAddress)
        return false;

    return server->client().localIP() == softApAddress;
}

bool isSuplaConfigurationUnlocked()
{
    return isSuplaProvisioningOpen() && requestArrivedViaSoftAp();
}

void scheduleDeviceRestart(bool closeProvisioningWindow)
{
    // Restart outside the HTTP callback so ESP8266WebServer is not deleted
    // while it is still unwinding the request handler.
    if(closeProvisioningWindow)
    {
        suplaProvisioningDeadlineMs = 0;
        memset(suplaCsrfToken, 0, sizeof(suplaCsrfToken));
    }
    deviceRestartDeadlineMs = millis() + 1500UL;
}

bool isProtectedStoragePath(String path)
{
    path.trim();
    path.replace('\\', '/');
    path.toLowerCase();

    const int queryStart = path.indexOf('?');
    if (queryStart >= 0)
        path.remove(queryStart);
    const int fragmentStart = path.indexOf('#');
    if (fragmentStart >= 0)
        path.remove(fragmentStart);

    if (!path.startsWith(F("/")))
        path = String(F("/")) + path;
    while (path.indexOf(F("//")) >= 0)
        path.replace(F("//"), F("/"));

    // Reject traversal and ambiguous dot segments instead of trying to
    // reproduce LittleFS path normalization in the HTTP layer.
    if (path == F("/..") || path.startsWith(F("/../")) ||
        path.endsWith(F("/..")) || path.indexOf(F("/../")) >= 0 ||
        path == F("/.") || path.startsWith(F("/./")) ||
        path.endsWith(F("/.")) || path.indexOf(F("/./")) >= 0)
        return true;

    // The file server transparently prefers .gz variants. Protect those under
    // the same policy as their uncompressed counterparts.
    while (path.endsWith(F(".gz")))
        path.remove(path.length() - 3);

    // These diagnostic files can contain raw stack bytes or command/state
    // history.  In particular, a post-mortem stack dump may include copies of
    // EEPROM-backed credentials, so never expose them through the generic
    // file browser.
    if (path == F("/mqtt.json") || path == F("/wifi.json") ||
        path == F("/crashlog.txt") || path == F("/crash-summary.bin") ||
        path == F("/crash-summary.tmp") || path == F("/debugoutput.txt") ||
        path == F("/debug.txt") || path == F("/log.txt") ||
        path == F("/bootlog.txt"))
        return true;

    return path.startsWith(F("/supla-dev.")) ||
           path == F("/supla") || path.startsWith(F("/supla/"));
}

bool isProtectedMutationPath(String path)
{
    // Credentials/runtime storage is neither readable nor mutable through the
    // generic file manager.  Web assets remain readable, but are immutable so
    // a LAN client cannot plant same-origin JavaScript which later steals the
    // short-lived provisioning token when the owner uses the setup AP.
    if(isProtectedStoragePath(path)) return true;

    path.trim();
    path.replace('\\', '/');
    path.toLowerCase();
    const int queryStart = path.indexOf('?');
    if(queryStart >= 0) path.remove(queryStart);
    const int fragmentStart = path.indexOf('#');
    if(fragmentStart >= 0) path.remove(fragmentStart);
    if(!path.startsWith(F("/"))) path = String(F("/")) + path;
    while(path.indexOf(F("//")) >= 0) path.replace(F("//"), F("/"));
    while(path.endsWith(F(".gz"))) path.remove(path.length() - 3);

    if(path.endsWith(F(".html")) || path.endsWith(F(".htm")) ||
       path.endsWith(F(".js")) || path.endsWith(F(".css")))
        return true;

    // Translation values are inserted with innerHTML by the legacy i18n code,
    // so they are executable web content for integrity purposes.
    if(path.startsWith(F("/langue_")) && path.endsWith(F(".txt")))
        return true;

    return path == F("/manifest.json") || path == F("/filelist.txt");
}

uint32_t suplaConfigChecksum(const sSupla_info_eeprom_t &config)
{
    // FNV-1a over the versioned payload (everything before checksum).
    const uint8_t *bytes = reinterpret_cast<const uint8_t *>(&config);
    uint32_t hash = 2166136261UL;
    for (size_t i = 0; i < offsetof(sSupla_info_eeprom_t, checksum); ++i)
    {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

bool isAllZero(const uint8_t *value, size_t length)
{
    uint8_t aggregate = 0;
    for (size_t i = 0; i < length; ++i)
        aggregate |= value[i];
    return aggregate == 0;
}

void ensureSuplaCsrfToken()
{
    if (suplaCsrfToken[0] != '\0')
        return;

    uint8_t randomBytes[SUPLA_CSRF_TOKEN_BYTES] = {};
    do
    {
        os_get_random(randomBytes, sizeof(randomBytes));
    } while (isAllZero(randomBytes, sizeof(randomBytes)));

    static const char HEX_DIGITS[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(randomBytes); ++i)
    {
        suplaCsrfToken[i * 2] = HEX_DIGITS[randomBytes[i] >> 4];
        suplaCsrfToken[i * 2 + 1] = HEX_DIGITS[randomBytes[i] & 0x0F];
    }
}

bool isValidSuplaCsrfToken(const char *provided)
{
    ensureSuplaCsrfToken();
    if (provided == nullptr || strlen(provided) != sizeof(suplaCsrfToken) - 1)
        return false;

    uint8_t difference = 0;
    for (size_t i = 0; i < sizeof(suplaCsrfToken) - 1; ++i)
        difference |= static_cast<uint8_t>(provided[i] ^ suplaCsrfToken[i]);
    return difference == 0;
}

void generateSuplaIdentity(sSupla_info &config)
{
    do
    {
        os_get_random(config.guid, sizeof(config.guid));
    } while (isAllZero(config.guid, sizeof(config.guid)));

    do
    {
        os_get_random(config.authKey, sizeof(config.authKey));
    } while (isAllZero(config.authKey, sizeof(config.authKey)));
}

bool hasJsonContentType()
{
    String contentType = server->header(F("Content-Type"));
    contentType.trim();
    const int parameterStart = contentType.indexOf(';');
    if (parameterStart >= 0)
    {
        contentType.remove(parameterStart);
        contentType.trim();
    }
    return contentType.equalsIgnoreCase(F("application/json"));
}

bool hasExclusivePlainBody()
{
    // ESP8266WebServer keeps the raw body in a synthetic "plain" argument,
    // but (despite arg()/argName() exposing it) does not include that synthetic
    // entry in args() on ESP8266 Arduino core 3.1.2. Requiring zero regular
    // arguments prevents a query/form value from shadowing the JSON body.
    return server->args() == 0 && server->hasArg(F("plain")) &&
           server->argName(0) == F("plain");
}

bool isValidSuplaEmail(const String &email)
{
    const int at = email.indexOf('@');
    if (email.length() < 3 || at <= 0 || at >= static_cast<int>(email.length()) - 1 ||
        email.indexOf('@', at + 1) >= 0)
        return false;
    for (size_t i = 0; i < email.length(); ++i)
    {
        const char c = email[i];
        if (c <= ' ' || c == '/' || c == '\\')
            return false;
    }
    return true;
}

bool isValidSuplaServer(const String &serverName)
{
    if (serverName.isEmpty())
        return true; // SUPLA Cloud autodiscovery based on the account e-mail.
    if (serverName[0] == '.' || serverName[0] == '-' ||
        serverName[serverName.length() - 1] == '.' || serverName[serverName.length() - 1] == '-')
        return false;
    for (size_t i = 0; i < serverName.length(); ++i)
    {
        const char c = serverName[i];
        const bool valid = (c >= 'a' && c <= 'z') ||
                           (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '-';
        if (!valid)
            return false;
        if (c == '.' &&
            ((i > 0 && (serverName[i - 1] == '.' || serverName[i - 1] == '-')) ||
             (i + 1 < serverName.length() && serverName[i + 1] == '-')))
            return false;
    }
    return true;
}

void removeSuplaRuntimeConfig()
{
    LittleFS.remove(F("/supla-dev.cfg"));
    LittleFS.remove(F("/supla-dev.bak"));
    LittleFS.remove(F("/supla-dev.tmp"));

    Dir directory = LittleFS.openDir(F("/supla"));
    while (directory.next())
    {
        String path = directory.fileName();
        if (!path.startsWith("/"))
            path = String(F("/supla/")) + path;
        LittleFS.remove(path);
        BWC_YIELD;
    }
}

const char *currentSuplaStatus()
{
    if(supla_info == nullptr || !supla_info->enabled)
        return "disabled";
    if(!supla_info->configured)
        return "not-configured";
    if(suplaBridge == nullptr)
        return "not-running";
    if(WiFi.status() != WL_CONNECTED)
        return "waiting-for-wifi";
    if(!hasValidUnixTime())
        return "waiting-for-time";
    return suplaBridge->status();
}

bool isFactoryAssetPath(String path)
{
    path.replace('\\', '/');
    while(path.startsWith(F("/"))) path.remove(0, 1);

    File manifest = LittleFS.open(F("/filelist.txt"), "r");
    if(!manifest) return false;

    while(manifest.available())
    {
        String entry = manifest.readStringUntil('\n');
        entry.trim();
        entry.replace('\\', '/');
        while(entry.startsWith(F("/"))) entry.remove(0, 1);
        if(entry == path)
        {
            manifest.close();
            return true;
        }
        BWC_YIELD;
    }
    manifest.close();
    return false;
}

void removeKnownMutableLittleFsFiles()
{
    static const char *const paths[] = {
        "/settings.json",
        "/states.txt",
        "/cmdq.json",
        "/cmdq.backup",
        "/webconfig.json",
        "/wifi.json",
        "/mqtt.json",
        "/hwcfg.json",
        "/crashlog.txt",
        "/crash-summary.bin",
        "/crash-summary.tmp",
        "/debugoutput.txt",
        "/debug.txt",
        "/log.txt",
        "/bootlog.txt"
    };
    for(const char *path : paths)
    {
        LittleFS.remove(path);
        BWC_YIELD;
    }
    removeSuplaRuntimeConfig();
}

void removeNonFactoryLittleFsFiles()
{
    removeKnownMutableLittleFsFiles();

    // The build-generated manifest is the authoritative list of files that
    // belong to the installed web UI and bundled assets. Remove everything
    // else, including files created by the former generic file manager and
    // user-named command-queue backups. If an old image has no manifest, the
    // known mutable files above are still erased without risking the web UI.
    if(!LittleFS.exists(F("/filelist.txt"))) return;

    // Removing while iterating may make the directory cursor skip an entry on
    // some LittleFS releases. Repeat a bounded number of passes until stable.
    for(uint8_t pass = 0; pass < 4; ++pass)
    {
        bool removedAny = false;
        Dir root = LittleFS.openDir("/");
        while(root.next())
        {
            String path = root.fileName();
            if(!path.startsWith(F("/"))) path = String(F("/")) + path;
            if(path == F("/filelist.txt") || isFactoryAssetPath(path))
                continue;
            if(LittleFS.remove(path)) removedAny = true;
            BWC_YIELD;
        }
        if(!removedAny) break;
    }
}

bool erasePersistentConfiguration()
{
    EEPROM.begin(EEPROM_CONFIG_SIZE);
    for(size_t i = 0; i < EEPROM_CONFIG_SIZE; ++i)
        EEPROM.write(i, 0xff);
    const bool saved = EEPROM.commit();
    EEPROM.end();
    return saved;
}

void performFactoryReset()
{
    // Stop every writer before deleting runtime files. Otherwise the normal
    // loop could recreate settings.json or states.txt between deletion and
    // restart.
    stopall();

    if(LittleFS.begin())
    {
        removeNonFactoryLittleFsFiles();
        LittleFS.end();
    }

    if(!erasePersistentConfiguration())
    {
        delay(100);
        erasePersistentConfiguration();
    }

    uint32_t marker = 0;
    ESP.rtcUserMemoryWrite(POST_FS_RESTART_RTC_OFFSET, &marker, sizeof(marker));
#if defined(ESP8266)
    ESP.eraseConfig();
#endif
    delay(250);
    ESP.restart();
}

uint32_t maxFirmwareUpdateSize()
{
    const uint32_t freeSpace = ESP.getFreeSketchSpace();
    if(freeSpace <= 0x1000UL) return 0;
    return (freeSpace - 0x1000UL) & 0xFFFFF000UL;
}

uint32_t filesystemUpdateSize()
{
    return FS_PHYS_SIZE;
}

const __FlashStringHelper *webUpdateTargetName(WebUpdateTarget target)
{
    return target == WebUpdateTarget::FILESYSTEM ? F("filesystem") : F("firmware");
}

void resetWebUpdateState(WebUpdateTarget target)
{
    webUpdateState = WebUpdateState{};
    webUpdateState.target = target;
}

void setWebUpdateError(const String &message)
{
    if(webUpdateState.error[0] == '\0')
        strlcpy(webUpdateState.error, message.c_str(), sizeof(webUpdateState.error));
}

bool parseWebUpdateSize(const String &header, uint32_t &result)
{
    if(header.isEmpty() || header.length() > 10) return false;

    uint32_t value = 0;
    for(size_t i = 0; i < header.length(); ++i)
    {
        const char c = header[i];
        if(c < '0' || c > '9') return false;
        const uint8_t digit = static_cast<uint8_t>(c - '0');
        if(value > (UINT32_MAX - digit) / 10UL) return false;
        value = value * 10UL + digit;
    }
    if(value == 0) return false;
    result = value;
    return true;
}

void loadPostFilesystemRestartMarker()
{
    uint32_t marker = 0;
    if(ESP.rtcUserMemoryRead(POST_FS_RESTART_RTC_OFFSET, &marker, sizeof(marker)) &&
       marker == POST_FS_RESTART_MAGIC)
    {
        postFilesystemRestartPending = true;
        postFilesystemRestartEarliestMs = millis() + POST_FS_RESTART_DELAY_MS;
        marker = 0;
        ESP.rtcUserMemoryWrite(POST_FS_RESTART_RTC_OFFSET, &marker, sizeof(marker));
    }
}

void markPostFilesystemRestart()
{
    uint32_t marker = POST_FS_RESTART_MAGIC;
    ESP.rtcUserMemoryWrite(POST_FS_RESTART_RTC_OFFSET, &marker, sizeof(marker));
}

void prepareWebUpdate(WebUpdateTarget target)
{
    webUpdateInProgress = true;
    webUpdateLastActivityMs = millis();

    // Free network buffers and prevent background clients from competing with
    // the HTTP upload. The SPA bridge itself remains alive and bwc->loop() is
    // called for every upload chunk so communication with the pump continues.
    if(suplaBridge) suplaBridge->stop();
    if(mqttClient) mqttClient->disconnect();
    if(webSocket) webSocket->disconnect();
    if(updateMqttTimer) updateMqttTimer->detach();
    if(updateWSTimer) updateWSTimer->detach();
    // The update page and endpoints live in firmware, so a failed or
    // interrupted LittleFS write remains recoverable without USB. The ESP8266
    // core 3.1.2 ATOMIC_FS_UPDATE boot path incorrectly tries to execute the
    // copied filesystem as an application; write web storage directly only
    // after every filesystem user has been stopped.
    if(target == WebUpdateTarget::FILESYSTEM)
        close_all_fs();
    ESP.wdtFeed();
}

void handleWebUpdatePage()
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
    server->send_P(200, PSTR("text/html; charset=utf-8"), WEB_UPDATE_PAGE);
}

void handleWebUpdateStatus()
{
    StaticJsonDocument<256> doc;
    doc[F("firmware")] = FW_VERSION;
    doc[F("minFirmwareBytes")] = MIN_FIRMWARE_UPDATE_BYTES;
    doc[F("maxFirmwareBytes")] = maxFirmwareUpdateSize();
    doc[F("filesystemBytes")] = filesystemUpdateSize();
    doc[F("freeHeap")] = ESP.getFreeHeap();
    doc[F("uptimeSeconds")] = millis() / 1000UL;
    doc[F("postFilesystemRestartPending")] = postFilesystemRestartPending;
    doc[F("updateInProgress")] = webUpdateInProgress;

    String json;
    json.reserve(256);
    if(serializeJson(doc, json) == 0)
    {
        server->send(500, F("application/json"), F("{\"error\":\"serialization\"}"));
        return;
    }
    server->sendHeader(F("Cache-Control"), F("no-store"));
    server->send(200, F("application/json"), json);
}

void handleWebUpdateUpload(WebUpdateTarget target)
{
    HTTPUpload &upload = server->upload();

    if(upload.status == UPLOAD_FILE_START)
    {
        resetWebUpdateState(target);
        if(webUpdateInProgress || Update.isRunning())
        {
            setWebUpdateError(F("Inna aktualizacja jest już w toku."));
            return;
        }

        String filename = upload.filename;
        filename.toLowerCase();
        if(!filename.endsWith(F(".bin")))
        {
            setWebUpdateError(F("Plik aktualizacji musi mieć rozszerzenie .bin."));
            return;
        }

        uint32_t expectedSize = 0;
        if(!parseWebUpdateSize(server->header(F("X-Update-Size")), expectedSize))
        {
            setWebUpdateError(F("Brak prawidłowego nagłówka rozmiaru pliku."));
            return;
        }
        webUpdateState.expectedSize = expectedSize;

        if(target == WebUpdateTarget::FILESYSTEM)
        {
            if(expectedSize != filesystemUpdateSize())
            {
                setWebUpdateError(String(F("Nieprawidłowy rozmiar obrazu LittleFS. Oczekiwano ")) +
                                  filesystemUpdateSize() + F(" bajtów."));
                return;
            }
        }
        else if(expectedSize < MIN_FIRMWARE_UPDATE_BYTES ||
                expectedSize > maxFirmwareUpdateSize())
        {
            setWebUpdateError(F("Rozmiar firmware jest poza dozwolonym zakresem."));
            return;
        }

        prepareWebUpdate(target);
        Update.runAsync(false);
        if(!Update.begin(expectedSize,
                         target == WebUpdateTarget::FILESYSTEM ? U_FS : U_FLASH))
        {
            setWebUpdateError(String(F("Nie można rozpocząć zapisu: ")) + Update.getErrorString());
            return;
        }
        webUpdateState.started = true;
        BWC_LOG_P(PSTR("Web update > %s start, %u bytes\n"),
                  String(webUpdateTargetName(target)).c_str(), expectedSize);
        return;
    }

    if(webUpdateState.target != target ||
       webUpdateState.error[0] != '\0')
        return;

    if(upload.status == UPLOAD_FILE_WRITE)
    {
        if(!webUpdateState.started ||
           upload.currentSize > webUpdateState.expectedSize - webUpdateState.receivedSize)
        {
            setWebUpdateError(F("Odebrano więcej danych niż zadeklarowano."));
            if(Update.isRunning()) Update.end(false);
            return;
        }

        // A full LittleFS image generated by mklittlefs carries its on-disk
        // magic in the first block. Size alone is not enough to distinguish a
        // random 1,024,000-byte file from a usable filesystem image.
        if(target == WebUpdateTarget::FILESYSTEM &&
           webUpdateState.receivedSize == 0 &&
           (upload.currentSize < 16 || memcmp(upload.buf + 8, "littlefs", 8) != 0))
        {
            setWebUpdateError(F("Plik nie jest prawidłowym obrazem LittleFS."));
            if(Update.isRunning()) Update.end(false);
            return;
        }

        // ESP8266WebServer consumes the multipart request synchronously. Keep
        // forwarding CIO/DSP traffic between flash chunks instead of leaving
        // the connected pump unattended for the duration of the upload.
        if(bwc) bwc->loop();
        const size_t written = Update.write(upload.buf, upload.currentSize);
        if(written != upload.currentSize)
        {
            setWebUpdateError(String(F("Błąd zapisu pamięci flash: ")) + Update.getErrorString());
            if(Update.isRunning()) Update.end(false);
            return;
        }
        webUpdateState.receivedSize += written;
        webUpdateLastActivityMs = millis();
        if(bwc) bwc->loop();
        ESP.wdtFeed();
        BWC_YIELD;
        return;
    }

    if(upload.status == UPLOAD_FILE_END)
    {
        webUpdateLastActivityMs = millis();
        if(!webUpdateState.started ||
           webUpdateState.receivedSize != webUpdateState.expectedSize ||
           upload.totalSize != webUpdateState.expectedSize)
        {
            setWebUpdateError(F("Plik jest niekompletny lub ma inny rozmiar niż zadeklarowany."));
            if(Update.isRunning()) Update.end(false);
            return;
        }
        if(!Update.end(false))
        {
            setWebUpdateError(String(F("Nie udało się zakończyć aktualizacji: ")) +
                              Update.getErrorString());
            return;
        }
        webUpdateState.finished = true;
        webUpdateState.success = true;
        if(target == WebUpdateTarget::FILESYSTEM)
            markPostFilesystemRestart();
        BWC_LOG_P(PSTR("Web update > %s verified, %u bytes\n"),
                  String(webUpdateTargetName(target)).c_str(), webUpdateState.receivedSize);
        return;
    }

    if(upload.status == UPLOAD_FILE_ABORTED)
    {
        if(Update.isRunning()) Update.end(false);
        setWebUpdateError(F("Wysyłanie pliku zostało przerwane."));
        if(webUpdateInProgress)
            scheduleDeviceRestart(true);
    }
}

void handleWebUpdateResult(WebUpdateTarget target)
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
    if(webUpdateState.target != target)
    {
        server->send(409, F("application/json"),
                     F("{\"ok\":false,\"error\":\"Niezgodny typ aktualizacji.\"}"));
        return;
    }

    StaticJsonDocument<256> doc;
    doc[F("ok")] = webUpdateState.success;
    doc[F("target")] = webUpdateTargetName(target);
    doc[F("bytes")] = webUpdateState.receivedSize;
    if(!webUpdateState.success)
        doc[F("error")] = webUpdateState.error[0] != '\0'
                           ? webUpdateState.error
                           : "Aktualizacja nie została zakończona.";

    String json;
    json.reserve(256);
    serializeJson(doc, json);
    // ESP8266WebServer may keep the upload connection alive. Disable Nagle and
    // wait for this small result to leave the TCP buffer before scheduling the
    // reboot, otherwise the browser can report a false network error although
    // the image was already committed successfully.
    server->client().setNoDelay(true);
    server->send(webUpdateState.success ? 200 : 400, F("application/json"), json);
    server->client().flush(1000);

    // A restart applies a successful image and also restores every background
    // service cleanly after a failed flash operation. Rejections that happened
    // before prepareWebUpdate() do not interrupt the running device.
    if(webUpdateInProgress)
        scheduleDeviceRestart(true);
}
} // namespace

// Setup a oneWire instance to communicate with any OneWire devices
// Setting arbitrarily to 231 since this isn't an actual pin
// Later during "setup" the correct pin will be set, if enabled
OneWire *oneWire;
// Pass our oneWire reference to Dallas Temperature sensor
DallasTemperature *tempSensors;

WiFiEventHandler gotIpEventHandler, disconnectedEventHandler;
void cb_gotIP(const WiFiEventStationModeGotIP &event)
{
    ++wifiGotIpCount;
    gotIP_flag = true;
}

void gotIP()
{
    ESP.wdtFeed();
    BWC_LOG_P(PSTR("start of gotip millis = %d\n"), millis());
    gotIP_flag = false;
    if(isSuplaProvisioningOpen() && (WiFi.getMode() & WIFI_AP) != 0)
    {
        WiFi.mode(WIFI_AP_STA);
        BWC_LOG_P(PSTR("Soft AP > provisioning window remains open for %d s\n"),
                  suplaProvisioningSecondsRemaining());
    }
    else
    {
        WiFi.softAPdisconnect();
        WiFi.mode(WIFI_STA);
        BWC_LOG_P(PSTR("Soft AP > closed\n"), 0);
    }
    BWC_LOG_P(PSTR("Connected as station with localIP: %s\n"), WiFi.localIP().toString().c_str());
    startNTP();
    BWC_LOG_P(PSTR("end of gotip millis = %d\n"), millis());
    bwc->print(WiFi.localIP().toString());
    if (mqtt_info->useMqtt)
        mqttConnect();
    BWC_YIELD;
}

void cb_disconnected(const WiFiEventStationModeDisconnected &event)
{
    ++wifiDisconnectCount;
    wifiLastDisconnectMillis = millis();
    wifiLastDisconnectReason = event.reason;
    disconnected_flag = true;
    // startSoftAp();
}

void setup()
{

    // init record of stack
    char stack;
    stack_start = &stack;

    Serial.begin(76800);
    loadPostFilesystemRestartMarker();
    BWC_LOG_P(PSTR("\nSetup > Start @ millis: %d\n"), millis());
    /*register wifi events */
    gotIpEventHandler = WiFi.onStationModeGotIP(cb_gotIP);
    disconnectedEventHandler = WiFi.onStationModeDisconnected(cb_disconnected);

    const bool filesystemMounted = LittleFS.begin();
#ifdef ESP8266
    CrashDiagnostics::begin(filesystemMounted);
#endif
    {
        HeapSelectIram ephemeral;
        bwc = new BWC;
        oneWire = new OneWire(231);
        tempSensors = new DallasTemperature(oneWire);
        // bootlogTimer = new Ticker;
        periodicTimer = new Ticker;
        startComplete_ticker = new Ticker;
        ntpCheck_ticker = new Ticker;
        // checkWifi_ticker = new Ticker;
        updateWSTimer = new Ticker;
        updateMqttTimer = new Ticker;
        mqtt_info = new sMQTT_info;
        mqtt_info->mqttBaseTopic = MQTT_BASE_TOPIC_F;
        mqtt_info->mqttClientId = MQTT_CLIENT_ID_F;
        mqtt_info->mqttHost = MQTT_HOST_F;
        mqtt_info->mqttPassword = MQTT_PASSWORD_F;
        mqtt_info->mqttPort = MQTT_PORT;
        mqtt_info->mqttTelemetryInterval = MQTT_TELEMETRY_INTERVAL;
        mqtt_info->mqttUsername = MQTT_USER_F;
        mqtt_info->useMqtt = MQTT_USEMQTT;
        supla_info = new sSupla_info;
        wifi_info = new sWifi_info{.enableWmApFallback = true};
    }
    bwc->setup();
    bwc->loop();
    periodicTimer->attach(periodicTimerInterval, []
                          { periodicTimerFlag = true; });
    // A Ticker callback must not delete its own Ticker or perform filesystem
    // work. Signal the cooperative loop and restore the SPA state there.
    startComplete_ticker->once(30, []
                               { restoreStatesFlag = true; });
    // update webpage every WS_PERIOD seconds. (will also be updated on state changes)
    updateWSTimer->attach(WS_PERIOD, []
                          { sendWSFlag = true; });
    loadWebConfig();
    loadSupla();
    loadWifi();
    openSuplaProvisioningWindow();
    startWiFi();
    // An unclaimed device must expose its initial setup AP.  Once claimed,
    // honour the user's recovery-AP preference; the window is always bounded.
    if (!supla_info->configured || wifi_info->enableWmApFallback)
        startSoftAp();
    startHttpServer();
    startWebSocket();
    startOTA();
    startMqtt();
    startSupla();
    if (bwc->hasTempSensor)
    {
        HeapSelectIram ephemeral;
        oneWire->begin(bwc->tempSensorPin);
        tempSensors->begin();
    }
    bwc->print("---"); // No overloaded function exists for the F() macro
    bwc->print(FW_VERSION);
    BWC_LOG_P(PSTR("End of setup() @ Millis: %d @ line: %d. Heap: %d\n"), millis(), __LINE__, ESP.getFreeHeap());
    heap_water_mark = ESP.getFreeHeap();
}

void loop()
{
    uint32_t freeheap = ESP.getFreeHeap();
    if (freeheap < heap_water_mark)
        heap_water_mark = freeheap;

    if (gotIP_flag)
        gotIP();
    if(restoreStatesFlag)
    {
        restoreStatesFlag = false;
        bwc->restoreStates();
    }
    if (disconnected_flag)
    {
        disconnected_flag = false;
        if(isSuplaProvisioningOpen() &&
           (!supla_info->configured || wifi_info->enableWmApFallback))
            startSoftAp();
        // Starting the recovery AP changes only the Wi-Fi mode; it does not
        // restart a station association that the SDK has abandoned.
        wifi_manual_reconnect();
    }

    if(suplaProvisioningDeadlineMs != 0 && !isSuplaProvisioningOpen())
    {
        suplaProvisioningDeadlineMs = 0;
        if((WiFi.getMode() & WIFI_AP) != 0)
        {
            WiFi.softAPdisconnect();
            WiFi.mode(WIFI_STA);
            BWC_LOG_P(PSTR("Soft AP > provisioning window expired\n"), 0);
        }
    }
    // We need this self-destructing info several times, so save it on the stack
    bool newData = bwc->newData();
    // Fiddle with the pump computer
    bwc->loop();

    // Give local control and recovery traffic priority while SUPLA is
    // downloading channel configuration after a fresh LittleFS image.
    if (server)
        server->handleClient();

    if (suplaBridge && !webUpdateInProgress)
    {
        // SuplaDevice's ESP8266 TLS client waits indefinitely for a valid
        // system clock before certificate validation. Keep the SPA, web UI and
        // OTA responsive while Wi-Fi/NTP are unavailable.
        if (WiFi.status() == WL_CONNECTED && hasValidUnixTime())
            suplaBridge->iterate();
        static uint32_t lastSuplaSyncMs = 0;
        if (newData || millis() - lastSuplaSyncMs >= 10000UL)
        {
            SpaSnapshot snapshot;
            if (bwc->getSnapshot(snapshot))
            {
                suplaBridge->sync(snapshot, lastSuplaSyncMs == 0);
                lastSuplaSyncMs = millis();
            }
        }
    }

    // listen for OTA events
    if(!webUpdateInProgress)
        ArduinoOTA.handle();
    // web socket
    if (webSocket && !webUpdateInProgress)
        webSocket->loop();
    if (!webUpdateInProgress && (newData || sendWSFlag))
    {
        sendWSFlag = false;
        sendWS();
    }
    // run only when a wifi connection is established
    /* MQTT, OTA & NTP is not relevant in softAP mode */
    if (WiFi.status() == WL_CONNECTED)
    {

        // MQTT
        if (!webUpdateInProgress && mqtt_info->useMqtt && mqttClient->loop())
        {
            String msg;
            msg.reserve(32);
            bwc->getButtonName(msg);
            // publish pretty button name if display button is pressed (or NOBTN if released)
            if (!msg.equals(prevButtonName))
            {
                mqttClient->publish((String(mqtt_info->mqttBaseTopic) + "/button").c_str(), String(msg).c_str(), true);
                prevButtonName = msg;
            }
            if (newData || sendMQTTFlag)
            {
                sendMQTT();
                sendMQTTFlag = false;
            }
            if (send_mqtt_cfg_needed)
            {
                send_mqtt_cfg_needed = false;
                sendMQTTConfig();
            }
        }

        if (checkNTP_flag)
        {
            checkNTP_flag = false;
            checkNTP();
        }
    }

    // run every X seconds
    if (periodicTimerFlag)
    {
        periodicTimerFlag = false;
        // The disconnect event normally retries immediately. This independent
        // 60-second safety net also recovers if the SDK ever loses that event
        // or abandons an association attempt during a longer router outage.
        // Never call WiFi.begin() while connected: doing that tears down a
        // healthy station link and needlessly disconnects SUPLA.
        if (WiFi.status() != WL_CONNECTED)
        {
            wifi_manual_reconnect();
        }
        if (!webUpdateInProgress && mqtt_info->useMqtt && !mqttClient->loop() &&
            (WiFi.status() == WL_CONNECTED))
        {
            BWC_LOG_P(PSTR("MQTT > Not connected\n"), 0);
            mqttConnect();
        }
        // Leverage the pre-existing periodicTimerFlag to also set temperature, if enabled
        setTemperatureFromSensor();
    }

    // Only do this if locked out! (by pressing POWER - LOCK - TIMER - POWER)
    if (bwc->getBtnSeqMatch())
    {
        resetWiFi();
        delay(3000);
        ESP.reset();
        delay(3000);
    }

    if(deviceRestartDeadlineMs != 0 &&
       static_cast<int32_t>(millis() - deviceRestartDeadlineMs) >= 0)
    {
        deviceRestartDeadlineMs = 0;
        if(factoryResetRequested)
        {
            factoryResetRequested = false;
            performFactoryReset();
            return;
        }
        stopall();
        delay(100);
        ESP.restart();
    }

    if(postFilesystemRestartPending && deviceRestartDeadlineMs == 0 &&
       static_cast<int32_t>(millis() - postFilesystemRestartEarliestMs) >= 0)
    {
        // The first boot recreates SUPLA's LittleFS runtime/channel cache. A
        // single clean restart then releases temporary TLS/config allocations
        // and starts the long-running process with a compact heap.
        postFilesystemRestartPending = false;
        scheduleDeviceRestart(true);
    }

    // A browser can disappear mid-request (sleep, roaming, closed tab). If the
    // HTTP stack does not reach its final callback, leave update mode and
    // restore all services automatically instead of waiting forever.
    if(webUpdateInProgress && deviceRestartDeadlineMs == 0 &&
       webUpdateLastActivityMs != 0 &&
       millis() - webUpdateLastActivityMs >= WEB_UPDATE_STALL_TIMEOUT_MS)
    {
        BWC_LOG_P(PSTR("Web update > stalled; restarting\n"), 0);
        scheduleDeviceRestart(true);
    }

}

/**
 * Send status data to web client in JSON format (because it is easy to decode on the other side)
 */
void sendWS()
{
    if (!webSocket)
        return;
    if (webSocket->connectedClients() == 0)
        return;
    HeapSelectIram ephemeral;
    // Serial.printf("IRamheap %d\n", ESP.getFreeHeap());
    // send states
    String json;
    json.reserve(512);

    bwc->getJSONStates(json);
    webSocket->broadcastTXT(json);
    // send times
    json.clear();
    bwc->getJSONTimes(json);
    webSocket->broadcastTXT(json);
    // send other info
    json.clear();
    getOtherInfo(json);
    webSocket->broadcastTXT(json);
    // json = bwc->getDebugData();
    // webSocket->broadcastTXT(json);
    // time_t now = time(nullptr);
    // struct tm timeinfo;
    // gmtime_r(&now, &timeinfo);
    // Serial.print("Current time: ");
    // Serial.print(asctime(&timeinfo));
    BWC_YIELD;
}

void getOtherInfo(String &rtn)
{
    DynamicJsonDocument doc(512);
    // Set the values in the document
    doc[F("CONTENT")] = F("OTHER");
    doc[F("MQTT")] = mqttClient->state();
    doc[F("SUPLA")] = currentSuplaStatus();
    /*TODO: add these:*/
    //   doc[F("PressedButton")] = bwc->getPressedButton();
    doc[F("HASJETS")] = bwc->hasjets;
    doc[F("HASGOD")] = bwc->hasgod;
    doc[F("MODEL")] = bwc->getModel();
    doc[F("RSSI")] = WiFi.RSSI();
    doc[F("IP")] = WiFi.localIP().toString();
    doc[F("SSID")] = WiFi.SSID();
    doc[F("FW")] = FW_VERSION;
    doc[F("loopfq")] = bwc->loop_count;
    bwc->loop_count = 0;

    // Serialize JSON to string
    if (doc.overflowed() || serializeJson(doc, rtn) == 0)
    {
        rtn = F("{\"error\": \"Failed to serialize other\"}");
    }
    BWC_YIELD;
}

/**
 * Send STATES and TIMES to MQTT
 * It would be more elegant to send both states and times on the "message" topic
 * and use the "CONTENT" field to distinguish between them
 * but it might break peoples home automation setups, so to keep it backwards
 * compatible I choose to start a new topic "/times"
 * @author 877dev
 */
void sendMQTT()
{
    HeapSelectIram ephemeral;
    // Serial.printf("IRamheap %d\n", ESP.getFreeHeap());
    String json;
    json.reserve(320);

    // send states
    bwc->getJSONStates(json);
    if (mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/message")).c_str(), String(json).c_str(), true))
    {
        BWC_LOG_P(PSTR("MQTT > message published\n"), 0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > message not published\n"), 0);
    }

    // send times
    json.clear();
    bwc->getJSONTimes(json);
    if (mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/times")).c_str(), String(json).c_str(), true))
    {
        BWC_LOG_P(PSTR("MQTT > times published\n"), 0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > times not published\n"), 0);
    }

    // send other info
    json.clear();
    getOtherInfo(json);
    if (mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/other")).c_str(), String(json).c_str(), true))
    {
        BWC_LOG_P(PSTR("MQTT > other published\n"), 0);
    }
    else
    {
        BWC_LOG_P(PSTR("MQTT > other not published\n"), 0);
    }
    BWC_YIELD;
}

void sendMQTTConfig()
{
    BWC_LOG_P(PSTR("MQTT > sending config\n"), 0);
    String json;
    json.reserve(320);
    bwc->getJSONSettings(json);
    mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/get_config")).c_str(), String(json).c_str(), true);
    mqttClient->loop();
    BWC_YIELD;
}

/**
 * Start a Wi-Fi access point, and try to connect to some given access points.
 * Then wait for either an AP or STA connection
 */
void startWiFi()
{
    BWC_LOG_P(PSTR("startWiFi() @ millis: %d\n"), millis());
    // WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.persistent(true);
    WiFi.hostname(DEVICE_NAME_F);
    WiFi.mode(WIFI_STA); // WiFi.setOutputPower(15.0);
    // RF tuning from wifi.json (web-UI editable, no rebuild needed).
    // Defaults: txPowerDbm=20.5 (max), noModemSleep=true, phyMode=auto.
    WiFi.setOutputPower(wifi_info->txPowerDbm);
    WiFi.setSleepMode(wifi_info->noModemSleep ? WIFI_NONE_SLEEP : WIFI_MODEM_SLEEP);
    switch (wifi_info->phyMode)
    {
        case 1: WiFi.setPhyMode(WIFI_PHY_MODE_11B); break; // most robust on weak links
        case 2: WiFi.setPhyMode(WIFI_PHY_MODE_11G); break;
        case 3: WiFi.setPhyMode(WIFI_PHY_MODE_11N); break;
        default: /* leave default (auto) */ break;
    }

    if (wifi_info->enableStaticIp4)
    {
        BWC_LOG_P(PSTR("Setting static IP\n"), 0);
        IPAddress ip4Address;
        IPAddress ip4Gateway;
        IPAddress ip4Subnet;
        IPAddress ip4DnsPrimary;
        IPAddress ip4DnsSecondary;
        ip4Address.fromString(wifi_info->ip4Address_str);
        ip4Gateway.fromString(wifi_info->ip4Gateway_str);
        ip4Subnet.fromString(wifi_info->ip4Subnet_str);
        ip4DnsPrimary.fromString(wifi_info->ip4DnsPrimary_str);
        ip4DnsSecondary.fromString(wifi_info->ip4DnsSecondary_str);
        BWC_LOG_P(PSTR("WiFi > using static IP %s on gateway %s\n"), ip4Address.toString().c_str(), ip4Gateway.toString().c_str());
        WiFi.config(ip4Address, ip4Gateway, ip4Subnet, ip4DnsPrimary, ip4DnsSecondary);
    }

    wifi_manual_reconnect();
    BWC_YIELD;
}

void wifi_manual_reconnect()
{
    /* Connect in station mode to the AP given (your router/ap) */
    if (wifi_info->enableAp)
    {
        WiFi.begin(wifi_info->apSsid.c_str(), wifi_info->apPwd.c_str());
        // checkWifi_ticker->attach(2.0, checkWiFi_ISR);
        BWC_LOG_P(PSTR("WiFi > AP info loaded. Waiting for connection ...\n"), 0);
    }
    else
    {
        BWC_LOG_P(PSTR("WiFi > AP info not found. Using last known AP ...\n"), 0);
        WiFi.begin();
    }
}

/**
 * start WiFiManager configuration portal
 */
void startSoftAp()
{
    disconnected_flag = false;
    if(!isSuplaProvisioningOpen())
    {
        BWC_LOG_P(PSTR("Soft AP > provisioning window is closed\n"), 0);
        return;
    }
    if (WiFi.getMode() == WIFI_AP_STA)
    {
        BWC_LOG_P(PSTR("Soft AP IP: %s.\n"), WiFi.softAPIP().toString().c_str());
        return;
    }
    BWC_LOG_P(PSTR("Station > disconnected. Starting soft AP\n"), 0);
    bwc->print(F("check network"));
    WiFi.mode(WIFI_AP_STA);
    IPAddress local_IP(192, 168, 4, 2);
    IPAddress gateway(192, 168, 4, 1);
    IPAddress subnet(255, 255, 255, 0);
    BWC_LOG_P(PSTR("WiFi > soft-AP configuration: %s\n"), WiFi.softAPConfig(local_IP, gateway, subnet) ? "OK" : "Failed!");
    BWC_LOG_P(PSTR("WiFi > soft AP mode: %s\n"),
              WiFi.softAP(WM_AP_NAME_F, String(WM_AP_PASSWORD_F).c_str()) ? "OK" : "SoftAP fail");
    BWC_LOG_P(PSTR("WiFi > Soft AP IP: %s\n"), WiFi.softAPIP().toString().c_str());
    BWC_YIELD;
}

void checkNTP_ISR()
{
    checkNTP_flag = true;
}

void checkNTP()
{
    time_t now = time(nullptr);
    // static uint8_t ntpTryNumber = 0;
    if (!hasValidUnixTime())
    {
        // if (++ntpTryNumber == 10) {
        //     ntpTryNumber = 0; //reset until next check
        //     ntpCheck_ticker->detach(); //give up. Next check won't happen.
        // }
        return;
    }
    ntpCheck_ticker->detach(); // time is set, don't check again
    struct tm timeinfo;
    gmtime_r(&now, &timeinfo);
    time_t boot_timestamp = getBootTime();
    tm *boot_time_tm = gmtime(&boot_timestamp);
    char boot_time_str[64];
    strftime(boot_time_str, 64, "%F %T", boot_time_tm);
    bwc->reboot_time_str = String(boot_time_str);
    bwc->reboot_time_t = boot_timestamp;
    if (firstNtpSyncAfterBoot)
    {
        BWC_LOG_P(PSTR("NTP > synced: %s. Saving boot info.\n"), bwc->reboot_time_str.c_str());
        if (mqtt_info->useMqtt && mqttClient->loop())
            mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/reboot_time")).c_str(), (bwc->reboot_time_str + 'Z').c_str(), true);
        firstNtpSyncAfterBoot = false;
        bwc->saveRebootInfo();
    }
    BWC_YIELD;
}

/**
 * start NTP sync
 */
void startNTP()
{
    BWC_LOG_P(PSTR("NTP > start\n"), 0);
    configTime(0, 0, wifi_info->ip4NTP_str, F("pool.ntp.org"), F("time.nist.gov"));
    ntpCheck_ticker->attach(3.0, checkNTP_ISR);
}

void startOTA()
{
    BWC_LOG_P(PSTR("OTA > start\n"), 0);
    String dname = DEVICE_NAME_F;
    ArduinoOTA.setHostname(dname.c_str());
    ArduinoOTA.setPassword(String(WM_AP_PASSWORD_F).c_str());

    ArduinoOTA.onStart([]()
                       {
        // Serial.println(F("OTA > Start"));
        stopall(); });
    ArduinoOTA.onEnd([]()
                     {
                         // Serial.println(F("OTA > End"));
                     });
    ArduinoOTA.onProgress([](unsigned int progress, unsigned int total)
                          {
                              // Serial.printf("OTA > Progress: %u%%\r\n", (progress / (total / 100)));
                          });
    ArduinoOTA.onError([](ota_error_t error)
                       {
                           // Serial.printf("OTA > Error[%u]: ", error);
                           // if (error == OTA_AUTH_ERROR) Serial.println(F("Auth Failed"));
                           // else if (error == OTA_BEGIN_ERROR) Serial.println(F("Begin Failed"));
                           // else if (error == OTA_CONNECT_ERROR) Serial.println(F("Connect Failed"));
                           // else if (error == OTA_RECEIVE_ERROR) Serial.println(F("Receive Failed"));
                           // else if (error == OTA_END_ERROR) Serial.println(F("End Failed"));
                       });
    ArduinoOTA.begin();
    // Serial.println(F("OTA > ready"));
}

void stopall()
{
    BWC_LOG_P(PSTR("Stop all > Free mem before stop: %d\n"), ESP.getFreeHeap());
    if (suplaBridge)
        suplaBridge->stop();
    bwc->stop();
    BWC_LOG_P(PSTR("MQTT > detaching\n"), 0);
    updateMqttTimer->detach();
    if (startComplete_ticker && startComplete_ticker->active())
        startComplete_ticker->detach();
    BWC_LOG_P(PSTR("Periodic timer > detaching\n"), 0);
    periodicTimer->detach();
    BWC_LOG_P(PSTR("WS > detaching\n"), 0);
    updateWSTimer->detach();
    if (ntpCheck_ticker->active())
        ntpCheck_ticker->detach();
    // if(checkWifi_ticker->active()) checkWifi_ticker->detach();
    // bwc->saveSettings();
    delete tempSensors;
    delete oneWire;
    BWC_LOG_P(PSTR("MQTT > stopping\n"), 0);
    if (mqtt_info->useMqtt)
        mqttClient->disconnect();
    if (aWifiClient)
        delete aWifiClient;
    aWifiClient = nullptr;
    // delete mqttClient; //Compiler nagging about not deleting virtual classes.
    mqttClient = nullptr;
    BWC_LOG_P(PSTR("HTTPServer > stopping\n"), 0);
    server->stop();
    delete server;
    server = nullptr;
    BWC_LOG_P(PSTR("WS > stopping\n"), 0);
    webSocket->close();
    delete webSocket;
    webSocket = nullptr;
    BWC_LOG_P(PSTR("FS > stopping\n"), 0);
    LittleFS.end();
    BWC_LOG_P(PSTR("Stop all > done. Free mem: %d\n"), ESP.getFreeHeap());
}

/*pause: action=true cont: action=false*/
void pause_all(bool action)
{
    if (action)
    {
        if (periodicTimer->active())
            periodicTimer->detach();
        // if(startComplete_ticker->active()) startComplete_ticker->detach();
        if (updateWSTimer->active())
            updateWSTimer->detach();
        // if(bootlogTimer->active()) bootlogTimer->detach();
        if (ntpCheck_ticker->active())
            ntpCheck_ticker->detach();
    }
    else
    {
        periodicTimer->attach(periodicTimerInterval, []
                              { periodicTimerFlag = true; });
        // startComplete_ticker->attach(60, []{ if(mqtt_info->useMqtt) enableMqtt = true; startComplete_ticker->detach(); });
        updateWSTimer->attach(WS_PERIOD, []
                              { sendWSFlag = true; });
        // bootlogTimer.attach(5, []{ if(DateTime.isTimeValid()) {bwc->saveRebootInfo(); bootlogTimer.detach();} });
    }
    bwc->pause_all(action);
}

void startWebSocket()
{
    HeapSelectIram ephemeral;
    BWC_LOG_P(PSTR("WS > start. IRam heap: %d\n"), ESP.getFreeHeap());
    if (webSocket != nullptr)
    {
        webSocket->disconnect();
        webSocket->close();
        delete webSocket;
        webSocket = nullptr;
    }
    webSocket = new WebSocketsServer(8080);
    webSocket->begin();
    // Remove half-open browser sessions so their TCP buffers cannot accumulate
    // across days of Wi-Fi roaming, laptop sleep and abrupt page closes.
    webSocket->enableHeartbeat(15000, 3000, 2);
    webSocket->onEvent(webSocketEvent);
    // Serial.println(F("WebSocket > server started"));
}

/**
 * handle web socket events
 */
void webSocketEvent(uint8_t num, WStype_t type, uint8_t *payload, size_t len)
{
    // When a WebSocket message is received
    switch (type)
    {
    // if the websocket is disconnected
    case WStype_DISCONNECTED:
        BWC_LOG_P(PSTR("WS > [%u] Disconnected!\n"), num);
        break;

    // if a new websocket connection is established
    case WStype_CONNECTED:
    {
        // IPAddress ip = webSocket->remoteIP(num);
        // Serial.printf("WebSocket > [%u] Connected from %d.%d.%d.%d url: %s\r\n", num, ip[0], ip[1], ip[2], ip[3], payload);
        // Defer the first snapshot until the connection callback has fully
        // unwound and the client list is stable. Periodic updates use the same
        // normal-loop path.
        sendWSFlag = true;
    }
    break;

    // if new text data is received
    case WStype_TEXT:
    {
        // Serial.printf("WebSocket > [%u] get Text: %s\r\n", num, payload);
        // DynamicJsonDocument doc(256);
        StaticJsonDocument<256> doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (error)
        {
            BWC_LOG_P(PSTR("WS > JSON command failed"), 0);
            return;
        }

        // Copy values from the JsonDocument to the Config
        Commands command = doc[F("CMD")];
        int64_t value = doc[F("VALUE")];
        int64_t xtime = doc[F("XTIME")];
        int64_t interval = doc[F("INTERVAL")];
        String txt = doc[F("TXT")] | "";
        command_que_item item;
        item.cmd = command;
        item.val = value;
        item.xtime = xtime;
        item.interval = interval;
        item.text = txt;
        bwc->add_command(item);
    }
    break;

    default:
        BWC_LOG_P(PSTR("WebSocket > Type:[%u]\r\n"), (unsigned int)type);
        break;
    }
}

/**
 * start a HTTP server with a file read and upload handler
 */
void startHttpServer()
{
    BWC_LOG_P(PSTR("HTTP Server > start/restart.\n"), 0);
    if (server != nullptr)
    {
        server->stop();
        server->close();
        delete server;
        server = nullptr;
    }

    {
        // HeapSelectIram ephemeral;
        server = new ESP8266WebServer(80);
        // Content-Type is not retained by ESP8266WebServer unless explicitly
        // collected. SUPLA's mutation endpoints require application/json.
        server->collectHeaders("Content-Type", "X-Update-Size");
        /* if you want a simple auth you can do something like this for every page you want to "protect" */
        // server->on(F("/"), []() {
        //     if (!server->authenticate("user", "pswd")) {
        //         return server->requestAuthentication();
        //     }
        //     handleNotFound();
        // });
        server->on(F("/getconfig/"), handleGetConfig);
        server->on(F("/setconfig/"), handleSetConfig);
        server->on(F("/getcommands/"), handleGetCommandQueue);
        server->on(F("/addcommand/"), handleAddCommand);
        server->on(F("/editcommand/"), handleEditCommand);
        server->on(F("/delcommand/"), handleDelCommand);
        server->on(F("/getwebconfig/"), handleGetWebConfig);
        server->on(F("/setwebconfig/"), handleSetWebConfig);
        server->on(F("/getwifi/"), handleGetWifi);
        server->on(F("/setwifi/"), handleSetWifi);
        server->on(F("/resetwifi/"), handleResetWifi);
        server->on(F("/getmqtt/"), handleGetMqtt);
        server->on(F("/setmqtt/"), handleSetMqtt);
        server->on(F("/getsupla/"), HTTP_GET, handleGetSupla);
        server->on(F("/setsupla/"), HTTP_POST, handleSetSupla);
        server->on(F("/resetsupla/"), HTTP_POST, handleResetSupla);
        server->on(F("/factory-reset/"), HTTP_POST, handleFactoryReset);
        server->on(F("/hwtest/"), handleHWtest);
        server->on(F("/inputs/"), handleInputs);
        server->on(F("/update.html"), HTTP_GET, handleWebUpdatePage);
        server->on(F("/update/status"), HTTP_GET, handleWebUpdateStatus);
        server->on(F("/update/firmware"), HTTP_POST, []()
                   { handleWebUpdateResult(WebUpdateTarget::FIRMWARE); }, []()
                   { handleWebUpdateUpload(WebUpdateTarget::FIRMWARE); });
        server->on(F("/update/filesystem"), HTTP_POST, []()
                   { handleWebUpdateResult(WebUpdateTarget::FILESYSTEM); }, []()
                   { handleWebUpdateUpload(WebUpdateTarget::FILESYSTEM); });
        server->on(F("/restart/"), handleRestart);
        server->on(F("/metrics"), handlePrometheusMetrics); // prometheus metrics
        server->on(F("/info/"), handleESPInfo);
        server->on(F("/health/"), HTTP_GET, handleHealth);
        server->on(F("/sethardware/"), handleSetHardware);
        server->on(F("/gethardware/"), handleGetHardware);
        server->on(F("/debug-on/"), []()
                   {bwc->BWC_DEBUG = true; server->send(200, F("text/plain"), "ok"); });
        server->on(F("/debug-off/"), []()
                   {bwc->BWC_DEBUG = false; server->send(200, F("text/plain"), "ok"); });
        server->on(F("/cmdq_file/"), handle_cmdq_file);

        // if someone requests any other file or page, go to function 'handleNotFound'
        // and check if the file exists
        server->onNotFound(handleNotFound);
        // start the HTTP server
        server->begin();
    }

    // Serial.println(F("HTTP > server started"));
}

void handleGetHardware()
{
    // if (!checkHttpPost(server->method()))
    //     return;
    // File file = LittleFS.open(F("hwcfg.json"), "r");

    sHW_config_eeprom_t hw_config_eeprom;
    EEPROM.begin(EEPROM_CONFIG_SIZE);
    EEPROM.get(HW_CONFIG_EEPROM_OFFSET, hw_config_eeprom);
    EEPROM.end();
    if (!isValidHardwareConfig(hw_config_eeprom))
    {
        hw_config_eeprom = makeDefaultHardwareConfig();
    }

    //json-ify the data for easier parsing on the client side (web UI)
    String json;
    json.reserve(512);
    json += "{";
    json += "\"cio\": \"" + String(hw_config_eeprom.cioNo) + "\",";
    json += "\"dsp\": \"" + String(hw_config_eeprom.dspNo) + "\",";
    json += "\"pcb\": \"" + String(hw_config_eeprom.pcbname) + "\",";
    json += "\"hasTempSensor\": ";
    json += (hw_config_eeprom.hasTempSensor ? true : false);
    json += ",";
    json += "\"pins\": [";
    for (int i = 0; i < 8; i++)
    {
        json += '\"';
        json += String(hw_config_eeprom.pins[i]);
        json += '\"';
        if (i < 7)
            json += ",";
    }
    json += "],";
    json += "\"pwr_levels\": {";
    json += "\"override\": ";
    json += hw_config_eeprom.power_levels_override ? true : false;
    json += ",";
    json += "\"heater_stage1\": \"" + String(hw_config_eeprom.power_levels.HEATERPOWER_STAGE1) + "\",";
    json += "\"heater_stage2\": \"" + String(hw_config_eeprom.power_levels.HEATERPOWER_STAGE2) + "\",";
    json += "\"pump\": \"" + String(hw_config_eeprom.power_levels.PUMPPOWER) + "\",";
    json += "\"idle\": \"" + String(hw_config_eeprom.power_levels.IDLEPOWER) + "\",";
    json += "\"air\": \"" + String(hw_config_eeprom.power_levels.AIRPOWER) + "\",";
    json += "\"jet\": \"" + String(hw_config_eeprom.power_levels.JETPOWER) + "\"";
    json += "}}";
    server->send(200, F("text/plain"), json);
    BWC_YIELD;

}

void handleSetHardware()
{
    if (!checkHttpPost(server->method()))
        return;
    String message = server->arg(0);

    // File file = LittleFS.open(F("hwcfg.json"), "w");
    // if (!file)
    // {
    //     // Serial.println(F("Failed to save hwcfg.json"));
    //     return;
    // }
    // file.print(message);
    // file.close();
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, message);
    if (error || !doc[F("cio")].is<int>() || !doc[F("dsp")].is<int>() ||
        !doc[F("pcb")].is<const char*>() || !doc[F("pins")].is<JsonArray>())
    {
        server->send(400, F("text/plain"), "Invalid JSON");
        return;
    }

    sHW_config_eeprom_t hw_config_eeprom;
    const String pcbName = doc[F("pcb")].as<String>();
    if(pcbName.isEmpty() || pcbName.length() >= sizeof(hw_config_eeprom.pcbname))
    {
        server->send(422, F("text/plain"), "Invalid PCB name");
        return;
    }

    hw_config_eeprom.cioNo = doc["cio"];
    hw_config_eeprom.dspNo = doc["dsp"];   
    memcpy(hw_config_eeprom.pcbname, pcbName.c_str(), pcbName.length() + 1);
    hw_config_eeprom.hasTempSensor = doc["hasTempSensor"];
    for (int i = 0; i < 8; i++)
    {
        hw_config_eeprom.pins[i] = doc["pins"][i];
    }
    hw_config_eeprom.power_levels_override = doc["pwr_levels"]["override"];
    hw_config_eeprom.power_levels.HEATERPOWER_STAGE1 = doc["pwr_levels"]["heater_stage1"];
    hw_config_eeprom.power_levels.HEATERPOWER_STAGE2 = doc["pwr_levels"]["heater_stage2"];
    hw_config_eeprom.power_levels.PUMPPOWER = doc["pwr_levels"]["pump"];
    hw_config_eeprom.power_levels.IDLEPOWER = doc["pwr_levels"]["idle"];
    hw_config_eeprom.power_levels.AIRPOWER = doc["pwr_levels"]["air"];
    hw_config_eeprom.power_levels.JETPOWER = doc["pwr_levels"]["jet"];

    if(!isValidHardwareConfig(hw_config_eeprom))
    {
        server->send(422, F("text/plain"), "Invalid or conflicting hardware pinout");
        return;
    }

    EEPROM.begin(EEPROM_CONFIG_SIZE);
    EEPROM.put(HW_CONFIG_EEPROM_OFFSET, hw_config_eeprom);
    const bool saved = EEPROM.commit();
    EEPROM.end();
    if(!saved)
    {
        server->send(500, F("text/plain"), "EEPROM write failed");
        return;
    }
    server->send(200, F("text/plain"), "ok");
    BWC_LOG_P(PSTR("HW config saved to EEPROM\n"), 0);
    BWC_YIELD;
}

void preparefortest()
{
    for (int i = 0; i < 7; i++)
    {
        pinMode(bwc->pins[i], INPUT);
    }
}

void handleInputs()
{
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "wait<br>");

    bwc->stop();
    preparefortest();

    /*
        Log all edges to a file in HEAP RAM. When that log is full send to web client
    */

    unsigned long pin_states = 0, old_pin_states = 0; // to store result from READ_PERI_REG (GPIOs)
    unsigned long t;                                  // timestamp - micros
    uint32_t edge_count = 0;
    const int array_len = 1024;
    unsigned long *p_input_log = new unsigned long[array_len * 2];

    while (edge_count < array_len)
    {
        pin_states = READ_PERI_REG(PIN_IN); // mix unsigned long with uint32_t which is the same
        if (pin_states != old_pin_states)
        {
            t = micros();
            p_input_log[edge_count] = t;                      // log time
            p_input_log[edge_count + array_len] = pin_states; // log states (all gpios)
            edge_count++;
        }
        old_pin_states = pin_states;
        yield(); // keep the watchdog away and manage wifi etc. Unclear how much time we waste here...
    }

    /* send statistics to client */
    char s[128];
    sprintf_P(s, PSTR("micros, gpio registers\n"));
    server->sendContent(s);
    for (int i = 0; i < array_len; i++)
    {
        sprintf_P(s, PSTR("%u,%X\n"), p_input_log[i], p_input_log[i + array_len]);
        server->sendContent(s);
        yield(); // keep the watchdog away and manage wifi etc. Unclear how much time we waste here...
    }
    sprintf_P(s, PSTR("Cut and paste all above. Zip and post on forum for help.\n"));
    server->sendContent(s);
    server->sendContent("");

    delete[] p_input_log;

    bwc->setup();
}

void handleHWtest()
{
    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    int errors = 0;
    bool state = false;
    char result[128];

    bwc->stop();
    preparefortest();

    for (int i = 0; i < 10; i++)
    {
        sprintf_P(result, PSTR("\nConnect the cables now!\nStarting test in %d seconds...\n"), 10 - i);
        server->sendContent(result);
        for (int t = 0; t < 512; t++)
            server->sendContent(" ");
        delay(1000);
    }

    /* First test CIO out/ DSP in ports */
    sprintf_P(result, PSTR("Start test. Seq begins with HIGH, then alters.\n\n"));
    server->sendContent(result);
    for (int pin = 0; pin < 3; pin++)
    {
        sprintf_P(result, PSTR("Sending on D%d, receiving on D%d\n"), gpio2dp(bwc->pins[pin]), gpio2dp(bwc->pins[pin + 3]));
        server->sendContent(result);
        pinMode(bwc->pins[pin], OUTPUT);
        pinMode(bwc->pins[pin + 3], INPUT);
        for (int t = 0; t < 100; t++)
        {
            state = !state;
            digitalWrite(bwc->pins[pin], state);
            delayMicroseconds(100);
            bool error = digitalRead(bwc->pins[pin + 3]) != state;
            errors += error;
            if (error)
                if (state)
                    server->sendContent("1");
                else
                    server->sendContent("0");
            else
                server->sendContent("-");
        }
        sprintf_P(result, PSTR(" // %d errors out of 100\n"), errors);
        server->sendContent(result);
        errors = 0;
        delay(0);
    }

    /* Test the other way around */

    for (int pin = 0; pin < 3; pin++)
    {
        sprintf_P(result, PSTR("Sending on D%d, receiving on D%d\n"), gpio2dp(bwc->pins[pin + 3]), gpio2dp(bwc->pins[pin]));
        server->sendContent(result);
        pinMode(bwc->pins[pin + 3], OUTPUT);
        pinMode(bwc->pins[pin], INPUT);
        for (int t = 0; t < 100; t++)
        {
            state = !state;
            digitalWrite(bwc->pins[pin + 3], state);
            delayMicroseconds(100);
            bool error = digitalRead(bwc->pins[pin]) != state;
            errors += error;
            if (error)
                if (state)
                    server->sendContent("1");
                else
                    server->sendContent("0");
            else
                server->sendContent("-");
        }
        sprintf_P(result, PSTR(" // %d errors out of 100\n"), errors);
        server->sendContent(result);
        errors = 0;
        delay(0);
    }

    sprintf_P(result, PSTR("End of test!\n\"1\" or \"0\" indicates ERROR, depending on test state. \"-\" is good.\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("Switching cio pins 5s HIGH -> 5s LOW -> input\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("then DSP pins 5s HIGH -> 5s LOW -> input, repeating\n"));
    server->sendContent(result);
    sprintf_P(result, PSTR("Disconnect cables then reset chip when done!\n"));
    server->sendContent(result);

    server->sendContent("");
    while (true)
    {
        /*CIO pins HIGH*/
        for (int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin + 3], INPUT);
            pinMode(bwc->pins[pin + 0], OUTPUT);
            digitalWrite(bwc->pins[pin], HIGH);
        }
        delay(5000);
        /*CIO pins LOW*/
        for (int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin + 3], INPUT);
            pinMode(bwc->pins[pin + 0], OUTPUT);
            digitalWrite(bwc->pins[pin], LOW);
        }
        delay(5000);
        /*DSP pins HIGH*/
        for (int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin + 0], INPUT);
            pinMode(bwc->pins[pin + 3], OUTPUT);
            digitalWrite(bwc->pins[pin + 3], HIGH);
        }
        delay(5000);
        /*DSP pins LOW*/
        for (int pin = 0; pin < 3; pin++)
        {
            pinMode(bwc->pins[pin + 0], INPUT);
            pinMode(bwc->pins[pin + 3], OUTPUT);
            digitalWrite(bwc->pins[pin + 3], LOW);
        }
        delay(5000);
    }
    bwc->setup();
}

void handleNotFound()
{
    // check if the file exists in the flash memory (LittleFS), if so, send it
    if (!handleFileRead(server->uri()))
    {
        server->send(404, F("text/plain"), F("404: File Not Found"));
    }
}

String getContentType(const String &filename)
{
    if (filename.endsWith(".html"))
        return F("text/html");
    else if (filename.endsWith(".css"))
        return F("text/css");
    else if (filename.endsWith(".js"))
        return F("application/javascript");
    else if (filename.endsWith(".ico"))
        return F("image/x-icon");
    else if (filename.endsWith(".gz"))
        return F("application/x-gzip");
    else if (filename.endsWith(".json"))
        return F("application/json");
    return F("text/plain");
}

/**
 * send the right file to the client (if it exists)
 */
bool handleFileRead(String path)
{
    pause_all(true);
    // Serial.println("HTTP > request: " + path);
    // If a folder is requested, send the index file
    if (path.endsWith("/"))
    {
        path += F("index.html");
    }
    // Deny reading credentials and SUPLA runtime storage, whether or not the
    // requested path currently exists.
    if (isProtectedStoragePath(path))
    {
        server->send(403, F("text/plain"), F("Permission denied."));
        // Serial.println(F("HTTP > file reading denied (credentials)."));
        pause_all(false);
        return true; // Request was handled; do not append a second 404 response.
    }
    String contentType = getContentType(path); // Get the MIME type
    String pathWithGz = path + ".gz";
    if (LittleFS.exists(pathWithGz) || LittleFS.exists(path))
    {                                         // If the file exists, either as a compressed archive, or normal
        if (LittleFS.exists(pathWithGz))      // If there's a compressed version available
            path += ".gz";                    // Use the compressed version
        File file = LittleFS.open(path, "r"); // Open the file
        size_t fsize = file.size();
        BWC_YIELD;
        size_t sent = server->streamFile(file, contentType); // Send it to the client
        BWC_LOG_P(PSTR("File size: %d\n"), fsize);
        BWC_LOG_P(PSTR("HTTPServer > Filename: %s. Bytes sent: %d\n"), path.c_str(), sent);
        if (fsize != sent)
        {
            BWC_LOG_P(PSTR("^^^^^ File not completed ^^^^^\n"), 0);
        }
        pause_all(false);
        file.close(); // Close the file again
        return true;
    }
    pause_all(false);
    // If the file doesn't exist, return false
    return false;
}

/**
 * checks the method to be a POST
 */
bool checkHttpPost(HTTPMethod method)
{
    if (method != HTTP_POST)
    {
        server->send(405, F("text/plain"), F("Method not allowed."));
        return false;
    }
    return true;
}

/**
 * response for /getconfig/
 * web server prints a json document
 */
void handleGetConfig()
{
    if (!checkHttpPost(server->method()))
        return;

    String json;
    json.reserve(320);
    bwc->getJSONSettings(json);
    server->send(200, F("text/plain"), json);
    BWC_YIELD;
}

/**
 * response for /setconfig/
 * web server writes a json document
 */
void handleSetConfig()
{
    if (!checkHttpPost(server->method()))
        return;

    String message = server->arg(0);
    bwc->setJSONSettings(message);

    server->send(200, F("text/plain"), "");
    send_mqtt_cfg_needed = true;
    BWC_YIELD;
}

/**
 * response for /getcommands/
 * web server prints a json document
 */
void handleGetCommandQueue()
{
    if (!checkHttpPost(server->method()))
        return;

    // A full queue can exceed the largest contiguous DRAM block after TLS is
    // connected. Count first, then stream through a small fixed buffer rather
    // than constructing a second copy of the complete JSON document.
    CountingPrint counter;
    if(!bwc->writeJSONCommandQueue(counter))
    {
        server->send(500, F("application/json"), F("{\"error\":\"Failed to size command queue\"}"));
        return;
    }

    server->setContentLength(counter.count());
    server->send(200, F("application/json"), "");
    BufferedPrint output(server->client());
    const bool written = bwc->writeJSONCommandQueue(output);
    if(!written || !output.finish())
        BWC_LOG_P(PSTR("HTTP > failed to stream command queue\n"), 0);
}

/**
 * response for /addcommand/
 * add a command to the queue
 */
void handleAddCommand()
{
    // if (!checkHttpPost(server->method())) return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message: ") + message);
        return;
    }

    Commands command = doc[F("CMD")];
    int64_t value = doc[F("VALUE")];
    int64_t xtime = doc[F("XTIME")];
    int64_t interval = doc[F("INTERVAL")];
    String txt = doc[F("TXT")] | "";
    command_que_item item;
    item.cmd = command;
    item.val = value;
    item.xtime = xtime;
    item.interval = interval;
    item.text = txt;
    bwc->add_command(item);

    server->send(200, F("text/plain"), F("ok"));
}

/**
 * response for /editcommand/
 * replace a command in the queue with new command
 */
void handleEditCommand()
{
    if (!checkHttpPost(server->method()))
        return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    Commands command = doc[F("CMD")];
    int64_t value = doc[F("VALUE")];
    int64_t xtime = doc[F("XTIME")];
    int64_t interval = doc[F("INTERVAL")];
    String txt = doc[F("TXT")] | "";
    uint8_t index = doc[F("IDX")];
    command_que_item item;
    item.cmd = command;
    item.val = value;
    item.xtime = xtime;
    item.interval = interval;
    item.text = txt;
    bwc->edit_command(index, item);

    server->send(200, F("text/plain"), "");
}

/**
 * response for /delcommand/
 * replace a command in the queue with new command
 */
void handleDelCommand()
{
    if (!checkHttpPost(server->method()))
        return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    uint8_t index = doc[F("IDX")];
    bwc->del_command(index);
    server->send(200, F("text/plain"), "");
}

void handle_cmdq_file()
{
    if (!checkHttpPost(server->method()))
        return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    if(!doc[F("ACT")].is<const char *>() || !doc[F("NAME")].is<const char *>())
    {
        server->send(400, F("text/plain"), F("Invalid command queue file request"));
        return;
    }

    String action = doc[F("ACT")].as<String>();
    String filename = "/";
    filename += doc[F("NAME")].as<String>();

    if((!action.equals(F("load")) && !action.equals(F("save"))) ||
       filename.length() < 2 || filename.length() > 65)
    {
        server->send(422, F("text/plain"), F("Invalid action or filename"));
        return;
    }
    if(isProtectedMutationPath(filename))
    {
        server->send(403, F("text/plain"), F("Permission denied."));
        return;
    }

    if (action.equals("load"))
    {
        copyFile("/cmdq.json", "/cmdq.backup");
        if(!copyFile(filename, "/cmdq.json"))
        {
            server->send(404, F("text/plain"), F("Command queue file not found"));
            return;
        }
        bwc->reloadCommandQueue();
    }
    else
    {
        if(!copyFile("/cmdq.json", filename))
        {
            server->send(500, F("text/plain"), F("Unable to save command queue file"));
            return;
        }
    }

    server->send(200, F("text/plain"), "");
    BWC_YIELD;
}

bool copyFile(const String &source, const String &dest)
{
    // Keep this guard in the shared helper as well as in the HTTP handler so a
    // future caller cannot copy credentials to a public alias or overwrite
    // SUPLA runtime storage accidentally.
    if(isProtectedMutationPath(source) || isProtectedMutationPath(dest))
        return false;

    char ibuffer[64]; // declare a buffer

    File f_source = LittleFS.open(source, "r"); // open source file to read
    if (!f_source)
    {
        return false;
    }

    File f_dest = LittleFS.open(dest, "w"); // open destination file to write
    if (!f_dest)
    {
        f_source.close();
        return false;
    }

    bool copied = true;
    while (f_source.available() > 0)
    {
        byte i = f_source.readBytes(ibuffer, 64); // i = number of bytes placed in buffer from file f_source
        if(f_dest.write(ibuffer, i) != i)
        {
            copied = false;
            break;
        }
    }

    f_dest.close();   // done, close the destination file
    f_source.close(); // done, close the source file
    BWC_YIELD;
    return copied;
}

/**
 * load "Web Config" json configuration from "webconfig.json"
 */
void loadWebConfig()
{
    // DynamicJsonDocument doc(1024);
    StaticJsonDocument<256> doc;

    File file = LittleFS.open(F("/webconfig.json"), "r");
    if (file)
    {
        DeserializationError error = deserializeJson(doc, file);
        if (error)
        {
            // Serial.println(F("Failed to deserialize webconfig.json"));
            file.close();
            return;
        }
    }
    else
    {
        // Serial.println(F("Failed to read webconfig.json. Using defaults."));
    }

    showSectionTemperature = (doc.containsKey(F("SST")) ? doc[F("SST")] : true);
    showSectionDisplay = (doc.containsKey(F("SSD")) ? doc[F("SSD")] : true);
    showSectionControl = (doc.containsKey(F("SSC")) ? doc[F("SSC")] : true);
    showSectionButtons = (doc.containsKey(F("SSB")) ? doc[F("SSB")] : true);
    showSectionTimer = (doc.containsKey(F("SSTIM")) ? doc[F("SSTIM")] : true);
    showSectionTotals = (doc.containsKey(F("SSTOT")) ? doc[F("SSTOT")] : true);
    useControlSelector = (doc.containsKey(F("UCS")) ? doc[F("UCS")] : false);
    BWC_YIELD;
}

/**
 * save "Web Config" json configuration to "webconfig.json"
 */
void saveWebConfig()
{
    File file = LittleFS.open(F("/webconfig.json"), "w");
    if (!file)
    {
        // Serial.println(F("Failed to save webconfig.json"));
        return;
    }

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;

    doc[F("SST")] = showSectionTemperature;
    doc[F("SSD")] = showSectionDisplay;
    doc[F("SSC")] = showSectionControl;
    doc[F("SSB")] = showSectionButtons;
    doc[F("SSTIM")] = showSectionTimer;
    doc[F("SSTOT")] = showSectionTotals;
    doc[F("UCS")] = useControlSelector;

    if (serializeJson(doc, file) == 0)
    {
        // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    }
    file.close();
    BWC_YIELD;
}

/**
 * response for /getwebconfig/
 * web server prints a json document
 */
void handleGetWebConfig()
{
    if (!checkHttpPost(server->method()))
        return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;

    doc[F("SST")] = showSectionTemperature;
    doc[F("SSD")] = showSectionDisplay;
    doc[F("SSC")] = showSectionControl;
    doc[F("SSB")] = showSectionButtons;
    doc[F("SSTIM")] = showSectionTimer;
    doc[F("SSTOT")] = showSectionTotals;
    doc[F("UCS")] = useControlSelector;

    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize webcfg\"}");
    }
    server->send(200, F("application/json"), json);
}

/**
 * response for /setwebconfig/
 * web server writes a json document
 */
void handleSetWebConfig()
{
    if (!checkHttpPost(server->method()))
        return;

    // DynamicJsonDocument doc(256);
    StaticJsonDocument<256> doc;
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    showSectionTemperature = doc[F("SST")];
    showSectionDisplay = doc[F("SSD")];
    showSectionControl = doc[F("SSC")];
    showSectionButtons = doc[F("SSB")];
    showSectionTimer = doc[F("SSTIM")];
    showSectionTotals = doc[F("SSTOT")];
    useControlSelector = doc[F("UCS")];

    saveWebConfig();

    server->send(200, F("text/plain"), "");
}

/**
 * load WiFi json configuration from "wifi.json"
 */
void loadWifi()
{
    // File file = LittleFS.open(F("/wifi.json"), "r");
    // if (!file)
    // {
    //     // Serial.println(F("Failed to read wifi.json. Using defaults."));
    //     return;
    // }

    // DynamicJsonDocument doc(1024);

    // DeserializationError error = deserializeJson(doc, file);
    // if (error)
    // {
    //     // Serial.println(F("Failed to deserialize wifi.json"));
    //     file.close();
    //     return;
    // }

    // wifi_info->enableAp = doc[F("enableAp")];
    // if (doc.containsKey(F("enableWM")))
    //     wifi_info->enableWmApFallback = doc[F("enableWM")];
    // wifi_info->apSsid = doc[F("apSsid")].as<String>();
    // wifi_info->apPwd = doc[F("apPwd")].as<String>();

    // wifi_info->enableStaticIp4 = doc[F("enableStaticIp4")];
    // String s(30);
    // wifi_info->ip4Address_str = doc[F("ip4Address")].as<String>();
    // wifi_info->ip4Gateway_str = doc[F("ip4Gateway")].as<String>();
    // wifi_info->ip4Subnet_str = doc[F("ip4Subnet")].as<String>();
    // wifi_info->ip4DnsPrimary_str = doc[F("ip4DnsPrimary")].as<String>();
    // wifi_info->ip4DnsSecondary_str = doc[F("ip4DnsSecondary")].as<String>();
    // wifi_info->ip4NTP_str = doc[F("ip4NTP")].as<String>();

    // // RF tuning fields — only override defaults if present, so older
    // // wifi.json files (no RF section yet) keep working with defaults.
    // if (doc.containsKey(F("txPowerDbm")))   wifi_info->txPowerDbm   = doc[F("txPowerDbm")].as<float>();
    // if (doc.containsKey(F("noModemSleep"))) wifi_info->noModemSleep = doc[F("noModemSleep")].as<bool>();
    // if (doc.containsKey(F("phyMode")))      wifi_info->phyMode      = doc[F("phyMode")].as<int>();

    wifiConfigurationLoaded = false;
    BWC_YIELD;
    EEPROM.begin(EEPROM_CONFIG_SIZE); // Cover every persisted config block before committing any of them.

    sWifi_info_eeprom_t wifi_info_eeprom;
    EEPROM.get(WIFI_CONFIG_EEPROM_OFFSET, wifi_info_eeprom);
    EEPROM.end();
    if(*(uint32_t*)(wifi_info_eeprom.tag) != 0x49464957) { // "WIFI" in hex (backwards), used to check if the eeprom contains valid wifi config
        BWC_LOG_P(PSTR("No valid wifi config in EEPROM. Using defaults.\n"),0);
        return;
    }   
    wifiConfigurationLoaded = true;
    wifi_info->apSsid = String(wifi_info_eeprom.apSsid);
    wifi_info->apPwd = String(wifi_info_eeprom.apPwd);
    BWC_LOG_P(PSTR("Loaded WiFi config: %s\n"), wifi_info->apSsid.c_str());
    IPAddress myip;
    myip = wifi_info_eeprom.ip4Address;
    wifi_info->ip4Address_str = myip.toString();
    myip = wifi_info_eeprom.ip4Gateway;
    wifi_info->ip4Gateway_str = myip.toString();
    myip = wifi_info_eeprom.ip4Subnet;
    wifi_info->ip4Subnet_str = myip.toString();
    myip = wifi_info_eeprom.ip4DnsPrimary;
    wifi_info->ip4DnsPrimary_str = myip.toString();
    myip = wifi_info_eeprom.ip4DnsSecondary;
    wifi_info->ip4DnsSecondary_str = myip.toString();
    myip = wifi_info_eeprom.ip4NTP;
    wifi_info->ip4NTP_str = myip.toString();
    wifi_info->enableAp = wifi_info_eeprom.enableAp;
    wifi_info->enableWmApFallback = wifi_info_eeprom.enableWmApFallback;
    wifi_info->enableStaticIp4 = wifi_info_eeprom.enableStaticIp4;
    wifi_info->txPowerDbm = wifi_info_eeprom.txPowerDbm;
    wifi_info->noModemSleep = wifi_info_eeprom.noModemSleep;
    wifi_info->phyMode = wifi_info_eeprom.phyMode;
}

/**
 * save WiFi json configuration to EEPROM
 */
bool saveWifi()
{
    // File file = LittleFS.open(F("/wifi.json"), "w");
    // if (!file)
    // {
    //     // Serial.println(F("Failed to save wifi.json"));
    //     return;
    // }

    // DynamicJsonDocument doc(1024);

    // doc[F("enableAp")] = wifi_info->enableAp;
    // doc[F("enableWM")] = wifi_info->enableWmApFallback;
    // doc[F("apSsid")] = wifi_info->apSsid;
    // doc[F("apPwd")] = wifi_info->apPwd;
    // doc[F("enableStaticIp4")] = wifi_info->enableStaticIp4;
    // doc[F("ip4Address")] = wifi_info->ip4Address_str;
    // doc[F("ip4Gateway")] = wifi_info->ip4Gateway_str;
    // doc[F("ip4Subnet")] = wifi_info->ip4Subnet_str;
    // doc[F("ip4DnsPrimary")] = wifi_info->ip4DnsPrimary_str;
    // doc[F("ip4DnsSecondary")] = wifi_info->ip4DnsSecondary_str;
    // doc[F("ip4NTP")] = wifi_info->ip4NTP_str;
    // doc[F("txPowerDbm")]   = wifi_info->txPowerDbm;
    // doc[F("noModemSleep")] = wifi_info->noModemSleep;
    // doc[F("phyMode")]      = wifi_info->phyMode;

    // if (serializeJson(doc, file) == 0)
    // {
    //     // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    // }
    // file.close();
    BWC_YIELD;
    EEPROM.begin(EEPROM_CONFIG_SIZE); // Cover every persisted config block before committing any of them.

    sWifi_info_eeprom_t wifi_info_eeprom;
    BWC_LOG_P(PSTR("Size of eeprom: %d\n"), EEPROM_CONFIG_SIZE);
    if(wifi_info->apSsid.length() >= sizeof(wifi_info_eeprom.apSsid) || wifi_info->apPwd.length() >= sizeof(wifi_info_eeprom.apPwd)) {
        BWC_LOG_P(PSTR("WiFi SSID or password too long to save to EEPROM\n"),0);
        EEPROM.end();
        return false;
    }
    strcpy(wifi_info_eeprom.apSsid, wifi_info->apSsid.c_str());
    strcpy(wifi_info_eeprom.apPwd, wifi_info->apPwd.c_str());
    IPAddress myip;
    myip.fromString(wifi_info->ip4Address_str);
    wifi_info_eeprom.ip4Address = (uint32_t)myip;
    myip.fromString(wifi_info->ip4Gateway_str);
    wifi_info_eeprom.ip4Gateway = (uint32_t)myip;
    myip.fromString(wifi_info->ip4Subnet_str);
    wifi_info_eeprom.ip4Subnet = (uint32_t)myip;
    myip.fromString(wifi_info->ip4DnsPrimary_str);
    wifi_info_eeprom.ip4DnsPrimary = (uint32_t)myip;
    myip.fromString(wifi_info->ip4DnsSecondary_str);
    wifi_info_eeprom.ip4DnsSecondary = (uint32_t)myip;
    myip.fromString(wifi_info->ip4NTP_str);
    wifi_info_eeprom.ip4NTP = (uint32_t)myip;
    wifi_info_eeprom.enableAp = wifi_info->enableAp;
    wifi_info_eeprom.enableWmApFallback = wifi_info->enableWmApFallback;
    wifi_info_eeprom.enableStaticIp4 = wifi_info->enableStaticIp4;
    wifi_info_eeprom.txPowerDbm = wifi_info->txPowerDbm;
    wifi_info_eeprom.noModemSleep = wifi_info->noModemSleep;
    wifi_info_eeprom.phyMode = wifi_info->phyMode;
    EEPROM.put(WIFI_CONFIG_EEPROM_OFFSET, wifi_info_eeprom);
    wifiConfigurationLoaded = EEPROM.commit();
    if(!wifiConfigurationLoaded)
        BWC_LOG_P(PSTR("WiFi config EEPROM commit failed\n"), 0);
    EEPROM.end();
    return wifiConfigurationLoaded;
}

/**
 * response for /getwifi/
 * web server prints a json document
 */
void handleGetWifi()
{
    if (!checkHttpPost(server->method()))
        return;

    DynamicJsonDocument doc(1024);

    doc[F("enableAp")] = wifi_info->enableAp;
    doc[F("enableWM")] = wifi_info->enableWmApFallback;
    doc[F("apSsid")] = wifi_info->apSsid;
    doc[F("apPwd")] = F("<enter password>");
    if (!hidePasswords)
    {
        doc[F("apPwd")] = wifi_info->apPwd;
    }

    doc[F("enableStaticIp4")] = wifi_info->enableStaticIp4;
    doc[F("ip4Address")] = wifi_info->ip4Address_str;
    doc[F("ip4Gateway")] = wifi_info->ip4Gateway_str;
    doc[F("ip4Subnet")] = wifi_info->ip4Subnet_str;
    doc[F("ip4DnsPrimary")] = wifi_info->ip4DnsPrimary_str;
    doc[F("ip4DnsSecondary")] = wifi_info->ip4DnsSecondary_str;
    doc[F("ip4NTP")] = wifi_info->ip4NTP_str;
    doc[F("txPowerDbm")]   = wifi_info->txPowerDbm;
    doc[F("noModemSleep")] = wifi_info->noModemSleep;
    doc[F("phyMode")]      = wifi_info->phyMode;
    String json;
    json.reserve(280);
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("application/json"), json);
}

/**
 * response for /setwifi/
 * web server writes a json document
 */
void handleSetWifi()
{
    if (!checkHttpPost(server->method()))
        return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }
    // Never print the request body: it contains the home-network password.
    wifi_info->enableAp = doc[F("enableAp")];
    if (doc.containsKey("enableWM"))
        wifi_info->enableWmApFallback = doc[F("enableWM")];
    wifi_info->apSsid = doc[F("apSsid")].as<String>();
    wifi_info->apPwd = doc[F("apPwd")].as<String>();

    wifi_info->enableStaticIp4 = doc[F("enableStaticIp4")];
    wifi_info->ip4Address_str = doc[F("ip4Address")].as<String>();
    wifi_info->ip4Gateway_str = doc[F("ip4Gateway")].as<String>();
    wifi_info->ip4Subnet_str = doc[F("ip4Subnet")].as<String>();
    wifi_info->ip4DnsPrimary_str = doc[F("ip4DnsPrimary")].as<String>();
    wifi_info->ip4DnsSecondary_str = doc[F("ip4DnsSecondary")].as<String>();
    wifi_info->ip4NTP_str = doc[F("ip4NTP")].as<String>();
    if (doc.containsKey(F("txPowerDbm")))   wifi_info->txPowerDbm   = doc[F("txPowerDbm")].as<float>();
    if (doc.containsKey(F("noModemSleep"))) wifi_info->noModemSleep = doc[F("noModemSleep")].as<bool>();
    if (doc.containsKey(F("phyMode")))      wifi_info->phyMode      = doc[F("phyMode")].as<int>();
    if(!saveWifi())
    {
        server->send(500, F("application/json"), F("{\"error\":\"EEPROM write failed\"}"));
        return;
    }

    server->send(200,
                 F("application/json"),
                 F("{\"saved\":true,\"restarting\":true}"));
    scheduleDeviceRestart(true);
}

/*
 * response for /resetwifi/
 * do this before giving away the device (be aware of other credentials e.g. MQTT)
 * a complete flash erase should do the job but remember to upload the filesystem as well.
 */
void handleResetWifi()
{
    server->send(200, F("text/html"), F("WiFi connection reset (erase) ..."));
    // Serial.println(F("WiFi connection reset (erase) ..."));
    resetWiFi();

    server->send(200, F("text/html"), F("WiFi connection reset (erase) ... done."));
// Serial.println(F("WiFi connection reset (erase) ... done."));
// Serial.println(F("ESP reset ..."));
#if defined(ESP8266)
    ESP.reset();
#else
    ESP.restart();
#endif
}

void resetWiFi()
{
    wifi_info->enableAp = false;
    wifi_info->enableWmApFallback = true;
    wifi_info->apSsid = F("empty");
    wifi_info->apPwd = F("empty");
    saveWifi();
    delay(3000);
    periodicTimer->detach();
    updateMqttTimer->detach();
    updateWSTimer->detach();
    if (ntpCheck_ticker->active())
        ntpCheck_ticker->detach();
    bwc->saveSettings();
    bwc->stop();
    delay(1000);
#if defined(ESP8266)
    ESP.eraseConfig();
#endif
    delay(1000);
    // ESP_WiFiManager wm;
    // wm.resetSettings();
    // WiFi.disconnect();
    delay(1000);
}

/**
 * load MQTT json configuration from "mqtt.json"
 */
void loadMqtt()
{
    // File file = LittleFS.open("mqtt.json", "r");
    // if (!file)
    // {
    //     BWC_LOG_P(PSTR("MQTT > Failed to read mqtt.json. Using defaults.\n"), 0);
    //     return;
    // }

    // DynamicJsonDocument doc(1024);

    // DeserializationError error = deserializeJson(doc, file);
    // if (error)
    // {
    //     // Serial.println(F("Failed to deserialize mqtt.json."));
    //     file.close();
    //     return;
    // }

    // mqtt_info->useMqtt = doc[F("enableMqtt")];
    // mqtt_info->mqttHost = doc[F("mqttHost")].as<String>();
    // mqtt_info->mqttPort = doc[F("mqttPort")];
    // mqtt_info->mqttUsername = doc[F("mqttUsername")].as<String>();
    // mqtt_info->mqttPassword = doc[F("mqttPassword")].as<String>();
    // mqtt_info->mqttClientId = doc[F("mqttClientId")].as<String>();
    // mqtt_info->mqttBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    // mqtt_info->mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];
    BWC_YIELD;
    //TODO: move this to the function that uses it. Don't forget to end eeprom. It will reserve memory always otherwise, even if not used.
    EEPROM.begin(EEPROM_CONFIG_SIZE); // Cover every persisted config block before committing any of them.
    sMQTT_info_eeprom_t mqtt_info_eeprom;
    EEPROM.get(MQTT_CONFIG_EEPROM_OFFSET, mqtt_info_eeprom);
    EEPROM.end();
    if(*(uint32_t*)(mqtt_info_eeprom.tag) != 0x5454514D) { // "MQTT" in hex (backwards), used to check if the eeprom contains valid mqtt config
        BWC_LOG_P(PSTR("No valid MQTT config in EEPROM. Using defaults.\n"), 0);
        return;
    }
    mqtt_info->mqttHost = mqtt_info_eeprom.mqttHost;
    mqtt_info->mqttPort = mqtt_info_eeprom.mqttPort;
    mqtt_info->mqttUsername = mqtt_info_eeprom.mqttUsername;
    mqtt_info->mqttPassword = mqtt_info_eeprom.mqttPassword;
    mqtt_info->mqttClientId = mqtt_info_eeprom.mqttClientId;
    mqtt_info->mqttBaseTopic = mqtt_info_eeprom.mqttBaseTopic;
    mqtt_info->mqttTelemetryInterval = mqtt_info_eeprom.mqttTelemetryInterval;
    mqtt_info->useMqtt = mqtt_info_eeprom.useMqtt;
}

/**
 * save MQTT json configuration to "mqtt.json"
 */
void saveMqtt()
{
    // File file = LittleFS.open("mqtt.json", "w");
    // if (!file)
    // {
    //     // Serial.println(F("Failed to save mqtt.json"));
    //     return;
    // }

    // DynamicJsonDocument doc(1024);

    // doc[F("enableMqtt")] = mqtt_info->useMqtt;
    // doc[F("mqttHost")] = mqtt_info->mqttHost;
    // doc[F("mqttPort")] = mqtt_info->mqttPort;
    // doc[F("mqttUsername")] = mqtt_info->mqttUsername;
    // doc[F("mqttPassword")] = mqtt_info->mqttPassword;
    // doc[F("mqttClientId")] = mqtt_info->mqttClientId;
    // doc[F("mqttBaseTopic")] = mqtt_info->mqttBaseTopic;
    // doc[F("mqttTelemetryInterval")] = mqtt_info->mqttTelemetryInterval;

    // if (serializeJson(doc, file) == 0)
    // {
    //     // Serial.println(F("{\"error\": \"Failed to serialize file\"}"));
    // }
    // file.close();
    BWC_YIELD;
        //TODO: move this to the function that uses it. Don't forget to end eeprom. It will reserve memory always otherwise, even if not used.
    EEPROM.begin(EEPROM_CONFIG_SIZE); // Cover every persisted config block before committing any of them.
    sMQTT_info_eeprom_t mqtt_info_eeprom;
    BWC_LOG_P(PSTR("Size of mqtt_info_eeprom: %d\n"), sizeof(mqtt_info_eeprom));  
    if(mqtt_info->mqttHost.length() >= sizeof(mqtt_info_eeprom.mqttHost) || mqtt_info->mqttUsername.length() >= sizeof(mqtt_info_eeprom.mqttUsername) || mqtt_info->mqttPassword.length() >= sizeof(mqtt_info_eeprom.mqttPassword) || mqtt_info->mqttClientId.length() >= sizeof(mqtt_info_eeprom.mqttClientId) || mqtt_info->mqttBaseTopic.length() >= sizeof(mqtt_info_eeprom.mqttBaseTopic)) {
        BWC_LOG_P(PSTR("MQTT config fields too long to save to EEPROM"),0);
        EEPROM.end();
        return;
    }
    strcpy(mqtt_info_eeprom.mqttHost, mqtt_info->mqttHost.c_str());
    BWC_LOG_P(PSTR("Saving MQTT Host: %s\n"), mqtt_info->mqttHost.c_str());
    mqtt_info_eeprom.mqttTelemetryInterval = mqtt_info->mqttTelemetryInterval;
    mqtt_info_eeprom.mqttPort = mqtt_info->mqttPort;
    strcpy(mqtt_info_eeprom.mqttUsername, mqtt_info->mqttUsername.c_str());
    strcpy(mqtt_info_eeprom.mqttPassword, mqtt_info->mqttPassword.c_str());
    strcpy(mqtt_info_eeprom.mqttClientId, mqtt_info->mqttClientId.c_str());
    strcpy(mqtt_info_eeprom.mqttBaseTopic, mqtt_info->mqttBaseTopic.c_str());
    mqtt_info_eeprom.useMqtt = mqtt_info->useMqtt;
    EEPROM.put(MQTT_CONFIG_EEPROM_OFFSET, mqtt_info_eeprom);
    EEPROM.commit();
    EEPROM.end();
}

/**
 * response for /getmqtt/
 * web server prints a json document
 */
void handleGetMqtt()
{
    if (!checkHttpPost(server->method()))
        return;

    DynamicJsonDocument doc(1024);

    doc[F("enableMqtt")] = mqtt_info->useMqtt;
    doc[F("mqttHost")] = mqtt_info->mqttHost;
    doc[F("mqttPort")] = mqtt_info->mqttPort;
    doc[F("mqttUsername")] = mqtt_info->mqttUsername;
    doc[F("mqttPassword")] = "<enter password>";
    if (!hidePasswords)
    {
        doc[F("mqttPassword")] = mqtt_info->mqttPassword;
    }
    doc[F("mqttClientId")] = mqtt_info->mqttClientId;
    doc[F("mqttBaseTopic")] = mqtt_info->mqttBaseTopic;
    doc[F("mqttTelemetryInterval")] = mqtt_info->mqttTelemetryInterval;

    String json;
    if (serializeJson(doc, json) == 0)
    {
        json = F("{\"error\": \"Failed to serialize message\"}");
    }
    server->send(200, F("text/plain"), json);
    BWC_YIELD;
}

/**
 * response for /setmqtt/
 * web server writes a json document
 */
void handleSetMqtt()
{
    if (!checkHttpPost(server->method()))
        return;

    DynamicJsonDocument doc(1024);
    String message = server->arg(0);
    DeserializationError error = deserializeJson(doc, message);
    if (error)
    {
        // Serial.println(F("Failed to read config file"));
        server->send(400, F("text/plain"), F("Error deserializing message"));
        return;
    }

    mqtt_info->useMqtt = doc[F("enableMqtt")];
    mqtt_info->mqttHost = doc[F("mqttHost")].as<String>();
    mqtt_info->mqttPort = doc[F("mqttPort")];
    mqtt_info->mqttUsername = doc[F("mqttUsername")].as<String>();
    mqtt_info->mqttPassword = doc[F("mqttPassword")].as<String>();
    mqtt_info->mqttClientId = doc[F("mqttClientId")].as<String>();
    mqtt_info->mqttBaseTopic = doc[F("mqttBaseTopic")].as<String>();
    mqtt_info->mqttTelemetryInterval = doc[F("mqttTelemetryInterval")];

    server->send(200, F("text/plain"), "");

    saveMqtt();
    startMqtt();
    BWC_YIELD;
}

/** Load the versioned SUPLA identity and account assignment from EEPROM. */
void loadSupla()
{
    supla_info->server = "";
    supla_info->email = "";
    supla_info->setupPassword = String(WM_AP_PASSWORD_F);
    supla_info->enabled = false;
    supla_info->configured = false;
    supla_info->setupPasswordConfigured = true;
    memset(supla_info->guid, 0, sizeof(supla_info->guid));
    memset(supla_info->authKey, 0, sizeof(supla_info->authKey));

    sSupla_info_eeprom_t stored;
    EEPROM.begin(EEPROM_CONFIG_SIZE);
    EEPROM.get(SUPLA_CONFIG_EEPROM_OFFSET, stored);
    EEPROM.end();

    const bool stringsTerminated =
        memchr(stored.server, '\0', sizeof(stored.server)) != nullptr &&
        memchr(stored.email, '\0', sizeof(stored.email)) != nullptr &&
        memchr(stored.setupPassword, '\0', sizeof(stored.setupPassword)) != nullptr;
    const bool valid = memcmp(stored.tag, "SUPL", sizeof(stored.tag)) == 0 &&
                       stored.version == SUPLA_CONFIG_VERSION &&
                       stringsTerminated &&
                       stored.checksum == suplaConfigChecksum(stored);
    if (!valid)
    {
        BWC_LOG_P(PSTR("SUPLA > no valid configuration in EEPROM\n"), 0);
        return;
    }

    supla_info->server = stored.server;
    supla_info->email = stored.email;
    supla_info->enabled = stored.enabled;
    memcpy(supla_info->guid, stored.guid, sizeof(supla_info->guid));
    memcpy(supla_info->authKey, stored.authKey, sizeof(supla_info->authKey));
    supla_info->configured = !supla_info->email.isEmpty() &&
                             !isAllZero(supla_info->guid, sizeof(supla_info->guid)) &&
                             !isAllZero(supla_info->authKey, sizeof(supla_info->authKey));
    if (supla_info->enabled && !supla_info->configured)
    {
        BWC_LOG_P(PSTR("SUPLA > incomplete or unclaimed configuration; integration disabled\n"), 0);
        supla_info->enabled = false;
    }
}

/** Persist a complete SUPLA candidate without mutating the live configuration. */
static bool persistSuplaConfig(const sSupla_info &config)
{
    if (config.server.length() >= SUPLA_SERVER_MAX_SIZE ||
        config.email.length() >= SUPLA_EMAIL_MAX_SIZE)
        return false;

    sSupla_info_eeprom_t stored;
    stored.enabled = config.enabled;
    memcpy(stored.guid, config.guid, sizeof(stored.guid));
    memcpy(stored.authKey, config.authKey, sizeof(stored.authKey));
    snprintf(stored.server, sizeof(stored.server), "%s", config.server.c_str());
    snprintf(stored.email, sizeof(stored.email), "%s", config.email.c_str());
    snprintf(stored.setupPassword,
             sizeof(stored.setupPassword),
             "%s",
             String(WM_AP_PASSWORD_F).c_str());
    stored.checksum = suplaConfigChecksum(stored);

    EEPROM.begin(EEPROM_CONFIG_SIZE);
    sSupla_info_eeprom_t previous;
    EEPROM.get(SUPLA_CONFIG_EEPROM_OFFSET, previous);
    EEPROM.put(SUPLA_CONFIG_EEPROM_OFFSET, stored);
    const bool saved = EEPROM.commit();

    if (!saved)
    {
        // EEPROM.end() commits dirty data once more. Restore the previous
        // bytes first so that its retry can never persist a rejected candidate.
        EEPROM.put(SUPLA_CONFIG_EEPROM_OFFSET, previous);
        if (!EEPROM.commit())
            BWC_LOG_P(PSTR("SUPLA > EEPROM rollback failed\n"), 0);
    }
    EEPROM.end();
    return saved;
}

/** Persist the current SUPLA settings (kept for internal API compatibility). */
bool saveSupla()
{
    return persistSuplaConfig(*supla_info);
}

void startSupla()
{
    if (suplaBridge || !supla_info->enabled || !supla_info->configured)
        return;

    HeapSelectIram ephemeral;
    suplaBridge = new SuplaSpaBridge;
    if (!suplaBridge ||
        !suplaBridge->begin(bwc,
                            supla_info->guid,
                            supla_info->authKey,
                            supla_info->server.c_str(),
                            supla_info->email.c_str()))
    {
        BWC_LOG_P(PSTR("SUPLA > initialization failed\n"), 0);
        delete suplaBridge;
        suplaBridge = nullptr;
        return;
    }

    SpaSnapshot snapshot;
    if (bwc->getSnapshot(snapshot))
        suplaBridge->sync(snapshot, true);
    BWC_LOG_P(PSTR("SUPLA > integration started\n"), 0);
}

void handleGetSupla()
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
    if (server->method() != HTTP_GET)
    {
        server->send(405, F("application/json"), F("{\"error\":\"Method not allowed\"}"));
        return;
    }

    StaticJsonDocument<640> doc;
    const bool configurationUnlocked = isSuplaConfigurationUnlocked();
    // The per-boot token also protects destructive local actions from
    // cross-origin requests.
    ensureSuplaCsrfToken();
    doc[F("enabled")] = supla_info->enabled;
    // Do not expose account data through the unauthenticated local status API.
    // An empty e-mail on save means "keep the existing assignment" when this
    // flag is true.
    doc[F("email")] = "";
    doc[F("emailConfigured")] = supla_info->configured;
    doc[F("server")] = supla_info->server;
    doc[F("configured")] = supla_info->configured;
    doc[F("wifiConfigured")] = hasHomeNetworkConfiguration();
    doc[F("setupPasswordConfigured")] = supla_info->setupPasswordConfigured;
    doc[F("configurationUnlocked")] = configurationUnlocked;
    doc[F("provisioningSecondsRemaining")] = suplaProvisioningSecondsRemaining();
    doc[F("running")] = suplaBridge && suplaBridge->isRunning();
    doc[F("status")] = currentSuplaStatus();
    doc[F("csrfToken")] = suplaCsrfToken;

    String json;
    json.reserve(384);
    if (serializeJson(doc, json) == 0)
    {
        server->send(500, F("application/json"), F("{\"error\":\"Serialization failed\"}"));
        return;
    }
    server->send(200, F("application/json"), json);
}

void handleSetSupla()
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
    if (!hasJsonContentType())
    {
        server->send(415, F("application/json"), F("{\"error\":\"Content-Type must be application/json\"}"));
        return;
    }
    if (!hasExclusivePlainBody())
    {
        server->send(400, F("application/json"), F("{\"error\":\"Missing JSON body\"}"));
        return;
    }

    // Worst case includes a 255-byte e-mail, a 64-byte server name and the
    // 32-byte CSRF token.
    StaticJsonDocument<640> doc;
    const DeserializationError error = deserializeJson(doc, server->arg(F("plain")));
    if (error || !doc.is<JsonObject>() ||
        !doc[F("enabled")].is<bool>() ||
        !doc[F("email")].is<const char *>() ||
        !doc[F("server")].is<const char *>() ||
        !doc[F("csrfToken")].is<const char *>())
    {
        server->send(400, F("application/json"), F("{\"error\":\"Invalid configuration\"}"));
        return;
    }

    if (!isValidSuplaCsrfToken(doc[F("csrfToken")].as<const char *>()))
    {
        server->send(403, F("application/json"), F("{\"error\":\"Invalid request token\"}"));
        return;
    }

    String email = doc[F("email")].as<String>();
    String serverName = doc[F("server")].as<String>();
    email.trim();
    serverName.trim();
    const bool enabled = doc[F("enabled")].as<bool>();
    if (email.isEmpty() && supla_info->configured)
        email = supla_info->email;

    if (enabled && email.isEmpty())
    {
        server->send(422, F("application/json"), F("{\"error\":\"Enabled SUPLA requires an e-mail\"}"));
        return;
    }

    if (enabled && !hasHomeNetworkConfiguration())
    {
        server->send(422,
                     F("application/json"),
                     F("{\"error\":\"Configure the home WiFi network before enabling SUPLA\"}"));
        return;
    }

    if (email.length() >= SUPLA_EMAIL_MAX_SIZE ||
        serverName.length() >= SUPLA_SERVER_MAX_SIZE ||
        (!email.isEmpty() && !isValidSuplaEmail(email)) || !isValidSuplaServer(serverName))
    {
        server->send(422, F("application/json"), F("{\"error\":\"Invalid e-mail or server name\"}"));
        return;
    }

    sSupla_info candidate = *supla_info;
    const bool identityMissing =
        isAllZero(supla_info->guid, sizeof(supla_info->guid)) ||
        isAllZero(supla_info->authKey, sizeof(supla_info->authKey));
    const bool emailChanged = supla_info->configured &&
                              !supla_info->email.equalsIgnoreCase(email);
    const bool serverChanged = supla_info->configured &&
                               supla_info->server != serverName;
    if (emailChanged || serverChanged || identityMissing)
        generateSuplaIdentity(candidate);

    candidate.email = email;
    candidate.server = serverName;
    candidate.setupPassword = String(WM_AP_PASSWORD_F);
    candidate.setupPasswordConfigured = true;
    candidate.enabled = enabled;
    candidate.configured = !email.isEmpty() &&
                           !isAllZero(candidate.guid, sizeof(candidate.guid)) &&
                           !isAllZero(candidate.authKey, sizeof(candidate.authKey));

    if (!persistSuplaConfig(candidate))
    {
        server->send(500, F("application/json"), F("{\"error\":\"EEPROM write failed\"}"));
        return;
    }

    // Only disrupt the working bridge and its runtime storage after the new
    // EEPROM record is durable. A failed write leaves the old session intact.
    const bool associationChanged = emailChanged || serverChanged || identityMissing;
    if (suplaBridge && (associationChanged || !enabled))
        suplaBridge->stop();
    if (associationChanged)
        removeSuplaRuntimeConfig();
    *supla_info = candidate;

    server->send(200, F("application/json"), F("{\"saved\":true,\"restartRequired\":true}"));
    scheduleDeviceRestart(true);
}

void handleResetSupla()
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
    if (!hasJsonContentType())
    {
        server->send(415, F("application/json"), F("{\"error\":\"Content-Type must be application/json\"}"));
        return;
    }
    if (!hasExclusivePlainBody())
    {
        server->send(400, F("application/json"), F("{\"error\":\"Missing JSON body\"}"));
        return;
    }

    StaticJsonDocument<256> doc;
    const DeserializationError error = deserializeJson(doc, server->arg(F("plain")));
    if (error || !doc.is<JsonObject>() || !doc[F("csrfToken")].is<const char *>())
    {
        server->send(400, F("application/json"), F("{\"error\":\"Invalid reset request\"}"));
        return;
    }

    if (!isValidSuplaCsrfToken(doc[F("csrfToken")].as<const char *>()))
    {
        server->send(403, F("application/json"), F("{\"error\":\"Invalid request token\"}"));
        return;
    }

    // Reset only the SUPLA cloud assignment. WiFi and the standard setup AP
    // password stay unchanged.
    sSupla_info candidate = *supla_info;
    candidate.enabled = false;
    candidate.configured = false;
    candidate.server = "";
    candidate.email = "";
    candidate.setupPassword = String(WM_AP_PASSWORD_F);
    candidate.setupPasswordConfigured = true;
    generateSuplaIdentity(candidate);

    if (!persistSuplaConfig(candidate))
    {
        server->send(500, F("application/json"), F("{\"error\":\"EEPROM write failed\"}"));
        return;
    }

    if (suplaBridge)
        suplaBridge->stop();
    removeSuplaRuntimeConfig();
    *supla_info = candidate;

    server->send(200, F("application/json"), F("{\"reset\":true,\"restartRequired\":true}"));
    scheduleDeviceRestart(true);
}

void handleFactoryReset()
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
    if(!hasJsonContentType())
    {
        server->send(415,
                     F("application/json"),
                     F("{\"error\":\"Content-Type must be application/json\"}"));
        return;
    }
    if(!hasExclusivePlainBody())
    {
        server->send(400,
                     F("application/json"),
                     F("{\"error\":\"Missing JSON body\"}"));
        return;
    }

    StaticJsonDocument<128> doc;
    const DeserializationError error = deserializeJson(doc, server->arg(F("plain")));
    if(error || !doc.is<JsonObject>() || !doc[F("csrfToken")].is<const char *>())
    {
        server->send(400,
                     F("application/json"),
                     F("{\"error\":\"Invalid factory reset request\"}"));
        return;
    }

    if(!isValidSuplaCsrfToken(doc[F("csrfToken")].as<const char *>()))
    {
        server->send(403,
                     F("application/json"),
                     F("{\"error\":\"Invalid request token\"}"));
        return;
    }

    factoryResetRequested = true;
    server->send(200,
                 F("application/json"),
                 F("{\"reset\":true,\"restarting\":true}"));
    scheduleDeviceRestart(true);
}

/**
 * response for /restart/
 */
void handleRestart()
{
    server->send(200, F("text/html"), F("ESP restart ..."));

    server->sendHeader(F("Location"), "/");
    server->send(303);

    delay(1000);
    stopall();
    delay(1000);
    BWC_LOG_P(PSTR("ESP restart ...\n"), 0);
    ESP.restart();
    delay(3000);
}

void updateStart()
{
    BWC_LOG_P(PSTR("OTA > update start\n"), 0);
}
void updateEnd()
{
    BWC_LOG_P(PSTR("OTA > update finish\n"), 0);
}
void udpateProgress(int cur, int total)
{
    BWC_LOG_P(PSTR("OTA: update process at %d of %d bytes...\n"), cur, total);
}
void updateError(int err)
{
    BWC_LOG_P(PSTR("update fatal error code %d\n"), err);
}

/**
 * MQTT setup and connect
 * @author 877dev
 */
void startMqtt()
{
    {
        HeapSelectIram ephemeral;
        BWC_LOG_P(PSTR("MQTT > start. Iram heap: %d\n"), ESP.getFreeHeap());
        if (!aWifiClient)
            aWifiClient = new WiFiClient;
        if (!mqttClient)
            mqttClient = new PubSubClient(*aWifiClient);

        // load mqtt credential file if it exists, and update default strings
        loadMqtt();

        // disconnect in case we are already connected
        mqttClient->disconnect();

        // setup MQTT broker information as defined earlier
        mqttClient->setServer(mqtt_info->mqttHost.c_str(), mqtt_info->mqttPort);
        // set buffer for larger messages, new to library 2.8.0
        // if (mqttClient->setBufferSize(1536))
        {
            // Serial.println(F("MQTT > Buffer size successfully increased"));
        }
        mqttClient->setKeepAlive(60);
        mqttClient->setSocketTimeout(30);
        // set callback details
        // this function is called automatically whenever a message arrives on a subscribed topic.
        mqttClient->setCallback(mqttCallback);
        // Connect to MQTT broker, publish Status/MAC/count, and subscribe to keypad topic.
    }
    mqttConnect();
    BWC_YIELD;
}

/**
 * MQTT callback function
 * @author 877dev
 */
void mqttCallback(char *topic, byte *payload, unsigned int length)
{
    // Serial.print(F("MQTT > Message arrived ["));
    // Serial.print(topic);
    // Serial.print(")] ");
    for (unsigned int i = 0; i < length; i++)
    {
        // Serial.print((char)payload[i]);
    }
    // Serial.println();
    if (String(topic).equals(String(mqtt_info->mqttBaseTopic) + F("/command")))
    {
        // DynamicJsonDocument doc(256);
        StaticJsonDocument<256> doc;
        String message = (const char *)&payload[0];
        DeserializationError error = deserializeJson(doc, message);
        if (error)
        {
            return;
        }

        Commands command = doc[F("CMD")];
        int64_t value = doc[F("VALUE")];
        int64_t xtime = doc[F("XTIME")];
        int64_t interval = doc[F("INTERVAL")];
        String txt = doc[F("TXT")] | "";
        command_que_item item;
        item.cmd = command;
        item.val = value;
        item.xtime = xtime;
        item.interval = interval;
        item.text = txt;
        bwc->add_command(item);
        return;
    }

    /* author @malfurion, edited by @visualapproach for v4 */
    if (String(topic).equals(String(mqtt_info->mqttBaseTopic) + F("/command_batch")))
    {
        DynamicJsonDocument doc(1024);
        String message = (const char *)&payload[0];
        DeserializationError error = deserializeJson(doc, message);
        if (error)
        {
            return;
        }

        JsonArray commandArray = doc.as<JsonArray>();

        for (JsonVariant commandItem : commandArray)
        {
            Commands command = commandItem[F("CMD")];
            int64_t value = commandItem[F("VALUE")];
            int64_t xtime = commandItem[F("XTIME")];
            int64_t interval = commandItem[F("INTERVAL")];
            String txt = doc[F("TXT")] | "";
            command_que_item item;
            item.cmd = command;
            item.val = value;
            item.xtime = xtime;
            item.interval = interval;
            item.text = txt;
            bwc->add_command(item);
        }

        return;
    }

    if (String(topic).equals(String(mqtt_info->mqttBaseTopic) + F("/set_config")))
    {
        String message = (const char *)&payload[0];
        bwc->setJSONSettings(message);
        send_mqtt_cfg_needed = true;
    }
}

/**
 * Connect to MQTT broker, publish Status/MAC/count, and subscribe to keypad topic.
 */
void mqttConnect()
{
    // do not connect if MQTT is not enabled or WiFI not connected
    if (!mqtt_info->useMqtt || (WiFi.status() != WL_CONNECTED))
    {
        return;
    }
    BWC_LOG_P(PSTR("MQTT > connecting\n"), 0);

    // Serial.print(F("MQTT > Connecting ... "));
    // We'll connect with a Retained Last Will that updates the 'Status' topic with "Dead" when the device goes offline...
    if (mqttClient->connect(
            mqtt_info->mqttClientId.c_str(),                           // client_id : the client ID to use when connecting to the server->
            mqtt_info->mqttUsername.c_str(),                           // username : the username to use. If NULL, no username or password is used (const char[])
            mqtt_info->mqttPassword.c_str(),                           // password : the password to use. If NULL, no password is used (const char[])setupHA
            (String(mqtt_info->mqttBaseTopic) + F("/Status")).c_str(), // willTopic : the topic to be used by the will message (const char[])
            0,                                                         // willQoS : the quality of service to be used by the will message (int : 0,1 or 2)
            1,                                                         // willRetain : whether the will should be published with the retain flag (int : 0 or 1)
            "Dead"))                                                   // willMessage : the payload of the will message (const char[])
    {
        // Serial.println(F("success!"));
        mqtt_connect_count++;

        // update MQTT every X seconds. (will also be updated on state changes)
        updateMqttTimer->attach(mqtt_info->mqttTelemetryInterval, []
                                { sendMQTTFlag = true; });

        // These all have the Retained flag set to true, so that the value is stored on the server and can be retrieved at any point
        // Check the 'Status' topic to see that the device is still online before relying on the data from these retained topics
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/Status")).c_str(), "Alive", true);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/MAC_Address")).c_str(), WiFi.macAddress().c_str(), true);                 // Device MAC Address
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/MQTT_Connect_Count")).c_str(), String(mqtt_connect_count).c_str(), true); // MQTT Connect Count
        mqttClient->loop();

        // Watch the 'command' topic for incoming MQTT messages
        mqttClient->subscribe((String(mqtt_info->mqttBaseTopic) + F("/command")).c_str());
        mqttClient->subscribe((String(mqtt_info->mqttBaseTopic) + F("/command_batch")).c_str());
        mqttClient->subscribe((String(mqtt_info->mqttBaseTopic) + F("/set_config")).c_str());
        mqttClient->loop();

#ifdef ESP8266
        // mqttClient->publish((String(mqttBaseTopic) + "/reboot_time").c_str(), DateTime.format(DateFormatter::SIMPLE).c_str(), true);
        // mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/reboot_time")).c_str(), (bwc->reboot_time_str+'Z').c_str(), true);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/reboot_reason")).c_str(), ESP.getResetReason().c_str(), true);
        String buttonname;
        buttonname.reserve(32);
        bwc->getButtonName(buttonname);
        mqttClient->publish((String(mqtt_info->mqttBaseTopic) + F("/button")).c_str(), buttonname.c_str(), true);
        mqttClient->loop();
        sendMQTT();
        BWC_LOG_P(PSTR("MQTT > Sending HA discovery"), 0);
        mqttClient->setBufferSize(1536);
        setupHA();
        mqttClient->setBufferSize(512);
        mqttClient->loop();
        // Serial.println(F("MQTT Sending config"));
        // sendMQTTConfig();    // Stack smashing if doing this here :-(
        send_mqtt_cfg_needed = true;
        BWC_LOG_P(PSTR("MQTT > connect done\n"), 0);
#endif
    }
    else
    {
        // Serial.print(F("failed, Return Code = "));
        // Serial.println(mqttClient->state()); // states explained in webSocket->js
    }
    BWC_YIELD;
}

time_t getBootTime()
{
    time_t seconds = millis() / 1000;
    time_t result = time(nullptr) - seconds;
    return result;
}

void handleHealth()
{
    server->sendHeader(F("Cache-Control"), F("no-store"));
#ifdef ESP8266
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t maxBlock = ESP.getMaxFreeBlockSize();
    // This is a high-water mark of stack use, not the current stack pointer.
    const uint32_t freeContinuationStack = ESP.getFreeContStack();
    DynamicJsonDocument doc(1024);
    doc[F("ok")] = freeHeap >= 6000 && maxBlock >= 4096;
    doc[F("firmware")] = FW_VERSION;
    doc[F("uptimeSeconds")] = millis() / 1000UL;
    doc[F("unixTime")] = static_cast<uint64_t>(time(nullptr));
    doc[F("freeHeap")] = freeHeap;
    doc[F("minimumHeap")] = heap_water_mark;
    doc[F("largestHeapBlock")] = maxBlock;
    doc[F("heapFragmentationPercent")] = ESP.getHeapFragmentation();
    doc[F("freeContinuationStack")] = freeContinuationStack;
    doc[F("wifiConnected")] = WiFi.status() == WL_CONNECTED;
    doc[F("wifiRssi")] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
    doc[F("wifiGotIpCount")] = wifiGotIpCount;
    doc[F("wifiDisconnectCount")] = wifiDisconnectCount;
    doc[F("wifiLastDisconnectReason")] = wifiLastDisconnectReason;
    doc[F("wifiLastDisconnectMillis")] = wifiLastDisconnectMillis;
    doc[F("suplaRunning")] = suplaBridge && suplaBridge->isRunning();
    doc[F("suplaStatus")] = suplaBridge ? suplaBridge->status() : "stopped";
    doc[F("resetReason")] = ESP.getResetReason();
    const auto& crash = CrashDiagnostics::lastCrash();
    char address[11];
    if(crash.reason == 0)
        doc[F("lastCrash")] = nullptr;
    else
    {
        auto summary = doc.createNestedObject(F("lastCrash"));
        summary[F("reason")] = CrashDiagnostics::reasonName(crash.reason);
        summary[F("reasonCode")] = crash.reason;
        summary[F("source")] = CrashDiagnostics::sourceName(crash.source);
        if(crash.reason == REASON_EXCEPTION_RST)
            summary[F("exceptionCause")] = crash.exceptionCause;
        if(crash.programCounter != 0)
        {
            snprintf_P(address, sizeof(address), PSTR("0x%08lx"),
                       static_cast<unsigned long>(crash.programCounter));
            summary[F("programCounter")] = address;
        }
    }
    doc[F("model")] = bwc ? bwc->getModel() : "unknown";
    if(bwc && bwc->cio && bwc->dsp)
    {
        doc[F("cioGoodPackets")] = bwc->cio->good_packets_count;
        doc[F("cioBadPackets")] = bwc->cio->bad_packets_count;
        doc[F("cioPacketError")] = bwc->cio->packet_error;
        doc[F("dspGoodPackets")] = bwc->dsp->good_packets_count;
        doc[F("dspBadPackets")] = bwc->dsp->bad_packets_count;
    }

    String json;
    const size_t size = measureJson(doc);
    if(doc.overflowed() || !json.reserve(size) || serializeJson(doc, json) != size)
    {
        server->send(500, F("application/json"), F("{\"ok\":false,\"error\":\"serialization\"}"));
        return;
    }
    server->send(200, F("application/json"), json);
#else
    server->send(501, F("application/json"), F("{\"ok\":false}"));
#endif
}

void handleESPInfo()
{
#ifdef ESP8266
    char stack;
    uint32_t stacksize = stack_start - &stack;
    constexpr size_t BUFSIZE = 384;
    std::unique_ptr<char[]> response(new (std::nothrow) char[BUFSIZE]);
    if(!response)
    {
        server->send(503, F("text/plain"), F("Not enough memory for diagnostics."));
        return;
    }

    char const *response_dram =
        PSTR(
            "Stack size:               %u \n"
            "Free Dram Heap:           %u \n"
            "Min Dram Heap:            %u \n"
            "Max free Dram block size: %u \n\n");

    char const *response_iram =
        PSTR(
            "Free Iram Heap:           %u \n"
            "Max free Iram block size: %u \n\n"
            "Core version:             %s \n"
            "CPU fq:                   %u MHz\n"
            "Cycle count:              %u \n"
            "Free cont stack:          %u \n"
            "Sketch size:              %u \n"
            "Free sketch space:        %u \n");

    server->setContentLength(CONTENT_LENGTH_UNKNOWN);
    server->send(200, F("text/plain"), "");

    snprintf_P(response.get(), BUFSIZE, response_dram,
               stacksize,
               ESP.getFreeHeap(),
               heap_water_mark,
               ESP.getMaxFreeBlockSize());
    server->sendContent(response.get());
    uint32_t iram_heap;
    uint32_t iram_maxblock;
    {
        HeapSelectIram ephemeral;
        iram_heap = ESP.getFreeHeap();
        iram_maxblock = ESP.getMaxFreeBlockSize();
    }
    snprintf_P(response.get(), BUFSIZE, response_iram,
               iram_heap,
               iram_maxblock,
               ESP.getCoreVersion().c_str(),
               ESP.getCpuFreqMHz(),
               ESP.getCycleCount(),
               ESP.getFreeContStack(),
               ESP.getSketchSize(),
               ESP.getFreeSketchSpace());

    server->sendContent(response.get());
    server->sendContent("");

#endif
}

void setTemperatureFromSensor()
{
    if (bwc->hasTempSensor)
    {
        tempSensors->requestTemperatures();
        float temperatureC = tempSensors->getTempCByIndex(0);
        // float temperatureF = tempSensors.getTempFByIndex(0);
        // Serial.print(temperatureC);
        // Serial.println("ºC");
        // Serial.print(temperatureF);
        // Serial.println("ºF");

        // Ignore bad reads
        if (temperatureC >= -20.0)
        {
            bwc->setAmbientTemperature(temperatureC, true);
        }
    }
    BWC_YIELD;
}

#include "ha.hpp"
#include "prometheus.hpp"
