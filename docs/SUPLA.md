# SUPLA Cloud integration

The Lay-Z-Spa module can connect directly to SUPLA Cloud, so remote control does
not require a public IP address, a private MQTT broker, or Home Assistant. The
integration uses SUPLA's native outgoing device protocol over TLS. It does not
route SUPLA through the firmware's existing MQTT integration.

The local web interface and MQTT integration remain available when SUPLA is
enabled.

## Before you start

You need:

- a SUPLA Cloud account and the SUPLA mobile app;
- the ESP8266 connected to a Wi-Fi network with Internet and DNS access;
- access to an NTP server, because TLS certificate validation starts only after
  the controller has obtained a valid clock;
- both the firmware and the matching LittleFS image installed. The SUPLA setup
  page is part of LittleFS, so updating only `firmware.bin` is not sufficient;
- registration of new devices temporarily enabled in SUPLA Cloud.

No SUPLA account password is entered into or stored by the module. Provisioning
uses the account email, a device GUID, and a randomly generated device
authorization key. The local setup AP and ArduinoOTA use the device password
configured in firmware (`layzspam0dule` by default); it is not a SUPLA
credential and the user is not asked to create another password. The browser
update page intentionally does not require a separate password.

## First-time provisioning

1. Power the unpaired module. Its first-time provisioning window remains open
   for 15 minutes after boot. Connect to the `Lay-Z-Spa-<chip-id>` setup access
   point with the legacy first-claim password `layzspam0dule`.
2. Open `http://192.168.4.2/wifi.html`, enter the home Wi-Fi credentials, and
   save them. You can stay on the setup access point until the final restart.
3. In SUPLA Cloud, enable registration of new devices for the account that will
   own the spa.
4. Open `http://192.168.4.2/supla.html` from the menu.
5. Select **Enable SUPLA** and enter the SUPLA account email. No SUPLA account
   password or additional administrator password is required.
6. Leave **SUPLA server** empty for SUPLA Cloud autodiscovery. A server hostname
   should be entered only for a self-hosted or explicitly assigned SUPLA server;
   do not include `https://`, a port, or a URL path. Custom servers must present
   a certificate trusted by the SUPLA client; self-signed TLS is not accepted.
7. Select **Save & restart**. After restarting, the module joins the configured
   Wi-Fi network and registers with SUPLA Cloud.
8. Confirm that the new `Lay-Z-Spa` device is online in SUPLA, then disable
   registration of new devices again.

For an already networked controller,
`http://layzspa.local/supla.html` (or the IP address assigned by the router)
remains available for status, enabling/disabling SUPLA, changing the account,
and resetting the pairing. The saved e-mail is intentionally not returned by
the API; leave the e-mail field empty to preserve the existing assignment.
Changing the e-mail or server generates a new device identity.

## Channels

Channel numbers are intentionally stable. Unsupported features remain in their
assigned positions so adding or removing a HydroJet or ambient sensor cannot
renumber every later channel.

