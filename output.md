# PocketNode ESP32-S3 — Phone-offloaded GPS + storage (serial PCAP / WiGLE) and dual-port CLI

**Device:** ESP32-S3, UART0 on COM17 @ 115200 (CH343), firmware `v0.0.0`, ESP-IDF v4.4.5, arduino-esp32 2.0.11
**Changed files:** `configs.h`, `GpsInterface.{h,cpp}`, `CommandLine.{h,cpp}`, `WiFiScan.{h,cpp}` (+ new `PocketNodeSerial.{h,cpp}`)
**Status:** implemented, compiled, flashed to COM17, serial QA passed — **uncommitted**, nothing staged, nothing pushed

---

## 1. Goal

Run the headless ESP32-S3 as a capture engine for a phone over serial, with no SD card, no GPS module and
no screen:

1. Accept NMEA from the host (phone GPS) and keep a live fix.
2. Stream PCAP (and WiGLE CSV) over serial by default when `HAS_SD` is absent.
3. Let either the laptop (UART0) or the phone (native USB-C) drive the board at the same time.

---

## 2. Design

### 2.1 GPS without a GPS UART

`POCKETNODE_S3` previously had no `HAS_GPS` at all, so every GPS command was compiled out. It now defines:

```c
#define HAS_GPS
#define GPS_NMEA_INJECTION
```

`GPS_NMEA_INJECTION` is a new switch inside `GpsInterface`:

- `begin()` sets `gps_enabled = true`, initializes the queue and installs the unknown-sentence handler,
  then returns before touching `Serial2` (this target has no `GPS_TX`/`GPS_RX` at all — they are not
  defined for `POCKETNODE_S3` in `configs.h`).
- `main()` keeps the shared validity/`setGPSInfo()` tail but skips the `Serial2` drain.
- `probeBaud()`, `setGpsTo115200From9600()`, `initGpsBaudAndForce115200()` and the `Serial2` global are
  compiled out in this mode.

New public entry point:

```cpp
void GpsInterface::injectNmea(const String& sentence) {
  String line = sentence;
  line.trim();
  if (!line.startsWith("$")) return;

  // Drop any half-received sentence left over from a previous connection so
  // the first injected line cannot inherit a broken parser state.
  nmea.process('\r');

  for (size_t i = 0; i < line.length(); i++)
    nmea.process(line.charAt(i));

  nmea.process('\r');
  nmea.process('\n');

  this->gps_enabled = true;
  this->nmea_sentence = line;
  this->setGPSInfo();
}
```

The leading `\r` matters: without it the **first** sentence after boot was silently discarded (observed
on hardware, see §4.1). MicroNMEA 2.0.6 also requires the fixed `hhmmss.ss` time field — `hhmmss,` makes it
walk into the latitude field and fail.

### 2.2 Dual-port serial

`PocketNodeSerial` is a `Stream` that owns both ports:

- `write()` → `Serial.write()` always, plus `USBSerial.write()` when the core exposes it.
- `begin()` → real `Serial.begin(baud)` plus `pocketnode::beginUsb(baud)`.
- USB is enabled only when the core is built with the hardware CDC (`ARDUINO_USB_MODE=1`) and UART0 keeps
  `Serial` (`ARDUINO_USB_CDC_ON_BOOT=0`); `POCKETNODE_HAS_USB_SERIAL` is derived from those macros.
- `USBSerial.setTxTimeoutMs(0)` so a host that stops reading makes writes drop, never block the scan loop.

Existing code keeps calling `Serial.*` unchanged; `configs.h` appends

```c
#include "PocketNodeSerial.h"

#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
  #define Serial pocketnode_serial
#endif
```

so every translation unit that includes `configs.h` (all PocketNode sources) is mirrored without touching
hundreds of call sites. Input is **not** merged through the proxy: `CommandLine::pollPort()` reads each real
port with its own 512-byte line buffer, so two hosts cannot corrupt each other's lines and a port with no host
costs nothing.

```cpp
void CommandLine::main(uint32_t currentTime) {
  (void)currentTime;
  this->pollPort(pocketnode::uartStream(), this->uart_line);
  this->pollPort(pocketnode::usbStream(), this->usb_line);
}
```

