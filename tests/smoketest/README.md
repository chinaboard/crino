# Smoketest — verify chassis weak-symbol app hooks

A tiny app that overrides all four crino extension hooks
(`cr_app_init`, `cr_app_register_routes`, `cr_app_led_busy`,
`cr_app_factory_reset`) with strong definitions. If the chassis
correctly defers to strong app overrides at link time, flashing this
build produces these visible effects you can check from a fresh boot:

1. Serial log line `===== smoketest cr_app_init() invoked =====`
   appearing after the chassis init order finishes.
2. Serial log line
   `===== smoketest cr_app_register_routes() invoked, /api/smoketest/ping rc=OK =====`
   appearing during `cr_http_start()`. `curl http://crino-XXXX.local/api/smoketest/ping`
   returns `pong\n`.
3. The status LED stays on the BUSY state (cyan fast-blink on RGB
   boards, DUTY_BLINK on PWM boards) for the first 30 s of uptime,
   then drops back to the normal chassis state (OK / AP / etc.).
4. Triggering a factory reset (`POST /api/system/factory_reset` with
   `{"confirm":"FACTORY_RESET"}`, or 5-second BOOT-button hold)
   prints `===== smoketest cr_app_factory_reset() invoked =====`
   on the serial right before the chassis NVS namespaces get wiped.

If none of those show up: the strong overrides didn't beat the
chassis weak defaults. Most common cause is the linker didn't pull
the smoketest archive into the final image — see WHOLE_ARCHIVE TRUE
in CMakeLists.txt.

## Build

The smoketest lives outside `components/` so IDF doesn't auto-pick
it up. Opt in via `EXTRA_COMPONENT_DIRS`:

```bash
make build BOARD=supermini-c3 EXTRA_COMPONENT_DIRS=tests
make flash BOARD=supermini-c3
```

To go back to a chassis-only build, just drop `EXTRA_COMPONENT_DIRS=tests`.

## Result (verified on ESP32-C3 SuperMini)

```
W (5341) smoketest: ===== smoketest cr_app_register_routes() invoked, /api/smoketest/ping rc=OK =====
I (5341) http: HTTP server listening on :80 (27 chassis routes)
W (5341) smoketest: ===== smoketest cr_app_init() invoked =====
```

LED visibly held BUSY state for 30 s after boot, then settled into
the normal AP state. The extension model works.

## Manual: full cross-partition recovery test

The factory-partition handoff (`cr_recovery_handoff_to_factory`) is
exercised by the chassis whenever the boot-loop counter trips while
running from a non-factory slot. The "already in factory" branch is
exercised automatically by any 3 fast resets after a fresh flash —
the chassis is running from `factory` already, so the handoff
returns false and the SoftAP recovery banner shows.

To exercise the cross-partition branch (the one that actually calls
`esp_ota_set_boot_partition(factory)` and reboots) you need an
image flashed into ota_0 first:

1. Fresh flash (writes chassis to factory): `make flash BOARD=supermini-c3`.
2. Connect a phone/laptop to SoftAP `crino-setup-XXXX`.
3. Browse to `http://192.168.4.1`, complete the first-run setup
   (admin password + WiFi creds). Device reboots into STA.
4. On the System tab, OTA upload `build-esp32c3/crino.bin`. The
   chassis writes it to `ota_0`, sets boot partition to ota_0,
   and reboots. Boot log should now say
   `running from partition 'ota_0' (ota_0)`.
5. From the System tab click `Restart device` three times in quick
   succession (each within 60 s of the previous one — the OTA
   validate timer must NOT fire between resets, or the boot-loop
   counter gets cleared).
6. On the 3rd restart, watch the serial log: you should see
   ```
   *** BOOT-LOOP RECOVERY ARMED *** count=3 (≥3)
   *** boot-loop recovery: jumping to factory partition 'factory' ***
   ```
   followed by a reboot, then `running from partition 'factory'`
   on the next boot, plus the red recovery banner in the Web UI.
7. Recovery from here: upload a working image via the chassis's
   own OTA. `/api/system/ota` clears the boot-loop counter as part
   of the upload, so the new image boots clean from ota_X next
   reboot.

If `parttool.py` is available you can shortcut steps 1-4 by
writing the chassis to ota_0 directly + updating otadata to point
there, but the manual flow above is the canonical user path.

