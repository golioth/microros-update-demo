# Troubleshooting: Tikk ↔ Connect Agent gateway ↔ Golioth cloud

Working notes from bringing the Tikk (nRF52840, pouch v0.2.0, mcuboot)
online through the Connect Agent snap (0.5.3 / rev 28, latest/beta) to
`gw.golioth.io`. Ordered by when each failure mode appears in the chain.

## 1. Gateway never attempts a BLE connection

**Symptom:** gateway log shows startup lines only; no `peer connected`,
ever. Board advertises (visible in nRF Connect / bluetoothctl).

**Cause A — advertisement format generation.** Pouch v0.2.0 changed the
BLE advertisement: the service is advertised with a 16-bit UUID
(`0xFC49`) inside Service Data in the MAIN advertisement. Firmware built
against pre-v0.1.0 pouch (e.g. the tikk-fleet pin 2d66b83) advertises
the old 128-bit UUID format, which v0.2-era gateways do not scan for.
**Fix:** build against pouch v0.2.0+ (see `west.yml`), use the
library-provided `POUCH_GATT_ADV_DATA_INIT` and `pouch/transport/bluetooth/gatt.h`.

**Cause B — sync-request flag not set.** The gateway (with
`gateway-ble-target-macs` set) connects when it sees the sync-request
flag in the advertisement service data. On this board it is set by the
button on P0.11 (`pouch_gatt_adv_req_sync(&adv, true)`), and cleared on
`POUCH_EVENT_SESSION_END`.

## 2. `failed to connect ... le-connection-abort-by-local` loop

**Symptom:** repeated BlueZ aborts on the device path in the snap log.

**Cause:** stale BlueZ-side bond. The device firmware does not persist
BLE bonds (no `CONFIG_BT_SETTINGS`), so every board reset orphans the
host bond and BlueZ refuses the connection.

**Fix:** `bluetoothctl remove <device-MAC>`, then
`snap restart connect-agent.gateway` (the restart is required — without
it you get ~450 ms auth-failure disconnects from stale agent state).

**Robustness option:** enable `CONFIG_BT_SETTINGS` + a settings backend
so bonds survive board resets.

## 3. Session fails at `forward device certificate: POST .g/device-cert: 400`

**Root cause (snap 0.5.3 / rev 28): stack-use-after-return in the
gateway work queue.** The snap was built 2026-07-22 19:52, between:

- `87136d7` 2026-07-22 16:54 "gateway: offload device cert cloud upload
  to the gateway work queue" (introduces the bug)
- `c91c2bf` 2026-07-23 16:05 "gateway: fix stack use-after-return in
  pouch_gateway_workq_run_sync" (the fix)

**Evidence** (captures in `evidence/20260915-snap-rev28/`, gathered with
`tools/gw-capture-proxy.py`):

- Every captured `POST /.g/device-cert` body is 361 bytes (the DER
  length — so the intended body was the raw cert) but contains dead
  stack: `-` fill plus unrelated stack text in the first session,
  repeating `04 00` fill in later sessions. The cert DER (starts
  `30 82 01 61`) is never present.
- The device's cert is byte-identical to the console-issued file
  (verified via smpmgr download + cmp).
- The server accepts the real cert: `POST` raw DER with the gateway's
  mTLS creds returns 200 (idempotent), while base64 and PEM bodies
  return 400 — i.e. raw DER is the expected format end-to-end.

**Fix:** rebuild the snap from pouch v0.2.0 (or any revision >=
c91c2bf). With a fixed snap, no device-side change is needed.

## 4. smpmgr timeouts / `mcumgr: command not found` on the device shell

**Cause:** `MCUMGR_TRANSPORT_SHELL` depends on `SHELL && BASE64 && CRC`.
Pouch pre-v0.2.0 `select BASE64` from its Kconfig; v0.2.0 dropped that,
so without an explicit `CONFIG_BASE64=y` the whole shell-transport
Kconfig block silently vanishes (no warning). **Fix:** prj.conf sets it
explicitly (b998434).

## 5. Serial port chaos (ttyACM renumbering)

CDC minor numbers shift whenever a stale fd survives a board reset or
replug (open minicom, docker device binds, watcher scripts). This repo
fixes it with udev symlinks keyed to the USB interface numbers, which
never change:

- `/dev/tikk-console` (interface 00) — shell / SMP
- `/dev/tikk-microros` (interface 02) — micro-ROS XRCE data

Install: `sudo cp host/70-tikk-serial-symlinks.rules /etc/udev/rules.d/
&& sudo udevadm control --reload-rules && sudo udevadm trigger`.

Also: run `minicom -o` — minicom's init strings inject escape garbage
into the Zephyr shell and wedge its VT100 state (recover with
`kernel reboot cold` over the shell; input processing survives even
when output looks trashed).