### 2.3 Serial storage as the default

`CommandLine::runCommand()` previously set `wifi_scan_obj.save_serial` only from `-serial`. Now:

```cpp
#ifdef HAS_SD
  wifi_scan_obj.save_serial = this->argSearch(&cmd_args, "-serial") != -1;
#else
  wifi_scan_obj.save_serial = true;
#endif
```

`Buffer::openFile()` already tolerates `fs == NULL` and streams through `Buffer::saveSerial()`, which frames
bytes as `[BUF/BEGIN]<payload>[BUF/CLOSE]` and now goes through the proxy (so PCAP/WiGLE reach both ports).
A capture consumer strips the markers; the marker is required so console text cannot corrupt binary PCAP.

### 2.4 WiGLE from `scanall`

`scanall` is `WIFI_SCAN_AP_STA` (`RunAPScan`), which used to produce a PCAP only. Now, when there is no SD
card and a fix is already present, it opens a WiGLE log, emits the standard header and appends AP rows from
`apSnifferCallbackFull()`:

```cpp
#ifndef HAS_SD
  this->wigle_serial_active = false;
  #ifdef HAS_GPS
    if (scan_mode == WIFI_SCAN_AP_STA &&
        gps_obj.getGpsModuleStatus() && gps_obj.getFixStatus()) {
      startLog("wardrive");
      buffer_obj.append(this->header_line);
      this->wigle_serial_active = true;
    }
  #endif
#endif

if (!this->wigle_serial_active) {
  if (scan_mode != WIFI_SCAN_AP_STA) startPcap("ap");
  else                               startPcap("ap_sta");
}
```

Row shape (14 columns, matching `WIGLE_CSV_COLUMNS`):

```
06:33:c2:dd:c1:dd,DOUAE,[WPA2_PSK],2094-03-23 12:35:19,6,2437,-60,48.1172981,11.5166664,545.40,2.25,,0,WIFI
```

**Known interaction (pre-existing):** running `sniffbeacon` immediately after `scanall`/`stopscan` captures
no frames on this firmware; a fresh radio state captures normally. The QA therefore runs the PCAP capture
before the AP/STA scan. This was not changed.

---

## 3. Build and flash

```
cd /p/tools
./arduino-cli.exe --config-file arduino-cli.yaml compile --jobs 2 \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=min_spiffs,FlashSize=4M,USBMode=hwcdc,CDCOnBoot=default" \
  --warnings default --build-path P:/build P:/esp32_pocketnode
```

Result: exit 0, `Sketch uses 1104833 bytes (56%)`, `Global variables 69584 bytes (21%)`.
`USBMode=hwcdc` is the menu default for `esp32s3`, so the board has the native CDC endpoint; `CDCOnBoot`
stays disabled so `Serial` remains UART0. The tree also compiles with the previous FQBN
(`PartitionScheme=min_spiffs,FlashSize=4M`), exit 0.

```
./arduino-cli.exe --config-file arduino-cli.yaml upload -p COM17 \
  --fqbn "esp32:esp32:esp32s3:PartitionScheme=min_spiffs,FlashSize=4M,USBMode=hwcdc,CDCOnBoot=default,UploadSpeed=921600" \
  --input-dir P:/build
```

`Hash of data verified.` — upload exit 0.

Only pre-existing warnings remain (beacon-spam `random()` narrowing in `WiFiScan.{h,cpp}`, and
`SDInterface.cpp:218` missing return in `getFile()`; that file was not modified). No warning comes from a
file changed here.

---

## 4. Verification (COM17, temporary pyserial script + `Test/`, both deleted)

### 4.1 NMEA injection

Both the raw-line path and the `nmea <sentence>` command word:

```
[NMEA] GP Fix=Yes Sats=8 Lat=48.1172981 Lon=11.5166664 Alt=545.4 Acc=2.2 UTC=2094-03-23 12:35:19
```

`gpsdata` then reports the same live values (`Fix: Yes`, `Sats: 8`, `Lat: 48.1172981`, `Lon: 11.5166664`).

