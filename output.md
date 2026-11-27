# PocketNode ESP32-S3 — Headless Firmware QA Report

**Device:** ESP32-S3 on COM17 @ 115200 baud
**Firmware:** PocketNode `v0.0.0` (ESP-IDF v4.4.5), app partition `app0`
**Method:** temporary `pyserial` harness (kept outside the git working tree, deleted after the run); 4 passes over COM17
**Scope:** every compiled headless command except SD/file ops (`save`, `load`, `ls`, `upload`) and GPS (`gpsdata`, `gps`, `gpspoi`, `gpstracker`, `nmea`)
**Safety:** passive only — no `attack`, `blespam`, `spoofat`, `evilportal`, `karma` was ever sent. Each scan test ended with `stopscan` + ~1.5 s settle.

## Result summary

| # | Group | Command | Result | Evidence |
|---|-------|---------|--------|----------|
| 1 | System | `help` | **PASS** | 58 command lines printed, no truncation |
| 2 | System | `info` | **PASS** | `Firmware: PocketNode \| v0.0.0 \| app0 \| IDF v4.4.5`, MACs printed |
| 3 | System | `settings` | **PASS** | 630 B settings table, no crash |
| 4 | System | `channel` | **PASS** | reports `1` |
| 5 | System | `channel -s 6` | **PASS** | reports `6`, radio retunes |
| 6 | Wi-Fi | `scanall` | **PASS** | 6 APs + 3 station→AP records streamed |
| 7 | Wi-Fi | `list -a` | **PASS** | `[0]..[5]` with CH and RSSI |
| 8 | Wi-Fi | `list -c` | **PASS** | executes cleanly (0 clients at that moment) |
| 9 | Wi-Fi | `list -s` | **PASS** | executes cleanly |
| 10 | Wi-Fi | `ssid -a -n TestNode` | **PASS** | echoes `TestNode`; appears in `list -s` |
| 11 | Wi-Fi | `select -a 0,1` | **PASS** | `2 selected, 0 unselected`; `(selected)` markers appear |
| 12 | Wi-Fi | `clearlist -a` | **PASS** | list back to empty |
| 13 | Sniffer | `sniffbeacon` | **PASS** | 416 B of beacon lines (AZIR, ch 1), clean stop |
| 14 | Sniffer | `sniffdeauth` | **PASS** | starts/stops cleanly; 0 deauth frames (quiet band) |
| 15 | Sniffer | `sniffpmkid` | **PASS** | live counters: Mgmt 12, Data 0, Beacon 8, EAPOL 0, `DEAUTH TX: FALSE` |
| 16 | Sniffer | `sniffsae` | **PASS** | starts/stops cleanly; no SAE frames in range |
| 17 | Sniffer | `remoteid scan` | **FAIL** | firmware abort — see defect section |
| 18 | Sniffer | `remoteid list` | **PASS** | prints cleanly |
| 19 | Sniffer | `csimotion start` | **UNSUPPORTED** | not in compiled help (no `csimotion` command) |
| 20 | Sniffer | `mactrack` | **PASS** | 858 B stream, clean stop |
| 21 | Sniffer | `pingscan` | **PASS (no-op)** | returns prompt with no output — needs an associated network, doesn't crash |
| 22 | BLE | `sniffbt` | **PASS** | 13 BLE advertisements captured immediately (RSSI −27…−77) |
| 23 | BLE | Wi-Fi→BLE switch | **PASS** | no heap corruption, no brownout, no panic |
| 24 | System | `reboot` | **PASS** | banner `PocketNode / v0.0.0` after `rst:0xc` (normal SW reset), responsive |

Totals: **PASS 22, FAIL 1, UNSUPPORTED 1** (`scanap` is not compiled; `scanall` was used).

Compiled command set reported by `help` on this build:

