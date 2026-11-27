# PocketNode ESP32-S3 — Fix: `remoteid scan` aborted inside `coex_core_enable()`

**Device:** ESP32-S3, COM17 @ 115200 (UART0 via CH343), firmware `v0.0.0`, ESP-IDF v4.4.5
**Changed files:** `esp32_pocketnode/WiFiScan.cpp`, `esp32_pocketnode/WiFiScanRemoteId.h`
**Status:** fixed, compiled, flashed, stress-tested — **uncommitted**, nothing staged, nothing pushed

## 1. Diagnosis

Original crash (from the QA run, 1 in 5 `remoteid scan` invocations):

```
abort() was called at PC 0x420871c6 on core 1
Backtrace: ... esp_bt_controller_enable -> coex_enable -> coex_core_enable -> abort
  <- NimBLEDevice::init (NimBLEDevice.cpp:770)
  <- WiFiScan::setupRemoteIdBle()   at WiFiScan.cpp:7405
```

Two defects in the original sequence:

1. **`RunRemoteIdScan()` started Wi-Fi, changed channel, and immediately enabled the BT controller.**
   `changeChannel(6)` only queues an ioctl to the Wi-Fi task; `pmTask`/`ppTask` processes it asynchronously.
   `setupRemoteIdBle()` then called `esp_bt_controller_enable()` while that channel change was still in
   flight. `coex_core_enable()` runs on the same Wi-Fi task path and aborts when the radio state it sees
   is not the state it expects — hence the intermittency (quiet band = Wi-Fi task idle fast = usually fine;
   busy band or a preceding sniffer = task busy = abort).
2. **`NimBLEDevice::init("")` was called unconditionally.** After `sniffbt` (and any earlier `remoteid scan`
   that did not fully tear down), the controller is already enabled, so this issued a second
   `esp_bt_controller_enable()`, which reaches the same aborted coexistence hook.

Both abandoned experiments are worth recording because they show what does *not* work on this IDF build:

- Setting `esp_wifi_set_ps(WIFI_PS_MIN_MODEM)` before the BT enable, while Wi-Fi is live, aborts in the
  Wi-Fi task: `abort -> pm_set_sleep_type -> wifi_set_ps_process -> ieee80211_ioctl_process -> ppTask`
  (and it made the failure deterministic: 12/12).
- Enabling BT **before** starting Wi-Fi aborts at Wi-Fi start:
  `abort -> coex_core_enable -> coex_enable -> coex_enable_wrapper (esp_adapter.c:600) -> wifi_hw_start`.
  So Wi-Fi-first is the only viable order for this firmware/IDF combination.

## 2. The fix

### `WiFiScanRemoteId.h` — `RunRemoteIdScan()`

- Tear down any leftover Wi-Fi session before starting the scanner, so `remoteid scan` never inherits a
  driver that a previous sniffer left running (`if (wifi_initialized) shutdownWiFi();`).
- After `changeChannel(6)`, **wait (bounded, 500 ms) until `esp_wifi_get_channel()` actually reports
  channel 6** before enabling the BT controller. This is a real synchronisation with the Wi-Fi task rather
  than a magic `delay()`, and it is what removes the race.

### `WiFiScan.cpp` — `setupRemoteIdBle()`

- Guard the controller bring-up with `NimBLEDevice::getInitialized()`; `NimBLEDevice::init("")` is only
  called when the controller is genuinely down. If it is already up (e.g. after `sniffbt`), the existing
  observer is stopped, its results cleared and restarted, so no second `esp_bt_controller_enable()` happens.
- Behaviour otherwise unchanged: same scan parameters (interval 83 / window 40, passive, duplicate filter
  off), same callbacks, `ble_initialized` / `ble_scanning` still set, `stopscan` still tears BLE down through
  the existing `shutdownBLE()` path (`pBLEScan->stop()`, `clearResults()`, `NimBLEDevice::deinit()`), so a
  following `scanall` / `sniffbeacon` / `sniffbt` starts from a clean state.

No changes were made to `stopscan`, `shutdownWiFi()`, `shutdownBLE()` or any other scan path.

## 3. Build & flash

```
arduino-cli compile --fqbn esp32:esp32:esp32s3:PartitionScheme=min_spiffs,FlashSize=4M --warnings default
  -> Sketch uses 1,079,221 bytes (54%), Global variables 69,060 bytes (21%), exit 0, 0 errors
arduino-cli upload -p COM17 --fqbn ...:UploadSpeed=921600  -> Hash of data verified, hard reset
```

