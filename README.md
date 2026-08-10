# TTGO T-Internet-COM — 4G → WiFi Router

ESP-IDF firmware that turns a [LILYGO TTGO T-Internet-COM](https://github.com/Xinyuan-LilyGO/LilyGo-T-Internet-COM)
with a SIMCom **A7670E** Mini-PCIE modem into a small LTE router: the modem
dials PPP, the ESP32 runs NAPT, and WiFi clients get internet through a SoftAP.

```
[4G network] ←→ [A7670E] ←→ [UART1 PPP] ←→ [ESP32 lwIP NAPT] ←→ [WiFi SoftAP] ←→ [clients]
```

Everything a user normally configures — SSID, WiFi password, channel, LAN
subnet, APN, SIM PIN, admin login — is stored in NVS and edited from a built-in
web UI. No credentials live in the source.

## First boot

1. Flash the firmware and power the board with the SIM inserted.
2. Join the WiFi network **`TTGO-4G-XXXX`** (the suffix is the last two bytes of
   the board's MAC) with the password **`12345678`**.
3. Open **http://192.168.4.1/** and log in with **`admin` / `admin`**.
4. Set your APN under *Internet*, and change the WiFi password and the admin
   login under *WiFi* and *System*. The board reboots after each save.

The defaults are deliberately well-known — change them before putting a board
into service. The factory values live at the top of
[src/router_config.h](src/router_config.h) and can be overridden at build time
(`-DROUTER_DEFAULT_APN=\"indosatgprs\"`) if you flash a batch of boards.

## Web UI

| Tab | Settings |
| --- | --- |
| Status | WAN state, IP/gateway/DNS, operator, signal, clients, uptime, heap |
| WiFi | SSID, password, channel, max clients, hidden SSID, LAN IP and netmask |
| Internet | APN, PPP username/password, SIM PIN |
| System | Admin username/password, reboot, factory reset |

The UI is protected by HTTP Basic auth over plain HTTP. That keeps other
clients on the LAN out of the settings page; it is not confidential against
someone who can already decrypt your WiFi. Treat the WPA2 key as the real
boundary.

REST endpoints, if you'd rather script it: `GET /api/status`,
`GET|POST /api/config`, `POST /api/reboot`, `POST /api/factory-reset`.

The WiFi AP and the config UI start before the modem and stay up regardless of
it, so a wrong APN or a missing SIM can always be fixed from a browser.

## Building

PlatformIO (what this repo is set up for):

```bash
pio run -t upload
pio device monitor
```

Plain ESP-IDF works too (`idf.py build flash monitor`), with one caveat:
PlatformIO does not execute the CMake rule that turns `EMBED_TXTFILES` into an
assembly stub, so `src/www/index.html` is listed **both** in
[src/CMakeLists.txt](src/CMakeLists.txt) and as `board_build.embed_txtfiles` in
[platformio.ini](platformio.ini). Keep the two in sync.

`sdkconfig.*` is generated and git-ignored — edit
[sdkconfig.defaults](sdkconfig.defaults) instead, and delete the generated
`sdkconfig.ttgo-t-internet-com` afterwards, or your change silently never
reaches the chip.

## Hardware notes

| Signal | GPIO | Note |
| --- | --- | --- |
| PCIE-TX | 33 | ESP32 UART1 TX → modem RX |
| PCIE-RX | 35 | modem TX → ESP32 UART1 RX (input-only pin, fine for RX) |
| PCIE-RST | 32 | present on the schematic, but pulsing it does **not** reset this module |

* **Baud is fixed at 115200.** `AT+IPR=460800` was tried: the modem did not
  follow, the link filled with framing garbage, and it needed a power-cycle.
  The Mini-PCIE slot has no RTS/CTS to make a faster rate reliable, so WAN
  throughput tops out around 11 kB/s.
* **A silent modem is usually still in PPP data mode** from a previous run — an
  ESP32 reset does not touch it. The firmware probes for that and sends the
  `+++` escape before falling back to `AT+CRESET`.
* The `sdkconfig.defaults` comments explain the lwIP tuning (TCPIP mailbox size,
  UART ring sizes) that fixed the `pppos_input_tcpip ERR_MEM` flood. Read them
  before changing those values.

## Known limitations

* No captive-portal DNS redirect — browse to the LAN IP directly.
* Settings apply on reboot, not live.
* IPv4 only.
