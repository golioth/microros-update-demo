# Troubleshooting: Tikk ↔ Connect Agent gateway ↔ Golioth cloud

Working notes from bringing the Tikk (nRF52840, pouch v0.2.0, mcuboot)
online through gateways to `gw.golioth.io` / `coap.golioth.io`. Ordered
by when each failure mode appears in the chain.

> **2026-09-15 FINAL VERDICT** (see section 3): the cloud and the pouch
> v0.2.0 gateway flow are PROVEN GOOD end-to-end — reference ble_gatt
> device on nRF52840-DK (Zephyr 4.4.0) → pouch v0.2.0 gateway on
> FRDM-RW612 (Zephyr 4.4.0) → production cloud all worked, device
> checked in. The remaining defect is OUR device build: the Tikk app on
> NCS v3.0.1 (Zephyr 4.0.99-ncs1 + SoftDevice Controller) corrupts the
> device-certificate somewhere in the BLE SAR/notification leg — the
> server's 4.00 responses were legitimate rejections of mangled bytes.
> Fix in flight: port the Tikk app to the pure Zephyr 4.4 workspace.

## 1. Gateway never attempts a BLE connection

**Symptom:** gateway log shows startup lines only; no `peer connected`,
ever. Board advertises (visible in nRF Connect / bluetoothctl).

**Cause A — advertisement format generation.** Pouch v0.2.0 changed the
BLE advertisement: the service is advertised with a 16-bit UUID
(`0xFC49`) inside Service Data in the MAIN advertisement. Firmware built
against pre-v0.1.0 pouch (e.g. the tikk-fleet pin 2d66b83) advertises
the old 128-bit UUID format, which v0.2-era gateways do not scan for.
**Fix:** build against pouch v0.2.0+ (see `west.yml`), use the
library-provided `POUCH_GATT_ADV_DATA_INIT` and
`pouch/transport/bluetooth/gatt.h`.

**Cause B — sync-request flag not set.** The gateway (with
`gateway-ble-target-macs` set) connects when it sees the sync-request
flag in the advertisement service data. On this board it is set by the
button on P0.11 (`pouch_gatt_adv_req_sync(&adv, true)`), and cleared on
`POUCH_EVENT_SESSION_END`. (The reference ble_gatt sample auto-queues
data on every uplink, which auto-sets the flag — no button needed.)

## 2. `failed to connect ... le-connection-abort-by-local` loop (snap)

**Symptom:** repeated BlueZ aborts on the device path in the snap log.

**Cause:** stale BlueZ-side bond. Device firmware without
`CONFIG_BT_SETTINGS` loses its bond on every reset; BlueZ refuses.

**Fix:** `bluetoothctl remove <device-MAC>`, then
`snap restart connect-agent.gateway` (the restart is required — without
it you get ~450 ms auth-failure disconnects from stale agent state).

**Robustness option:** enable `CONFIG_BT_SETTINGS` + a settings backend
so bonds survive board resets.

## 3. Session fails at `forward device certificate: 400` / CoAP `4.00`

Three separate causes were unpeeled here; do not conflate them:

### 3a. Connect Agent snap rev 28 (0.5.3): stack-use-after-return

Captured with `tools/gw-capture-proxy.py`: the snap POSTs **dead stack**
instead of the device cert — first session after process start contains
'-' fill + unrelated stack text, later sessions repeating `04 00` fill;
body length matches the DER length (361 B) but contains zero cert
content. The snap (built 2026-07-22 19:52) contains the telemetry-team
private repo's version of the async device-cert upload — note: BOTH snap
revisions on this machine (0.5.2/rev26 built Jul 14, 0.5.3/rev28 built
Jul 22) fail the same way, and the private repo's history governs —
upstream pouch commits `87136d7`/`c91c2bf` are suggestive but NOT the
snap's code lineage. Evidence: `evidence/20260915-snap-rev28/`.
**Fix: snap rebuild from a tree containing the equivalent of upstream
c91c2bf.** The HTTPS surface (gw.golioth.io) accepts raw DER
(curl-verified 200).

### 3b. The server is NOT at fault (proven 2026-09-15 ~17:20)

The hardware gateway (pouch v0.2.0 on FRDM-RW612, Zephyr 4.4.0/SDK
1.0.1, reference code) initially showed the same 4.00 on
`coap.golioth.io/.g/device-cert` when paired with OUR Tikk firmware —
which looked server-side. The controlled experiment that settled it:

- Reference `ble_gatt` sample on **nRF52840-DK** (Zephyr 4.4.0, same
  pouch v0.2.0, same credential files) → **same RW612 gateway →
  production cloud: SESSION COMPLETED, device checked in.**
- Our Tikk app (pouch v0.2.0 on NCS v3.0.1 / Zephyr 4.0.99-ncs1 +
  SoftDevice Controller) → same gateway: cert upload 4.00 every time.
- The gateway's own cert (no BLE leg) uploads fine from either side.

**Conclusion: the NCS 4.0.99 device build corrupts the device cert in
the BLE SAR/notification transfer.** Pouch v0.2.0 is CI-tested on
Zephyr 4.4; building/booting/pairing OK on 4.0.99 does NOT mean the
transport is OK.

### 3c. Porting note

The Tikk app must move to the pure-Zephyr 4.4 workspace (see
`~/golioth/gateway-ws` for the working pattern: zephyr v4.4.0 + pouch
v0.2.0 + sdk-ng 1.0.1). Pure-Zephyr builds use vanilla mbedtls
(tf-psa-crypto) — the NCS crypto shims (`MBEDTLS_LEGACY_CRYPTO_C`,
NRF_SECURITY) are Nordic-only and must be dropped. The DK reference
prj.conf is the template for the pouch/BT block.

## 4. smpmgr timeouts / `mcumgr: command not found` on the device shell

**Cause:** `MCUMGR_TRANSPORT_SHELL` depends on `SHELL && BASE64 && CRC`.
Pouch pre-v0.2.0 `select BASE64` from its Kconfig; v0.2.0 dropped that,
so without an explicit `CONFIG_BASE64=y` the whole shell-transport
Kconfig block silently vanishes (no warning). **Fix:** prj.conf sets it
explicitly (b998434).

## 5. Serial port chaos (ttyACM renumbering + VID:PID collisions)

CDC minor numbers shift whenever a stale fd survives a board reset or
replug. Fixed with udev symlinks keyed to USB interface numbers
(host/70-tikk-serial-symlinks.rules). **Caveat discovered later:** the
RW612's USB CDC build (gateway-usb-console) uses Zephyr's DEFAULT
VID:PID 2fe3:0100 — the SAME as the Tikk — so `/dev/tikk-console` gets
poisoned whenever both boards are attached (the RW612 CDC matches
`if=00` too). Identify ports by USB serial instead:
Tikk = `5F5F7992...`, RW612 target CDC = `83F4C0FDF0D120...`,
nRF52840-DK OB = `1050284083`. The udev rule should be re-keyed to
`ID_SERIAL_SHORT` as a follow-up.

Also: run `minicom -o` (its init strings wedge the Zephyr shell's VT100
state), and never open the same port from two readers — bytes split
randomly between them and both sides look "slow/garbled".

## 6. Hardware gateway build (FRDM-RW612) notes

The pouch v0.2.0 gateway sample on frdm_rw612 requires:
- a **pure Zephyr workspace** — under NCS, `nrf_security` hijacks the
  MBEDTLS_* symbols with `depends on SOC_FAMILY_NORDIC_NRF`, which can
  never resolve on NXP targets (Kconfig abort)
- **Zephyr SDK 1.0.x (sdk-ng)** — assembled from a v1.0.1 tag clone of
  https://github.com/zephyrproject-rtos/sdk-ng + hosttools installer +
  `toolchain_gnu_linux-x86_64_arm-zephyr-eabi.tar.xz` extracted under
  `gnu/` + a hand-made `sdk_version` file
- **RW612 radio firmware blobs**: `west blobs fetch hal_nxp -l rw61x`
- console provisioning via the **target USB CDC overlay** (the debug
  OB VCOM's host→target input path was unreliable on this host):
  `gateway-usb-console.overlay` + `.conf` in `~/golioth/gateway-ws/`,
  built with `-DEXTRA_DTC_OVERLAY_FILE=... -DEXTRA_CONF_FILE=...`
- provisioning: smpmgr over the target-USB console to
  `/lfs1/credentials/{crt,key}.der` (same layout as devices)
- after `west flash`, the OB's soft reset may leave the app wedged —
  press the physical RESET button if the console goes silent
- the nRF52840-DK's Button 1 is on **P0.11 — the same pin as the
  Tikk's sync button**, so the Tikk button overlay works on both