27 warnings remain build-wide, all pre-existing and untouched by this change
(`WiFiScan.cpp:2330/2334/2353` beacon-spam `random()` narrowing, `WiFiScan.h:571/1045`). The new code
produces no warnings.

## 4. Stress test (COM17 @ 115200, pyserial harness, deleted afterwards)

All cycles: cold boot via RTS reset, `stopscan` + 1.2 s settle between scanners, every response scanned for
`abort()`, `Backtrace`, `Guru Meditation`, `Brownout`, `assert failed`, `StoreProhibited`,
`LoadProhibited`, `Stack canary`, and unexpected `ESP-ROM`/`rst:` banners.

| Batch | Pattern | Cycles | Result |
|-------|---------|--------|--------|
| 1 | cold boot → `remoteid scan` 3 s → `stopscan` | 4 | 0 aborts, 0 resets |
| 1 | cold boot → `scanall` 3 s → `sniffbeacon` 2 s → `remoteid scan` 3 s → `stopscan` | 4 | 0 aborts, 0 resets |
| 1 | cold boot → `sniffbt` 3 s → `remoteid scan` 3 s → `stopscan` | 4 | 0 aborts, 0 resets; 12–13 BLE devices captured each time |
| 2 | same three patterns with 8 s Remote ID dwell | 11 | 0 aborts, 0 resets |
| 3 | post-fix regression mix (rid → scanall → sniffbeacon → sniffbt → rid → info) | 1 | all clean: scanall found APs, 29 beacon lines, 10 BLE devices, `info` OK |
| 4 | final image re-flash sanity (3 rid cycles, one after `sniffbt`) | 3 | 0 aborts, `scanall` + `info` still work |

**Totals: 23 stress cycles + 4 final sanity cycles = 27 Remote ID starts, 0 aborts, 0 unexpected resets.**
Before the fix the same harness produced 1 abort in 5 starts on the shipped image (and 12/12 with the
abandoned PS-based approach).

Observations while testing:

- Remote ID produced no RID frames — expected, no drones/UAS broadcasting in this location; the scanner
  starts, dwells and stops without faults.
- `sniffbt` continues to work before and after a Remote ID run, so the BLE reuse/teardown path is intact.
- No heap-corruption, brownout or watchdog messages appeared anywhere in the transcripts.

## 5. `git status`

```
 M esp32_pocketnode/WiFiScan.cpp
 M esp32_pocketnode/WiFiScanRemoteId.h
```

Nothing staged, nothing committed, nothing pushed. The pyserial harnesses were deleted; only git-ignored
run logs remain under `tools/`.

## 6. `git diff`

