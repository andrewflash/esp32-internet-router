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
| Status | WAN state, IP/gateway/DNS, operator, signal, clients, uptime, last reset, heap |
| WiFi | SSID, password, channel 1-11, max clients, hidden SSID, LAN IP and netmask |
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

The DHCP server advertises fallback DNS while cellular is starting. When the
carrier supplies DNS, connected stations are re-associated once so they renew
their leases instead of remaining connected with stale DNS. While PPP is up,
the router performs a DNS data-plane check every 30 seconds; three consecutive
failures trigger a PPP redial even when the modem never reports a lost-IP event.
The last ESP32 reset reason is retained on the status page to distinguish a
software restart or watchdog from a brownout.

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
| PWRKEY | 32 | modem power control, **asserted HIGH** — see below |

* **GPIO32 is PWRKEY, not a reset.** The board pinout image labels it
  "PCIE-RST", which is misleading: LilyGo's own examples define it as
  `MODEM_PWRKEY`, and it is asserted by driving the ESP32 pin **HIGH** (the
  module's PWRKEY is pulled up to VBAT internally and triggers on a LOW, so
  there is an inverting stage between the two). A LOW pulse asserts nothing at
  all. Per the [A7672X/A7670X hardware design][hw] §3.2, the module needs
  ~50 ms asserted to power **on** and **≥2.5 s** to power **off** — a short
  pulse on a running module is ignored by design.
* **A silent modem is usually still in PPP data mode** from a previous run — an
  ESP32 reset does not touch it. Bring-up escalates: AT sync with a `+++`
  escape → PWRKEY power-on pulse → full PWRKEY power cycle. `AT+CRESET` and
  `AT+CPOF` are only useful while the modem still answers AT; the power cycle
  is the only recovery that reaches a wedged module. Do not cut the modem's
  VBAT to force the issue — SIMCom warns it can corrupt the module's flash.
* **Baud is fixed at 115200.** `AT+IPR=460800` was tried: the modem did not
  follow, the link filled with framing garbage, and it needed a power-cycle.
  The Mini-PCIE slot has no RTS/CTS to make a faster rate reliable, so WAN
  throughput tops out around 11 kB/s.

[hw]: https://files.waveshare.com/wiki/A7670E-Cat-1-GNSS-HAT/A7672X_A7670X_Series_Hardware_Design_V1.03.pdf
* The `sdkconfig.defaults` comments explain the lwIP tuning (TCPIP mailbox size,
  UART ring sizes) that fixed the `pppos_input_tcpip ERR_MEM` flood. Read them
  before changing those values.

## Known limitations

* No captive-portal DNS redirect — browse to the LAN IP directly.
* Settings apply on reboot, not live.
* IPv4 only.
