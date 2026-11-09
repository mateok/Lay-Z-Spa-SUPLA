#include "crash_summary.h"

#ifdef ESP8266
#include <Arduino.h>
#include <LittleFS.h>
#include <cstdlib>
#include <cstring>
#include "user_interface.h"

namespace
{
using CrashDiagnostics::Summary;
constexpr uint32_t kRecordMagic = 0x4C5A4301UL; // LZC, format 1
// eboot owns words 0–31, the filesystem update marker uses word 32.
constexpr uint32_t kRtcOffset = 33;
constexpr uint32_t kStackSmash = 253; // ESP8266 core postmortem reason codes
constexpr uint32_t kPanic = 254;
enum Source : uint32_t { Sdk = 1, Postmortem, LegacyLog };

struct Record
{
    uint32_t magic = 0;
    Summary summary;
};
static_assert(sizeof(Record) == 5 * sizeof(uint32_t), "Unexpected crash record layout");
static_assert(kRtcOffset * sizeof(uint32_t) + sizeof(Record) <= 512, "RTC record does not fit");
Summary lastSummary;

bool isFault(uint32_t reason)
{
    return (reason >= REASON_WDT_RST && reason <= REASON_SOFT_WDT_RST) ||
           reason == kStackSmash || reason == kPanic;
}

bool isCodeAddress(uint32_t address)
{
    return address >= 0x40000000UL && address < 0x40300000UL;
}

Summary makeSummary(uint32_t reason, uint32_t cause, uint32_t pc, Source source)
{
    return {reason, reason == REASON_EXCEPTION_RST && cause < 64 ? cause : 0,
            isCodeAddress(pc) ? pc : 0, source};
}

bool valid(const Record& record)
{
    const Summary& s = record.summary;
    return record.magic == kRecordMagic && isFault(s.reason) &&
           s.source >= Sdk && s.source <= LegacyLog &&
           (s.reason == REASON_EXCEPTION_RST ? s.exceptionCause < 64 : s.exceptionCause == 0) &&
           (s.programCounter == 0 || isCodeAddress(s.programCounter));
}

bool save(const Summary& summary)
{
    const Record record{kRecordMagic, summary};
    File file = LittleFS.open(F("/crash-summary.tmp"), "w");
    if(!file) return false;
    const bool written = file.write(reinterpret_cast<const uint8_t*>(&record), sizeof(record)) == sizeof(record);
    file.close();
    return written && LittleFS.rename(F("/crash-summary.tmp"), F("/crash-summary.bin"));
}

// Import just the numeric header of the newest legacy report. Old builds
// appended raw stack dumps; neither those bytes nor any text from the file
// are ever returned by the diagnostics API.
Summary importLegacyCrash()
{
    File file = LittleFS.open(F("/crashlog.txt"), "r");
    constexpr size_t kScanLimit = 64 * 1024;
    if(!file || (file.size() > kScanLimit && !file.seek(file.size() - kScanLimit))) return {};

    Summary result;
    enum class Field { Header, Reason, Cause, Pc } field = Field::Header;
    uint32_t reason = 0, cause = 0;
    uint8_t chunk[256];
    char line[128];
    size_t used = 0;
    // A seek may have landed in the middle of a line.
    bool tooLong = file.position() != 0;
    while(const size_t count = file.read(chunk, sizeof(chunk)))
    {
        for(size_t i = 0; i < count; ++i)
        {
            const char ch = static_cast<char>(chunk[i]);
            if(ch != '\n')
            {
                if(used + 1 < sizeof(line)) line[used++] = ch;
                else tooLong = true;
                continue;
            }

            line[used] = '\0';
            if(!tooLong)
            {
                char* end = nullptr;
                if(strncmp(line, "Crashed time: ", 14) == 0)
                    field = Field::Reason;
                else if(field == Field::Reason && strncmp(line, "Reason: ", 8) == 0)
                {
                    reason = strtoul(line + 8, &end, 10);
                    field = end != line + 8 && strncmp(end, " (", 2) == 0 && isFault(reason)
                            ? Field::Cause : Field::Header;
                }
                else if(field == Field::Cause && strncmp(line, "Cause:  ", 8) == 0)
                {
                    cause = strtoul(line + 8, &end, 10);
                    field = end != line + 8 && strncmp(end, " (", 2) == 0 && cause < 64
                            ? Field::Pc : Field::Header;
                }
                else if(field == Field::Pc && used == 14 && strncmp(line, "epc1: ", 6) == 0)
                {
                    const uint32_t pc = strtoul(line + 6, &end, 16);
                    if(end == line + used) result = makeSummary(reason, cause, pc, LegacyLog);
                    field = Field::Header;
                }
            }
            used = 0;
            tooLong = false;
        }
        yield();
    }
    return result;
}
}

void CrashDiagnostics::begin(bool filesystemMounted)
{
    Record rtc;
    const bool haveRtc = ESP.rtcUserMemoryRead(kRtcOffset, reinterpret_cast<uint32_t*>(&rtc), sizeof(rtc)) &&
                         valid(rtc) && rtc.summary.source == Postmortem;
    const rst_info& reset = *ESP.getResetInfoPtr();
    if(haveRtc)
        lastSummary = rtc.summary;
    else if(isFault(reset.reason))
        lastSummary = makeSummary(reset.reason, reset.exccause, reset.epc1, Sdk);

    if(!filesystemMounted) return;
    if(lastSummary.reason != 0)
    {
        if(save(lastSummary) && haveRtc)
        {
            uint32_t clearMagic = 0;
            ESP.rtcUserMemoryWrite(kRtcOffset, &clearMagic, sizeof(clearMagic));
        }
        return;
    }

    Record stored;
    {
        File file = LittleFS.open(F("/crash-summary.bin"), "r");
        if(file && file.size() == sizeof(stored) &&
           file.read(reinterpret_cast<uint8_t*>(&stored), sizeof(stored)) == sizeof(stored) && valid(stored))
        {
            lastSummary = stored.summary;
            return;
        }
    }
    lastSummary = importLegacyCrash();
    if(lastSummary.reason != 0) save(lastSummary);
}

const Summary& CrashDiagnostics::lastCrash() { return lastSummary; }

const char* CrashDiagnostics::reasonName(uint32_t reason)
{
    switch(reason)
    {
        case REASON_WDT_RST: return "Hardware Watchdog";
        case REASON_EXCEPTION_RST: return "Exception";
        case REASON_SOFT_WDT_RST: return "Software Watchdog";
        case kStackSmash: return "Stack smashing";
        case kPanic: return "Software panic";
        default: return "Unknown";
    }
}

const char* CrashDiagnostics::sourceName(uint32_t source)
{
    switch(source)
    {
        case Sdk: return "boot";
        case Postmortem: return "postmortem";
        case LegacyLog: return "legacy-log";
        default: return "unknown";
    }
}

extern "C" void custom_crash_callback(struct rst_info* reset, uint32_t, uint32_t)
{
    // The ESP may be out of memory or already have a damaged stack. Do not
    // allocate, write to flash, wait, or inspect raw stack memory here.
    if(!reset || !isFault(reset->reason)) return;
    Record record{kRecordMagic, makeSummary(reset->reason, reset->exccause, reset->epc1, Postmortem)};
    ESP.rtcUserMemoryWrite(kRtcOffset, reinterpret_cast<uint32_t*>(&record), sizeof(record));
}
#endif