```diff
diff --git a/esp32_pocketnode/WiFiScan.cpp b/esp32_pocketnode/WiFiScan.cpp
index 05c3a5d..cdd695d 100644
--- a/esp32_pocketnode/WiFiScan.cpp
+++ b/esp32_pocketnode/WiFiScan.cpp
@@ -7400,28 +7400,44 @@ void WiFiScan::RunBluetoothScan(uint8_t scan_mode, uint16_t color) {
 
 void WiFiScan::setupRemoteIdBle() {
   #ifdef HAS_BT
-    NimBLEDevice::setScanFilterMode(CONFIG_BTDM_SCAN_DUPL_TYPE_DEVICE);
-    NimBLEDevice::setScanDuplicateCacheSize(0);
-    NimBLEDevice::init("");
+    // The RID scanner starts Wi-Fi first and calls this only once the driver
+    // reports its dwell channel: enabling the BT controller while the Wi-Fi
+    // task is still starting or changing channel aborts inside
+    // coex_core_enable(). A controller that sniffbt already enabled is reused,
+    // because a second esp_bt_controller_enable() aborts the same way.
+    const bool ble_already_initialized = NimBLEDevice::getInitialized();
+    if (!ble_already_initialized) {
+      NimBLEDevice::setScanFilterMode(CONFIG_BTDM_SCAN_DUPL_TYPE_DEVICE);
+      NimBLEDevice::setScanDuplicateCacheSize(0);
+      NimBLEDevice::init("");
+    }
+
     pBLEScan = NimBLEDevice::getScan();
-    #ifndef HAS_NIMBLE_2
-      pBLEScan->setAdvertisedDeviceCallbacks(&bluetoothScanCallbacks, false);
-    #else
-      pBLEScan->setScanCallbacks(&bluetoothScanCallbacks, false);
-    #endif
-    pBLEScan->setActiveScan(false);
-    // Avoid repeatedly masking Wi-Fi beacons sent at an 80 ms cadence.
-    pBLEScan->setInterval(83);
-    pBLEScan->setWindow(40);
-    pBLEScan->setMaxResults(0);
-    pBLEScan->setDuplicateFilter(false);
-    #ifdef HAS_NIMBLE_2
-      pBLEScan->start(0, false, false);
-    #else
-      pBLEScan->start(0, scanCompleteCB, false);
-    #endif
-    ble_initialized = true;
-    ble_scanning = true;
+    if (pBLEScan != nullptr) {
+      if (ble_already_initialized) {
+        // Restart the observer so the RID callbacks are the ones dispatched.
+        pBLEScan->stop();
+        pBLEScan->clearResults();
+      }
+      #ifndef HAS_NIMBLE_2
+        pBLEScan->setAdvertisedDeviceCallbacks(&bluetoothScanCallbacks, false);
+      #else
+        pBLEScan->setScanCallbacks(&bluetoothScanCallbacks, false);
+      #endif
+      pBLEScan->setActiveScan(false);
+      // Avoid repeatedly masking Wi-Fi beacons sent at an 80 ms cadence.
+      pBLEScan->setInterval(83);
+      pBLEScan->setWindow(40);
+      pBLEScan->setMaxResults(0);
+      pBLEScan->setDuplicateFilter(false);
+      #ifdef HAS_NIMBLE_2
+        pBLEScan->start(0, false, false);
+      #else
+        pBLEScan->start(0, scanCompleteCB, false);
+      #endif
+      ble_initialized = true;
+      ble_scanning = true;
+    }
   #endif
 }
 
diff --git a/esp32_pocketnode/WiFiScanRemoteId.h b/esp32_pocketnode/WiFiScanRemoteId.h
index 3a9382b..357bd62 100644
--- a/esp32_pocketnode/WiFiScanRemoteId.h
+++ b/esp32_pocketnode/WiFiScanRemoteId.h
@@ -257,6 +257,11 @@ void WiFiScan::RunRemoteIdScan(uint8_t scan_mode, uint16_t color) {
     #endif
   #endif
 
+  // Never enable the BT controller while a previous sniffer still owns the
+  // Wi-Fi driver: tear any live Wi-Fi session down first.
+  if (this->wifi_initialized)
+    this->shutdownWiFi();
+
   esp_wifi_init(&cfg2);
   #ifdef HAS_IDF_3
     esp_wifi_set_country(&country);
@@ -267,6 +272,17 @@ void WiFiScan::RunRemoteIdScan(uint8_t scan_mode, uint16_t color) {
   wifi_initialized = true;
   Serial.println("RID scheduler: NAN dwell CH6/CH149 when supported; beacon sweep constrained by configured radio plan");
 
+  // Enabling the BT controller while a channel change is still queued in the
+  // Wi-Fi task races the coexistence hook and intermittently aborted inside
+  // coex_core_enable(). Wait (bounded) until the driver reports the channel.
+  uint8_t rid_channel = 0;
+  wifi_second_chan_t rid_second = WIFI_SECOND_CHAN_NONE;
+  uint32_t rid_wait_start = millis();
+  while ((esp_wifi_get_channel(&rid_channel, &rid_second) != ESP_OK ||
+          rid_channel != 6) &&
+         ((millis() - rid_wait_start) < 500))
+    delay(5);
+
   setupRemoteIdBle();
 
   setLEDMode(MODE_SNIFF);
```

The diff above is the exact output of `git diff -- esp32_pocketnode` (this document is itself modified, so a bare `git diff` also lists it).

## 7. Verification gaps (honest list)

- No real Remote ID transmitter was present, so the fix was validated on lifecycle/stability only, not on
  RID frame decoding.
- The 500 ms channel-settle wait is bounded but not asserted: if the driver never reports channel 6 the code
  proceeds anyway. That is deliberate (never block the CLI), but it means a pathological Wi-Fi task stall
  could still reach the BT enable in an unexpected state.
- Stress volume was ~27 starts; the original defect appeared at roughly 1 in 5, so this is strong evidence
  but not a proof at scale. A longer soak (hundreds of cycles) would tighten confidence.
- Only the S3 headless target was built and flashed; `HAS_NIMBLE_2` branches were not compiled here.
