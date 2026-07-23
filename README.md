# Lay-Z-Spa SUPLA

[![Release](https://img.shields.io/github/v/release/mateok/Lay-Z-Spa-SUPLA?include_prereleases)](https://github.com/mateok/Lay-Z-Spa-SUPLA/releases)
[![License: GPL v3](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Platform: ESP8266](https://img.shields.io/badge/platform-ESP8266-orange.svg)](Code/platformio.ini)

Sterownik Wi-Fi dla jacuzzi Bestway Lay-Z-Spa oparty na ESP8266, rozszerzony o
natywną integrację z SUPLA Cloud. Umożliwia sterowanie SPA spoza domu bez
publicznego adresu IP, Home Assistanta i własnego brokera MQTT.

Projekt jest forkiem
[visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA](https://github.com/visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA)
i zachowuje jego lokalny panel WWW, obsługę panelu pompy, harmonogramy oraz
integrację MQTT. Bazą tej wersji była gałąź `development_v4`.

## Dokumentacja

- [Instalacja i pierwsza konfiguracja po polsku](docs/INSTALLATION_PL.md)
- [Szczegółowa dokumentacja integracji SUPLA](docs/SUPLA.md)
- [Historia zmian](CHANGELOG.md)
- [Instrukcja sprzętowa projektu bazowego](bwc-manual.pdf)
- [Dokumentacja techniczna projektu bazowego](bwc_docs.xlsx)

## Najważniejsze funkcje

- bezpośrednie, wychodzące połączenie ESP8266 z SUPLA Cloud przez TLS;
- zdalne sterowanie zasilaniem SPA, filtracją, grzaniem, AirJet i HydroJet,
  zależnie od możliwości wybranego modelu;
- termostat SUPLA z temperaturą docelową ustawianą co 1°C;
- timer termostatu i program tygodniowy wykonywany lokalnie przez ESP8266;
- odczyt temperatury wody, temperatury wirtualnej i opcjonalnej temperatury
  otoczenia;
- liczniki wieku chloru i filtrów wraz ze zdalnym resetowaniem;
- czas pracy pompy, grzania, AirJet i HydroJet;
- szacowane zużycie energii, aktualna moc i informacyjny czas do kąpieli;
- automatyczne pomijanie kanałów niedostępnych dla wybranego sprzętu;
- polski panel WWW i polskie domyślne nazwy kanałów SUPLA;
- równoległe działanie lokalnego panelu WWW, SUPLA i opcjonalnego MQTT;
- zdalna aktualizacja `firmware.bin` oraz `littlefs.bin` z przeglądarki;
- ustawienia fabryczne dostępne z lokalnego panelu;
- diagnostyka pod adresem `/health/`;
- automatyczne ponowne łączenie z Wi-Fi i SUPLA.

## Pobieranie gotowej wersji

Najnowsze gotowe obrazy znajdują się w sekcji
[Releases](https://github.com/mateok/Lay-Z-Spa-SUPLA/releases).

Do pełnej instalacji potrzebne są:

- `firmware.bin`;
- `littlefs.bin`.

Plik `SHA256SUMS.txt` pozwala zweryfikować pobrane obrazy. Szczegółowe
polecenia dla USB, PlatformIO i aktualizacji przez przeglądarkę opisuje
[instrukcja instalacji](docs/INSTALLATION_PL.md).

## Pierwsze uruchomienie

Po wgraniu obu obrazów:

1. Połącz się z siecią `Lay-Z-Spa-<identyfikator-układu>`.
2. Użyj domyślnego hasła `layzspam0dule`.
3. Otwórz `http://192.168.4.2/wifi.html`.
4. Zapisz dane domowej sieci Wi-Fi i uruchom ESP ponownie.
5. W SUPLA Cloud włącz rejestrację nowych urządzeń.
6. Otwórz lokalną stronę `supla.html`, włącz SUPLA i wpisz adres e-mail konta.
7. Pole serwera pozostaw puste dla automatycznego wykrywania SUPLA Cloud.
8. Zapisz konfigurację i poczekaj na urządzenie `Lay-Z-Spa` w aplikacji SUPLA.

Moduł nie wymaga hasła do konta SUPLA ani osobnego hasła administratora.

## Kanały SUPLA

Sterownik rejestruje wyłącznie kanały obsługiwane przez aktualną konfigurację
sprzętu. Dostępne typy kanałów obejmują:

- termostat SPA;
- zasilanie, pompę filtrującą, AirJet i HydroJet;
- temperatury i kod błędu pompy;
- liczniki serwisowe oraz akcje ich resetowania;
- czasy pracy, moc i energię;
- informacyjny czas do osiągnięcia temperatury docelowej.

Program tygodniowy jest pobierany z SUPLA i wykonywany lokalnie w kwadransach.
Ogólne harmonogramy z sekcji automatyzacji SUPLA działają jako polecenia
chmurowe. Szczegóły znajdują się w [opisie integracji](docs/SUPLA.md).

## Lokalny panel i MQTT

Dodanie SUPLA nie zastępuje oryginalnych funkcji projektu. Sterowanie przez
lokalny panel ESP8266 nadal działa, a MQTT można włączyć niezależnie, na
przykład do integracji z Home Assistantem.

Na stronie głównej panel pokazuje status MQTT, SUPLA, wersję firmware, model SPA
i poziom sygnału Wi-Fi.

## Aktualizacja bez USB

Otwórz:

```text
http://layzspa.local/update.html
```

lub użyj adresu IP modułu. Strona pozwala oddzielnie przesłać firmware i pełny
obraz LittleFS. Jest osadzona w firmware, dlatego pozostaje dostępna również do
odtworzenia przerwanej aktualizacji systemu plików.

Aktualizacja samego firmware zachowuje konfigurację i lokalne dane. Wgranie
LittleFS zachowuje konfigurację Wi-Fi, MQTT, sprzętu i przypisanie SUPLA
zapisane w EEPROM, ale odtwarza statystyki, kolejkę poleceń, ustawienia panelu i
pliki robocze SUPLA.

## Budowanie

Wymagane jest PlatformIO. Z katalogu `Code`:

```sh
pio run -e nodemcuv2
pio run -e nodemcuv2 -t buildfs
```

Wygenerowane obrazy:

```text
Code/.pio/build/nodemcuv2/firmware.bin
Code/.pio/build/nodemcuv2/littlefs.bin
```

Wgrywanie przez USB:

```sh
pio run -e nodemcuv2 -t upload --upload-port /dev/cu.usbserial-130
pio run -e nodemcuv2 -t uploadfs --upload-port /dev/cu.usbserial-130
```

Port należy dopasować do własnego konwertera USB-UART. Domyślna prędkość
115200 jest celowo zachowawcza i działa stabilniej z różnymi układami USB-UART.

## Sprzęt projektu bazowego

- ESP8266 NodeMCU 1.0, nie ESP32;
- dwukierunkowy konwerter poziomów logicznych;
- złącza odpowiednie dla cztero- lub sześcioprzewodowego panelu SPA.

Schematy, PCB i informacje o montażu pochodzą z projektu bazowego:

- [PCB V2B](https://oshwlab.com/visualapproach/bestway-wireless-controller-2_copy);
- [instrukcja sprzętowa](bwc-manual.pdf);
- [oryginalne FAQ](https://github.com/visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA/discussions/46).

## Pochodzenie i licencja

Oryginalny projekt został stworzony przez
[visualapproach](https://github.com/visualapproach) wraz z jego
współtwórcami. Historia Git i informacje o autorach zostały zachowane.

Ten fork dodaje integrację SUPLA, polski interfejs, aktualizacje przez
przeglądarkę, rozszerzoną diagnostykę oraz poprawki stabilności. Zmodyfikowane
wydanie zostało przygotowane w 2026 roku.

Projekt bazowy i niniejsze modyfikacje są udostępniane na warunkach
[GNU General Public License v3.0](LICENSE). Kod źródłowy odpowiadający każdemu
publicznemu wydaniu jest dostępny pod jego tagiem.

Jeżeli chcesz wesprzeć autora projektu bazowego:
[Buy me a coffee](https://paypal.me/TLandahl).