```
add arpscan attack blespam brightness channel clearlist cloneapmac clonestamac
evilportal findmy foxhunt geofence gps gpsdata gpspoi gpstracker info join karma
led list load ls mactrack nmea packetcount pingscan portscan protocolinfo randapmac
randstamac reboot recon remoteid save scanall select settings sniffbeacon sniffbt
sniffdeauth sniffmultissid sniffpinescan sniffpmkid sniffprobe sniffpwn sniffraw
sniffsae sniffskim spoofat ssid stopscan update upload
```

Note: no `csimotion`, no `scanap`, no `wardrive` in this build, so those were reported as unsupported rather than tested.

## Defect: `remoteid scan` aborts the firmware (intermittent)

```
#remoteid scan
Starting Remote ID scan. Stop with stopscan
RID scheduler: NAN dwell CH6/CH149 when supported; beacon sweep constrained by configured radio plan

abort() was called at PC 0x420871c6 on core 1
Backtrace: 0x40377322 0x4037f3cd 0x40385659 0x420871c6 0x4208700f 0x420677fc 0x4202611e 0x420177d4 ...
rst:0xc (RTC_SW_CPU_RST)   <- device rebooted itself
```

Decoded with `xtensa-esp32s3-elf-addr2line -pfiaC -e build/esp32_pocketnode.ino.elf`:

```
panic_abort (esp_system/panic.c:408)
 <- esp_bt_controller_enable (bt/controller/esp32c3/bt.c:1479)
 <- NimBLEDevice::init (NimBLEDevice.cpp:770)
 <- WiFiScan::setupRemoteIdBle()   at esp32_pocketnode/WiFiScan.cpp:7405
```

### Root cause

Not a Remote-ID parsing bug. `WiFiScanRemoteId.h` re-initialises the Wi-Fi driver, sets `WIFI_MODE_NULL`, prints the `RID scheduler:` line and then calls `setupRemoteIdBle()`, which calls `NimBLEDevice::init()`. Enabling the BT controller there enters the coexistence hook (`coex_enable` → `coex_core_enable`) and aborts while Wi-Fi is in that just-reconfigured / NULL-mode state. Both coex options are enabled in this platform build (`CONFIG_ESP32_WIFI_SW_COEXIST_ENABLE=y`, `CONFIG_SW_COEXIST_ENABLE=y`).

### Reproducibility

**1 crash in 5 `remoteid scan` invocations** — it is a race, not deterministic:

- cold boot → `remoteid scan` ×3, all crash-free
- full sniffer sequence (`scanall` → lists → sniffs) → crashed once, then crash-free ×2 more

Because the same command sometimes reboots the unit, this is a field-reliability problem, not just a test artifact.

### Suggested investigation (not applied)

- Tear the Wi-Fi sniffer down (`esp_wifi_stop` / `esp_wifi_deinit`) before `NimBLEDevice::init`, or
- Defer BLE init until after the first channel-schedule tick of the RID scanner, and
- Guard `esp_bt_controller_enable` / `NimBLEDevice::init` against re-entry when the BT controller is already enabled (`sniffbt` previously enabled it).

## Non-defects worth recording

- `reboot` initially flagged because the boot log contains `rst:0xc (RTC_SW_CPU_RST)` — that is the normal reset reason for a software reboot, not a panic.
- `sniffdeauth` / `sniffsae` / `remoteid list` produced no frames: no deauth, SAE or RID traffic was in the air at this location. This proves the listeners start and stop cleanly; it does not prove frame decoding.
- `sniffbt -t ibeacon` returned nothing because no iBeacons were nearby; unfiltered `sniffbt` captured 13 devices immediately, so the BLE path itself works.
- `pingscan` prints nothing without an associated network (station mode never joined). No crash and `stopscan` still works.
- `settings` was read only; the only state change made was the in-RAM `TestNode` SSID, cleared with `clearlist`.

## What was not tested

- SD-card features (`save`, `load`, `ls`, `upload`) and GPS commands — excluded per instructions.
- Active/offensive commands (`attack`, `blespam`, `spoofat`, `evilportal`, `karma`, `findmy`) — excluded by the passive-only rule.
- Any feature requiring a joined network (`join`-dependent `pingscan`/`portscan`/`arpscan` behaviour), and long-duration soak testing.