Regression found during QA and fixed: the first sentence after every boot was rejected (`Fix=No`,
`Lat=999.0000000`); the second identical sentence parsed. Cause: parser state inherited from boot. Fix:
`injectNmea()` now terminates the buffer before feeding a new sentence (see §2.1). After the fix the first
raw `$GPGGA` already reports `Fix=Yes`.

### 4.2 PCAP over serial

`sniffbeacon`, 6 s, frames reassembled from `[BUF/BEGIN]…[BUF/CLOSE]`:

- 47,357 bytes, global header magic `d4c3b2a1`
- 137 records; every record's `incl_len` walks the file to exactly the end (no truncation)
- no `SD Card NOT Supported`/mount errors, no panic

### 4.3 WiGLE over serial

`scanall`, 10 s, with the injected fix:

- 11,444 bytes, starts with `WigleWifi-1.6,appRelease=v0.0.0,…`
- standard column header line, then non-empty 14-field `…,WIFI` rows carrying the injected
  coordinates/altitude/accuracy and real BSSIDs/SSIDs/channels/frequencies
- header appears exactly once

### 4.4 Crash checks

No `Guru Meditation`, `abort() was called`, `Backtrace:`, `assert failed` or `Stack canary watchpoint` in
any transcript.

---

## 5. Limitations / not verified

- **Native USB mirroring was not exercised.** The USB-C port is connected to the Android phone, so the PC
  only enumerates COM17 (CH343, `VID:PID=1A86:55D3`). The USB path compiles into the image and command input
  from either port is implemented, but a phone-side check is still required.
- PCAP/WiGLE are validated by reassembling the `[BUF/…]` frames; a real capture app must strip those
  markers. The payloads themselves are a clean PCAP stream and a clean CSV stream.
- `scanall` only streams WiGLE when a fix already exists at scan start; otherwise it still writes PCAP.
- PCAP/WiGLE throughput is bounded by 115200 UART. `Buffer` drops packets when its double buffer fills
  (pre-existing behaviour).

---

## 6. Working tree

```
 M esp32_pocketnode/CommandLine.cpp
 M esp32_pocketnode/CommandLine.h
 M esp32_pocketnode/GpsInterface.cpp
 M esp32_pocketnode/GpsInterface.h
 M esp32_pocketnode/WiFiScan.cpp
 M esp32_pocketnode/WiFiScan.h
 M esp32_pocketnode/configs.h
?? esp32_pocketnode/PocketNodeSerial.cpp
?? esp32_pocketnode/PocketNodeSerial.h
```

```
 esp32_pocketnode/CommandLine.cpp  | 94 +++++++++++++++++++++++++++++++++------
 esp32_pocketnode/CommandLine.h    |  7 ++-
 esp32_pocketnode/GpsInterface.cpp | 40 +++++++++++++++++
 esp32_pocketnode/GpsInterface.h   |  4 ++
 esp32_pocketnode/WiFiScan.cpp     | 41 +++++++++++++++--
 esp32_pocketnode/WiFiScan.h       |  2 +
 esp32_pocketnode/configs.h        | 15 ++++++
 7 files changed, 185 insertions(+), 18 deletions(-)
```

No commit, no push. Temporary `tools/serial_qa.py`, `tools/apply_edits.py`, `Test/` and the scratch build
directory were removed; the QA transcript is kept in git-ignored `tools/qa_serial.log`.

---

## 7. Review notes

Points worth a second opinion:

1. **`#define Serial` in `configs.h`.** It is the only way to mirror ~1,000 existing call sites without
   touching them. It is guarded to builds where `ARDUINO_USB_CDC_ON_BOOT` is 0 (so the core's
   `extern HWCDC Serial;` / `extern USBCDC Serial;` declarations are never renamed), and it only takes
   effect after `Arduino.h` has already been included, so the core's `Serial` object itself is untouched.
2. **USB write policy.** `setTxTimeoutMs(0)` prefers dropping bytes over stalling; if the phone app needs
   lossless USB capture, this should become a small ring buffer with a bounded wait instead.
3. **`HAS_GPS` on a target without GPS hardware.** It re-enables `gpspoi`/`gpstracker`/`nmea`/`wardrive` and
   their GPX/WiGLE writers; with no SD those now stream over serial too (marker-framed), which is intended
   but not covered by the QA above.
