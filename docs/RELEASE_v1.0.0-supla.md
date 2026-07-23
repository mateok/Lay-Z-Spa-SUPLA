# Lay-Z-Spa SUPLA v1.0.0-supla

Pierwsze publiczne wydanie forka
[visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA](https://github.com/visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA)
z natywną obsługą SUPLA Cloud.

## Najważniejsze zmiany

- bezpośrednie połączenie z SUPLA Cloud bez publicznego IP, Home Assistanta i
  prywatnego brokera MQTT;
- termostat SPA z temperaturą ustawianą co 1°C, timerem i programem tygodniowym;
- sterowanie zasilaniem, filtracją, grzaniem, AirJet i HydroJet zależnie od
  konfiguracji pompy;
- temperatury, liczniki serwisowe, czasy pracy, energia, moc, kod błędu i czas
  do kąpieli;
- kanały niedostępne dla wybranego modelu nie są dodawane do SUPLA;
- polski panel WWW i polskie nazwy kanałów;
- aktualizacja firmware oraz LittleFS przez przeglądarkę;
- ustawienia fabryczne z lokalnego panelu;
- diagnostyka `/health/` oraz poprawki stabilności Wi-Fi, TLS i pracy ciągłej.

Oryginalny panel WWW i opcjonalna integracja MQTT pozostają dostępne.

## Pliki

- `firmware.bin` — firmware dla ESP8266 NodeMCU 1.0;
- `littlefs.bin` — pełny obraz panelu WWW i zasobów;
- `SHA256SUMS.txt` — sumy kontrolne SHA-256.

Do pełnej pierwszej instalacji potrzebne są `firmware.bin` i `littlefs.bin`.

## Instalacja

Pełna instrukcja po polsku:
[docs/INSTALLATION_PL.md](https://github.com/mateok/Lay-Z-Spa-SUPLA/blob/v1.0.0-supla/docs/INSTALLATION_PL.md).

Pierwsza konfiguracja:

1. Połącz się z siecią `Lay-Z-Spa-<identyfikator-układu>`.
2. Hasło: `layzspam0dule`.
3. Otwórz `http://192.168.4.2/wifi.html` i zapisz domową sieć Wi-Fi.
4. Włącz rejestrację nowych urządzeń w SUPLA Cloud.
5. Na stronie `supla.html` włącz SUPLA i wpisz adres e-mail konta.
6. Zapisz ustawienia i poczekaj na urządzenie `Lay-Z-Spa` w aplikacji.

## Pochodzenie i licencja

Wydanie bazuje na gałęzi `development_v4` projektu
[visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA](https://github.com/visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA).
Kod pozostaje dostępny na licencji GNU GPL v3.
