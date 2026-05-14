# Smoketest — verify chassis weak-symbol app hooks

A tiny app that overrides all three crino extension hooks
(`cr_app_init`, `cr_app_register_routes`, `cr_app_led_busy`) with
strong definitions. If the chassis correctly defers to strong app
overrides at link time, flashing this build produces three visible
effects you can check from a fresh boot:

1. Serial log line `===== smoketest cr_app_init() invoked =====`
   appearing after the chassis init order finishes.
2. Serial log line
   `===== smoketest cr_app_register_routes() invoked, /api/smoketest/ping rc=OK =====`
   appearing during `cr_http_start()`. `curl http://crino-XXXX.local/api/smoketest/ping`
   returns `pong\n`.
3. The status LED stays on the BUSY state (cyan fast-blink on RGB
   boards, DUTY_BLINK on PWM boards) for the first 30 s of uptime,
   then drops back to the normal chassis state (OK / AP / etc.).

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
