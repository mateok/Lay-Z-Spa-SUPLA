#pragma once
#include <Arduino.h>
#include <cstring>

/*Indicies to access binary codes or pretty button names.*/
enum Buttons: byte
{
    NOBTN,
    LOCK,
    TIMER,
    BUBBLES,
    UNIT,
    HEAT,
    PUMP,
    DOWN,
    UP,
    POWER,
    HYDROJETS
};

/*Pretty names for buttons. Must be in same order as "enum Buttons" in file "enums.h"*/
const String ButtonNames[] = {
    "NOBTN",
    "LOCK",
    "TIMER",
    "BUBBLES",
    "UNIT",
    "HEAT",
    "PUMP",
    "DOWN",
    "UP",
    "POWER",
    "HYDROJETS"
};

enum States: byte
{
    LOCKEDSTATE,
    POWERSTATE,
    UNITSTATE,
    BUBBLESSTATE,
    HEATGRNSTATE,
    HEATREDSTATE,
    HEATSTATE,
    PUMPSTATE,
    TEMPERATURE,
    TARGET,
    CHAR1,
    CHAR2,
    CHAR3,
    JETSSTATE,
    ERROR
};

const uint8_t CHARS[] = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', ' ', '-', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 
    'h', 'H', 'i', 'j', 'k', 'l', 'm', 'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'x', 'y', 'z'
};

enum Commands: byte
{
    SETTARGET,
    SETUNIT,
    SETBUBBLES,
    SETHEATER,
    SETPUMP,
    RESETQ,
    REBOOTESP,
    GETTARGET,
    RESETTIMES,
    RESETCLTIMER,
    RESETFREPLACETIMER,
    SETJETS,
    SETBRIGHTNESS,
    SETBEEP,
    SETAMBIENTF,
    SETAMBIENTC,
    RESETDAILY,
    SETGODMODE,
    SETFULLPOWER,
    PRINTTEXT,
    SETREADY,
    SETR,
    RESETFRINSETIMER,
    RESETFCLEANTIMER,
    SETPOWER
};

enum ToggleButtons: byte
{
    BUBBLETOGGLE,
    JETSTOGGLE,
    PUMPTOGGLE,
    HEATTOGGLE
};

enum Models: uint8_t
{
    PRE2021,
    MIAMI2021,
    MALDIVES2021,
    M54149E,
    M54173,
    M54154,
    M54144,
    M54138,
    M54123
};

struct Power
{
    int HEATERPOWER_STAGE1;
    int HEATERPOWER_STAGE2;
    int PUMPPOWER;
    int AIRPOWER;
    int IDLEPOWER;
    int JETPOWER;
};

struct HeaterStages
{
    bool stage1_on = false;
    bool stage2_on = false;
};

struct sStates
{
    uint8_t locked = 0;
    uint8_t power = 0;
    uint8_t unit = 0;
    uint8_t bubbles = 0;
    uint8_t heatgrn = 0;
    uint8_t heatred = 0;
    uint8_t heat = 0;
    uint8_t pump = 0;
    uint8_t temperature = 25;
    uint8_t target = 20;
    uint8_t char1 = 'a';
    uint8_t char2 = 'b';
    uint8_t char3 = 'c';
    uint8_t jets = 0;
    uint8_t error = 0;
    // uint8_t timerbutton = 0;
    // uint8_t upbutton = 0;
    // uint8_t downbutton = 0;
    uint8_t timerled1 = 0;
    uint8_t timerled2 = 0;
    uint8_t timerbuttonled = 0;
    uint8_t brightness = 8;
    // bool fullpower = false;
    uint8_t no_of_heater_elements_on = 2;
    bool gettarget = false;
    bool godmode = false;

    // String toString()
    // {
    //     char res[130];
    //     PGM_P resfmt = PSTR("LCK: %d PWR: %d UNT: %d AIR: %d GRN: %d RED: %d HTR: %d FLT: %d TMP: %d TGT: %d JET: %d TIMLED1 %d TIMLED2: %d TIMLEDBTN: %d\n");
    //     sprintf_P(res, resfmt, locked, power, unit, bubbles, heatgrn, heatred, heat, pump, temperature, target, jets, timerled1, timerled2, timerbuttonled);
    //     return String(res);
    // }

    inline bool operator==(const sStates& rhs)
    {
        bool result = false;
        result = locked == rhs.locked;
        result &= power == rhs.power;
        result &= unit == rhs.unit;
        result &= bubbles == rhs.bubbles;
        result &= heatgrn == rhs.heatgrn;
        result &= heatred == rhs.heatred;
        result &= heat == rhs.heat;
        result &= pump == rhs.pump;
        result &= temperature == rhs.temperature;
        result &= target == rhs.target;
        result &= char1 == rhs.char1;
        result &= char2 == rhs.char2;
        result &= char3 == rhs.char3;
        result &= jets == rhs.jets;
        // result &= timerbutton == rhs.timerbutton;
        // result &= upbutton == rhs.upbutton;
        // result &= downbutton == rhs.downbutton;
        result &= timerled1 == rhs.timerled1;
        result &= timerled2 == rhs.timerled2;
        result &= brightness == rhs.brightness;
        result &= gettarget == rhs.gettarget;
        // result &= fullpower == rhs.fullpower;
        result &= no_of_heater_elements_on == rhs.no_of_heater_elements_on;
        result &= godmode == rhs.godmode;
        
        return result;
    }  
    inline bool operator!=(const sStates& rhs){return ! (*this == rhs);}
};

