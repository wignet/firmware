# Wi-Fi: the networks a Wignet knows

Every ESP32 and ESP8266 Wignet remembers up to **5 Wi-Fi networks**, the most
recently joined first, and joins whichever is in range. Moving a Wignet
between home and office no longer means going through setup again. Code:
`esp32/src/wifi_provisioning.cpp`. Since 2026-10-18; every firmware
target builds it, and a new board must keep it (`HARDWARE.md`).

## At start-up

1. **The list** is read: NVS on ESP32 (`Preferences`, namespace
   `wignet-wifi`, key `known`), EEPROM on ESP8266. A Wignet updated from a
   firmware without the list starts with the one network WiFiManager had
   saved, so it keeps its Wi-Fi.
2. **A scan**, then the known networks in range are tried strongest first,
   15 s each. The one joined moves to the top of the list.
3. **None in range:** the setup access point `Wignet-XXXX` opens, as before
   (WiFiManager's captive portal, `esp32/portal/`). A network saved
   there is added at the top; the oldest falls off past 5. With known
   networks, the portal closes after 3 minutes without a visitor
   (`KNOWN_RETRY_PORTAL_SEC`) and the Wignet scans and tries them again: a
   Wignet that boots before its router, after a power cut, joins it once it
   is up instead of waiting in setup. A Wignet that knows no network keeps
   its portal open until one is saved.

## The setup portal

- The access point is open, so the portal answers only its own pages
  (`registerPortalRoutes()` in `wifi_provisioning.cpp`): WiFiManager's
  firmware upload (`/update`, `/u`), `/erase`, `/restart`, `/exit`, `/close`,
  `/info`, `/param`, `/paramsave`, `/status` and `/0wifi` answer 404.
- Save posts to WiFiManager's `/wifisave`, which answers before trying the
  network. The page keeps saying "Connecting…" and asks
  `/save-status.json`: a failed try reopens the portal, which answers
  `{"state":"failed","reason":"password"|"not_found"|"other"}`, and the page
  goes back to the form with the reason. A success closes the portal; after
  40 s without an answer the page says the settings are saved.
- `/networks.json` answers 503 when the scan fails, so the page says the
  search failed rather than that no network is in range.

## While running

Wi-Fi lost: the firmware asks for the same network again every 10 s; after a
minute without it, it scans and joins any known network in range, then tries
again every minute (`loop()` in `main.cpp`, `reconnectKnownWiFi()`).

## Seeing and forgetting them from afar

Every `/schedule` carries `wifi=`, URL-encoded JSON: the network in use, its
signal and the known networks' names, never passwords:
`{"cur": "Home", "rssi": -52, "known": ["Home", "Office"]}`
(`wifiReportJson()`). The server keeps the last one (`backoffice/wifi.py`,
`devices.reported_wifi`).

- **Seeing them:** the owner's page, Settings › Wi-Fi, lists them with the one
  in use and its signal; an agent reads them with `get_wifi_networks` (REST
  `GET /api/owner/<id>/wifi`), and `get_state` carries them as `wifi`.
- **Forgetting one:** a Forget button on that page, or `forget_wifi_network
  {"ssid": ...}`. The request waits in `devices.wifi_forget`; `/schedule`
  answers `wifiForget: [names]` to a Wignet that reports its networks, it
  forgets them (`applyWifiForget()`), and its next report no longer lists
  them, which clears the request.
- **Never the network in use:** refused by the server, and by the firmware if
  asked anyway. Forgetting it would cut the Wignet off until someone set it
  up again next to it.
- The tools are listed only for a Wignet whose firmware reports its networks,
  and never in the catalogue without a device (`platform_mcp.WIFI_TOOLS`).

`/wifi/reset` on the Wignet's local web page, and the one-shot
`*_clear_wifi` builds, still forget every known network at once: the Wignet
opens its setup portal at the next boot. Adding a network is done next to the
Wignet, on its setup page: never from afar.

## Limits

- Hidden networks (no SSID broadcast) are only joined if they are the network
  saved in the ESP32's own configuration: the scan cannot see them.
- 2.4 GHz only, as before; SSIDs up to 32 bytes and passwords up to 64.
- Passwords are stored in flash in clear, as WiFiManager already did.
