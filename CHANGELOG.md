# Changelog

All notable changes to the Lay-Z-Spa SUPLA fork are documented here.

## [v1.0.0-supla] - 2026-07-23

First public SUPLA release, based on the `development_v4` branch of
[visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA](https://github.com/visualapproach/WiFi-remote-for-Bestway-Lay-Z-SPA).

### Added

- Native outbound SUPLA Cloud integration over TLS.
- SUPLA thermostat with whole-degree targets, countdown timer and locally
  evaluated weekly program.
- Conditional SUPLA channels matched to the selected SPA hardware.
- Polish web interface and Polish default SUPLA channel captions.
- Browser-based updates for both firmware and LittleFS, with an embedded
  recovery page.
- Factory reset that clears Wi-Fi, MQTT, SUPLA, hardware configuration,
  schedules, statistics and runtime state while preserving installed firmware
  and web assets.
- SUPLA status in the local web interface and a read-only `/health/`
  diagnostics endpoint.

### Changed

- Default hardware profile for the tested six-wire 2021 AirJet controller.
- Wi-Fi reconnect behavior, bounded SUPLA network waits and runtime memory
  handling for unattended operation.
- Web navigation and responsive layout.
- LittleFS build now starts from a clean generated directory.
- Firmware version is reported as `v1.0.0-supla`.

### Removed

- Obsolete firmware-update checker.
- Generic LittleFS directory, file upload and file removal pages.

[v1.0.0-supla]: https://github.com/mateok/Lay-Z-Spa-SUPLA/releases/tag/v1.0.0-supla