struct sToggles
{
    Buttons pressed_button = NOBTN;
    uint8_t target = 20;
    uint8_t no_of_heater_elements_on = 2;
    bool locked_pressed = 0;
    bool power_change = 0;
    bool unit_change = 0;
    bool bubbles_change = 0;
    bool heat_change = 0;
    bool pump_change = 0;
    bool jets_change = 0;
    bool timer_pressed = 0;
    bool up_pressed = 0;
    bool down_pressed = 0;
    /*Requested state, not toggled*/
    bool godmode = false;
    /*Requested state, not toggled*/
    /*Requested state, not toggled*/

    // String toString()
    // {
    //     char res[130];
    //     PGM_P resfmt = PSTR("Toggle: LCK: %d PWR: %d UNT: %d AIR: %d HTR: %d FLT: %d JET: %d   Set: TGT: %d BTN: %d\n");
    //     sprintf_P(res, resfmt, locked_pressed, power_change, unit_change, bubbles_change, heat_change, pump_change, jets_change, target, pressed_button);
    //     return String(res);
    // }

    inline bool operator==(const sToggles& rhs)
    {
        bool result = false;
        result = locked_pressed == rhs.locked_pressed;
        result &= power_change == rhs.power_change;
        result &= unit_change == rhs.unit_change;
        result &= bubbles_change == rhs.bubbles_change;
        result &= heat_change == rhs.heat_change;
        result &= pump_change == rhs.pump_change;
        result &= jets_change == rhs.jets_change;
        result &= no_of_heater_elements_on == rhs.no_of_heater_elements_on;
        result &= pressed_button == rhs.pressed_button;
        result &= godmode == rhs.godmode;
        result &= target == rhs.target;
        return result;
    }
    inline bool operator!=(const sToggles& rhs){return !(*this == rhs);}
};

struct sNote
{
    int frequency_hz;
    int duration_ms;
};

struct sWifi_info
{
    String apSsid;
    String apPwd;
    String ip4Address_str;
    String ip4Gateway_str;
    String ip4Subnet_str;
    String ip4DnsPrimary_str;
    String ip4DnsSecondary_str;
    String ip4NTP_str;
    bool enableAp;
    bool enableWmApFallback;
    bool enableStaticIp4;
    // RF tuning — adjustable from web UI without rebuild/reflash.
    // Defaults tuned for max range. Both fields persist in wifi.json.
    float txPowerDbm = 20.5f;       // 0..20.5 dBm; max wins for marginal links (+3 dB vs upstream default).
    bool noModemSleep = true;       // true = WIFI_NONE_SLEEP, snappier + ~+1 dB effective.
    int phyMode = 0;                // 0=auto, 1=802.11b, 2=11g, 3=11n.
                                    // 11b is +5..+8 dB more sensitive on the edge but
                                    // capped at 11 Mbps (irrelevant here).
};

struct sWifi_info_eeprom_t
{
    //size of block is 136
    char tag[4] = {'W', 'I', 'F', 'I'}; // tag to identify valid data
    uint32_t ip4Address;
    uint32_t ip4Gateway;
    uint32_t ip4Subnet;
    uint32_t ip4DnsPrimary;
    uint32_t ip4DnsSecondary;
    uint32_t ip4NTP;
    int phyMode;
    float txPowerDbm;
    char apSsid[32];
    char apPwd[64];
    bool enableAp;
    bool enableWmApFallback;
    bool enableStaticIp4;
    bool noModemSleep;
};
struct sMQTT_info_eeprom_t
{
    //size of block is 176
    char tag[4] = {'M', 'Q', 'T', 'T'}; // tag to identify valid data
    char mqttHost[32];
    int mqttTelemetryInterval;
    int mqttPort;
    char mqttUsername[32];
    char mqttPassword[32];
    char mqttClientId[32];
    char mqttBaseTopic[32];
    bool useMqtt;
};

struct sHW_config_eeprom_t
{
    //size of block is 72
    Power power_levels;
    char tag[4] = {'H', 'W', 'C', 'F'}; // tag to identify valid data
    Models cioNo;
    Models dspNo;
    char pcbname[8];
    int pins[8];
    bool hasTempSensor;
    bool power_levels_override;
};

