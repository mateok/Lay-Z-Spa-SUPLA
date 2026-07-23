# Instalacja Lay-Z-Spa SUPLA

Ta instrukcja dotyczy wydania `v1.0.0-supla` przeznaczonego dla ESP8266
NodeMCU 1.0 (`nodemcuv2`) z 4 MB pamięci flash.

## Pliki wydania

Z sekcji
[Releases](https://github.com/mateok/Lay-Z-Spa-SUPLA/releases/latest)
pobierz:

- `firmware.bin` — program ESP8266;
- `littlefs.bin` — kompletny obraz panelu WWW i zasobów;
- `SHA256SUMS.txt` — sumy kontrolne obu obrazów.

Do pełnej instalacji wymagane są oba pliki. `littlefs.bin` ma zawsze dokładnie
1 024 000 bajtów, ponieważ jest obrazem całej przydzielonej partycji LittleFS.
Rozmiar ten nie oznacza, że same pliki panelu zajmują całą partycję.

Na macOS lub Linux sumy można sprawdzić poleceniem:

```sh
shasum -a 256 -c SHA256SUMS.txt
```

## Pierwsza instalacja przez USB

### Gotowe obrazy i esptool

Zainstaluj `esptool`, jeśli nie jest jeszcze dostępny:

```sh
python3 -m pip install esptool
```

Podłącz ESP8266 i sprawdź nazwę portu. Na macOS:

```sh
ls /dev/cu.usbserial-* /dev/cu.wchusbserial-* 2>/dev/null
```

Wgraj oba obrazy, zastępując nazwę portu właściwą wartością:

```sh
python3 -m esptool \
  --chip esp8266 \
  --port /dev/cu.usbserial-130 \
  --baud 115200 \
  write_flash \
  --flash_size detect \
  0x000000 firmware.bin \
  0x300000 littlefs.bin
```

### Budowanie i wgrywanie przez PlatformIO

Po sklonowaniu repozytorium:

```sh
cd Code
pio run -e nodemcuv2
pio run -e nodemcuv2 -t buildfs
pio run -e nodemcuv2 -t upload --upload-port /dev/cu.usbserial-130
pio run -e nodemcuv2 -t uploadfs --upload-port /dev/cu.usbserial-130
```

Jeżeli szybkie wysyłanie jest niestabilne, pozostaw `upload_speed = 115200`
w `Code/platformio_override.ini`.

## Aktualizacja działającego modułu

W lokalnym panelu otwórz:

```text
http://layzspa.local/update.html
```

Możesz również użyć adresu IP przydzielonego przez router.

1. W sekcji firmware wybierz `firmware.bin` i poczekaj na restart.
2. Ponownie otwórz `/update.html`.
3. W sekcji systemu plików wybierz `littlefs.bin`.
4. Poczekaj, aż urządzenie odtworzy pliki SUPLA i zakończy restarty.

Aktualizacja firmware zachowuje EEPROM oraz LittleFS. Aktualizacja LittleFS
zachowuje dane Wi-Fi, MQTT, konfigurację sprzętu i przypisanie SUPLA zapisane w
EEPROM, ale odtwarza lokalne statystyki SPA, kolejkę poleceń, ustawienia panelu
oraz pliki robocze SUPLA.

Strona `/update.html` jest częścią firmware. Pozostaje dostępna także wtedy,
gdy przesyłanie obrazu LittleFS zostało przerwane.

## Pierwsza konfiguracja

1. Po uruchomieniu połącz się z siecią
   `Lay-Z-Spa-<identyfikator-układu>`.
2. Domyślne hasło sieci konfiguracyjnej to `layzspam0dule`.
3. Otwórz `http://192.168.4.2/wifi.html`.
4. Wprowadź nazwę i hasło domowej sieci Wi-Fi, a następnie wybierz
   **Zapisz i uruchom ponownie**.
5. Po połączeniu z domową siecią odczytaj adres ESP8266 z routera albo użyj
   `http://layzspa.local/`.

## Konfiguracja sprzętu

W `hwconfig.html` domyślnie wybrano profil używany podczas przygotowania
wydania:

- CIO: `6 wire, 2021 (air)`;
- DSP: `6 wire, 2021 (air)`;
- PCB: `Custom`;
- CIO: data `D4`, clock `D3`, chip select `D2`;
- DSP: data `D7`, clock `D6`, chip select `D5`, audio `D0`.

Dla innego sterownika lub płytki wybierz wartości zgodne z posiadanym
okablowaniem przed rozpoczęciem sterowania.

## Dodanie do SUPLA

1. W SUPLA Cloud czasowo włącz rejestrację nowych urządzeń.
2. Otwórz `supla.html` w lokalnym panelu ESP8266.
3. Zaznacz **Włącz SUPLA**.
4. Wprowadź adres e-mail konta SUPLA. Hasła do konta SUPLA nie podaje się.
5. Pole serwera pozostaw puste, aby użyć automatycznego wykrywania SUPLA Cloud.
6. Wybierz **Zapisz i uruchom ponownie**.
7. Poczekaj, aż urządzenie `Lay-Z-Spa` pojawi się w aplikacji, a następnie
   wyłącz rejestrację nowych urządzeń w SUPLA Cloud.

Kanały niedostępne dla wybranego modelu SPA nie są rejestrowane. Termostat
obsługuje temperaturę docelową w pełnych stopniach, timer i program tygodniowy.
Pełny opis kanałów znajduje się w [dokumentacji SUPLA](SUPLA.md).

## Reset ustawień

Ostatnia pozycja menu lokalnego panelu, **Ustawienia fabryczne**, usuwa dane
Wi-Fi, MQTT, SUPLA, konfigurację sprzętu, ustawienia SPA, harmonogramy,
statystyki oraz pliki robocze. Firmware i wbudowany panel WWW pozostają
zainstalowane. Po restarcie urządzenie ponownie uruchamia sieć konfiguracyjną.

## Podstawowa diagnostyka

Stan urządzenia można odczytać bez modyfikowania konfiguracji:

```text
http://layzspa.local/health/
```

Odpowiedź zawiera między innymi wersję firmware, czas działania, pamięć,
parametry Wi-Fi, status SUPLA, przyczynę ostatniego restartu i liczniki ramek
CIO/DSP.