| No. | Default caption | SUPLA role | Direction / unit |
|---:|---|---|---|
| 0 | Water temperature | Thermometer | Read-only, °C |
| 1 | SPA thermostat | Thermostat | Water temperature, 20–40 °C target, heating control |
| 2 | SPA power | Switch | Read/write when the selected spa model supports power control |
| 3 | Filter pump | Switch | Read/write |
| 4 | AirJet bubbles | Switch | Read/write on AirJet-equipped models |
| 5 | HydroJet | Switch | Read/write on HydroJet-equipped models |
| 6 | Virtual temperature | Thermometer | Read-only, °C |
| 7 | Ambient temperature | Thermometer | Read-only, °C; requires the configured external sensor |
| 8 | Chlorine age | Measurement | Read-only, days |
| 9 | Filter replacement age | Measurement | Read-only, days |
| 10 | Filter cleaning age | Measurement | Read-only, days |
| 11 | Filter rinse age | Measurement | Read-only, days |
| 12 | Pump runtime | Measurement | Read-only, hours |
| 13 | Heating runtime | Measurement | Read-only, hours |
| 14 | AirJet runtime | Measurement | Read-only, hours |
| 15 | HydroJet runtime | Measurement | Read-only, hours |
| 16 | Energy today | Measurement | Read-only estimate, kWh |
| 17 | Energy total | Measurement | Read-only estimate, kWh |
| 18 | Current power | Measurement | Read-only estimate, W |
| 19 | Time to ready | Measurement | Read-only estimate, hours |
| 20 | Error code | Measurement | Read-only numeric controller error; `0` means no error |
| 21 | Reset chlorine age | Momentary action | Write-only reset trigger |
| 22 | Reset filter replacement age | Momentary action | Write-only reset trigger |
| 23 | Reset filter cleaning age | Momentary action | Write-only reset trigger |
| 24 | Reset filter rinsing age | Momentary action | Write-only reset trigger |

The mobile app may render temperatures in the unit selected in the user's SUPLA
preferences. The firmware always sends canonical Celsius values to avoid a unit
change on the physical panel changing channel semantics.

Energy and power values are estimates derived from the component power levels
configured in the firmware; they are not readings from a certified energy
meter. Time-to-ready is also a model-based estimate and can be unavailable when
there is not enough calibration data or the target cannot be reached under the
configured assumptions.

Switch commands are asynchronous. The SUPLA channel is updated from the state
confirmed by the spa controller, rather than optimistically assuming that a
command succeeded. Some six-wire controllers can take several seconds to echo a
panel command. Avoid repeatedly toggling a channel while the first operation is
still pending.

## Thermostat timer and weekly program

The SPA thermostat supports both automation modes exposed inside the native
SUPLA thermostat tile:

- **Countdown timer** temporarily applies the selected heating mode and target
  for the requested duration. When it expires, the thermostat restores the mode
  that was active before the timer.
- **Weekly program** is configured in SUPLA but downloaded to the ESP8266 and
  evaluated locally in 15-minute quarters. Once the controller has obtained the
  correct time from SUPLA, a temporary Internet outage does not stop an already
  downloaded weekly program. Rebooting without any source of valid time delays
  scheduled execution until the clock is synchronized again.

Each local timer expiry and weekly-program transition is translated to the
original SPA command queue as a whole-degree target followed by the requested
heater-enable state. The physical SPA controller remains responsible for
cycling the heating element as the water reaches the target.

These thermostat-local modes are distinct from general schedules created under
SUPLA Cloud's **Automation** section. General cloud schedules send a command at
the configured time and therefore require the device to be reachable through
SUPLA Cloud at that moment.

The `Time to ready` measurement is informational only. It is never used to
start, stop, or offset a thermostat program.

Channel captions are initial defaults. SUPLA preserves names customized by the
user and may also preserve the original caption after a firmware update. Reset
pairing to create a new cloud device only when a genuinely new identity is
required, not merely to rename channels.

## Disable, move, or reset the integration

- These operations can be performed from the controller's normal LAN page at
  `/supla.html`; a setup-AP reset is not required.
- To suspend cloud access without losing the assignment, clear **Enable
  SUPLA**, and select **Save & restart**. The email and device identity remain
  stored for later re-enabling.
- Changing the account email creates a new device identity. Enable new-device
  registration on the destination SUPLA account before saving.
- **Reset pairing** disables SUPLA, clears its email and server assignment, and
  generates a new GUID and authorization key. The old device is not deleted
  from SUPLA Cloud; remove it manually from the old account.
- **Factory settings**, available as the last item in the local menu, removes
  the home Wi-Fi credentials, MQTT and SUPLA configuration, hardware and SPA
  settings, schedules, statistics, and runtime files, then restarts into the
  setup access point. Firmware and the built-in web interface remain installed.
  This operation cannot be undone; a confirmation is required in the browser.