// Factory wiring for the direct Wemos D1 mini installation documented by
// this firmware. Values are Dx labels (not raw GPIO numbers); BWC converts
// them through the board's D0..D8 mapping before configuring the buses.
constexpr uint8_t DEFAULT_CIO_PINS[] = {4, 3, 2};
constexpr uint8_t DEFAULT_DSP_PINS[] = {7, 6, 5, 0};
constexpr uint8_t DEFAULT_TEMPERATURE_SENSOR_PIN = 8;

inline sHW_config_eeprom_t makeDefaultHardwareConfig()
{
    sHW_config_eeprom_t config{};
    config.cioNo = MIAMI2021;
    config.dspNo = MIAMI2021;
    config.pcbname[0] = 'c';
    config.pcbname[1] = 'u';
    config.pcbname[2] = 's';
    config.pcbname[3] = 't';
    config.pcbname[4] = 'o';
    config.pcbname[5] = 'm';
    config.pcbname[6] = '\0';
    for(size_t i = 0; i < 3; ++i) config.pins[i] = DEFAULT_CIO_PINS[i];
    for(size_t i = 0; i < 4; ++i) config.pins[3 + i] = DEFAULT_DSP_PINS[i];
    config.pins[7] = DEFAULT_TEMPERATURE_SENSOR_PIN;
    config.hasTempSensor = false;
    config.power_levels_override = false;
    return config;
}

inline bool isValidHardwareConfig(const sHW_config_eeprom_t& config)
{
    if(memcmp(config.tag, "HWCF", sizeof(config.tag)) != 0 ||
       config.cioNo > M54123 || config.dspNo > M54123 ||
       memchr(config.pcbname, '\0', sizeof(config.pcbname)) == nullptr)
        return false;

    for(size_t i = 0; i < 8; ++i)
        if(config.pins[i] < 0 || config.pins[i] > 8) return false;

    // Only pins used by the selected buses must be unique. Four-wire models
    // legitimately store zero in the unused clock/CS/audio slots.
    bool active[8] = {true, true, false, true, true, false, false,
                      config.hasTempSensor};
    if(config.cioNo <= M54149E) active[2] = true;
    if(config.dspNo <= M54149E)
    {
        active[5] = true;
        active[6] = true;
    }

    for(size_t i = 0; i < 8; ++i)
    {
        if(!active[i]) continue;
        for(size_t j = i + 1; j < 8; ++j)
            if(active[j] && config.pins[i] == config.pins[j]) return false;
    }
    return true;
}

// SUPLA credentials are kept in EEPROM so they survive LittleFS updates.
// Keep this structure independent of the SuplaDevice headers: the sizes below
// mirror the public SUPLA protocol limits (server 65, e-mail 256, GUID/AuthKey
// 16 bytes each).
constexpr uint8_t SUPLA_CONFIG_VERSION = 2;
constexpr size_t SUPLA_SERVER_MAX_SIZE = 65;
constexpr size_t SUPLA_EMAIL_MAX_SIZE = 256;
constexpr size_t SUPLA_ID_SIZE = 16;
constexpr size_t SUPLA_SETUP_PASSWORD_MAX_SIZE = 64;

#pragma pack(push, 1)
struct sSupla_info_eeprom_t
{
    char tag[4] = {'S', 'U', 'P', 'L'};
    uint8_t version = SUPLA_CONFIG_VERSION;
    bool enabled = false;
    uint8_t guid[SUPLA_ID_SIZE] = {};
    uint8_t authKey[SUPLA_ID_SIZE] = {};
    char server[SUPLA_SERVER_MAX_SIZE] = {};
    char email[SUPLA_EMAIL_MAX_SIZE] = {};
    char setupPassword[SUPLA_SETUP_PASSWORD_MAX_SIZE] = {};
    uint32_t checksum = 0;
};
#pragma pack(pop)

// Centralized EEPROM layout. EEPROM.begin() has to cover the complete sector:
// committing a shorter buffer would erase configuration blocks placed after it.
constexpr size_t WIFI_CONFIG_EEPROM_OFFSET = 0;
constexpr size_t MQTT_CONFIG_EEPROM_OFFSET =
    WIFI_CONFIG_EEPROM_OFFSET + sizeof(sWifi_info_eeprom_t);
constexpr size_t HW_CONFIG_EEPROM_OFFSET =
    MQTT_CONFIG_EEPROM_OFFSET + sizeof(sMQTT_info_eeprom_t);
constexpr size_t SUPLA_CONFIG_EEPROM_OFFSET =
    HW_CONFIG_EEPROM_OFFSET + sizeof(sHW_config_eeprom_t);
constexpr size_t EEPROM_CONFIG_SIZE =
    SUPLA_CONFIG_EEPROM_OFFSET + sizeof(sSupla_info_eeprom_t);
static_assert(EEPROM_CONFIG_SIZE <= 4096,
              "Persistent configuration must fit in one ESP EEPROM sector");