## Security notes

- The cloud connection is initiated by the ESP8266 and uses TLS. No router port
  forwarding or public inbound endpoint is needed.
- The module never stores the SUPLA account password. The local status API does
  not return the configured account email, device setup password, device GUID,
  or authorization key; it reports only whether the relevant values have been
  configured.
- The generic LittleFS directory, upload, and removal pages are not exposed.
  This prevents accidental deletion of runtime data and same-origin script
  planting through uploaded web content.
- Configuration changes require a random, per-boot same-origin CSRF token. The
  token is included in both save and reset requests and prevents a third-party
  website from silently mutating a controller on the user's LAN.
- The setup AP and ArduinoOTA use the firmware device password
  (`layzspam0dule` by default). The browser update page at `/update.html` does
  not ask for a password and its upload endpoints are intentionally local-only
  by network placement, not by application authentication.
- The web interface uses HTTP and SUPLA configuration is editable from the home
  LAN. Keep the controller on a trusted LAN or access it through a VPN; never
  expose the interface with router port forwarding. Anyone who can reach the
  local browser interface can start a firmware or LittleFS update.
- Someone with physical access to the ESP8266 flash can extract stored device
  credentials. Reset the SUPLA pairing and remove the old cloud device if the
  module is lost or transferred.
- Keep the SUPLA Cloud new-device registration window open only for the time
  needed to pair this module.

## Hardware acceptance test

Disconnect mains power before opening or rewiring a spa controller. Initial
functional testing should be supervised at the spa, with the original panel
within reach.

1. Verify on the SUPLA setup page that pairing is configured, the client is
   running, and the cloud status is connected/registered.
2. Compare water, target, virtual, and ambient temperatures with the local web
   interface and physical panel.
3. Change the target by one degree in SUPLA. Confirm the physical panel receives
   it and SUPLA settles on the value reported back by the controller.
4. Test power, filtration, heating, AirJet, and HydroJet one at a time. Skip
   features not supported by the selected hardware model.
5. Change each supported state from the physical panel and confirm SUPLA follows
   it without issuing a second command.
6. Record the four service ages, trigger one reset channel, and confirm only the
   selected counter returns close to zero. Repeat deliberately for the remaining
   counters.
7. Compare runtimes and energy estimates with the local web interface. Validate
   that daily energy rolls over after a valid NTP time has been obtained.
8. Restart the ESP8266 and the router. Confirm automatic Wi-Fi and SUPLA
   reconnection without creating a duplicate device.
9. Temporarily block Internet access while keeping Wi-Fi available. Local web
   control and optional local MQTT should continue working; SUPLA should recover
   after Internet access returns.
10. Run the controller for at least 24 hours while observing free heap, reconnect
    behavior, physical-panel synchronization, and SUPLA command latency.

Record the spa model, controller wiring type, firmware revision, and result of
every step. A successful test on one pump family does not prove power-control or
HydroJet behavior on all Lay-Z-Spa variants.

## Building and installing both images

From the `Code` directory:

```sh
pio run -e nodemcuv2
pio run -e nodemcuv2 -t buildfs
pio run -e nodemcuv2 -t upload
pio run -e nodemcuv2 -t uploadfs
```

The repository's existing OTA workflow can also upload both `firmware.bin` and
`littlefs.bin`. The default ArduinoOTA password is `layzspam0dule` unless
`WM_AP_PASSWORD` was changed before compilation. See the main README and the
Polish installation guide for the browser update workflow and recovery
considerations.

## Protocol references

- [SUPLA Device library v26.4](https://github.com/SUPLA/supla-device/releases/tag/v26.4)
- [SUPLA Device Arduino quick start (v26.4)](https://github.com/SUPLA/supla-device/blob/v26.4/docs/quickstart/arduino.md)
- [SUPLA Cloud](https://cloud.supla.org/)
