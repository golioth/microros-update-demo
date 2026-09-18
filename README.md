# Tikk micro-ROS Liquid Tilt Display

Zephyr app for the [Tikk](https://github.com/golioth/tikk-fleet) board
(promicro_nrf52840 + add-on): a pool of "liquid" lives on the 7x15 LED
display (IS31FL3731 controller) — calm and covering the display at rest,
draining toward the low side as you tilt. Tilt data publishes over
USB-CDC micro-ROS to a host agent, and the whole thing takes **OTA
firmware updates over BLE** through a pouch gateway — the "before/after"
demo pair below shows the update landing live on the display itself.

## What the OTA demo looks like

- "before" firmware: tilt display + /tilt publisher. Reports version
  `0.1.2` (hardcoded in `src/fw_update.h`).
- "after" firmware: adds the TMP102 temperature publisher on /temp.
  Reports the `VERSION`-file string (`0.2.2`).
- Deploy the "after" artifact from the Golioth console → press the
  board's sync button ONCE → the manifest and the image download are
  carried across sessions automatically (auto-resync — see below).
- During the download the LED matrix shows the percentage ticking up
  as text (" 5%" → "50%" → "99%"), the liquid display and the
  publishers stand down, and a checkmark flashes when the image is
  complete. The board reboots into the new firmware and scrolls its
  version across the matrix ("v0.2.2") at boot.
- The payoff in the host viz: the /temp panel is blank on "before"
  and fills with live readings once "after" boots.

The % display and version scroll are drawn by the firmware that is
RUNNING, so the first update after changing the pair shows the
previous build's visuals; every one after that shows the new.

## Workspace & build (pure Zephyr 4.4)

Built against a pure-Zephyr workspace (NOT NCS — the port traps are in
`troubleshooting/connect-agent.md` §3d):

    cd ~/gateway-ws            # zephyr v4.4.0, pouch v0.2.0 (modules/lib/pouch),
                               # micro-ROS module (deps/modules/lib/micro_ros_zephyr_module),
                               # is31fl3731 + pixel_font modules — see its manifest/west.yml
    source ~/golioth/microros/.venv/bin/activate
    export MICROROS_ZEPHYR_MODULE_PATH=~/gateway-ws/deps/modules/lib/micro_ros_zephyr_module
    export ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-1.0.1

    # BEFORE build (temp publisher OFF — reports 0.1.2):
    west build --sysbuild -b promicro_nrf52840 \
        -d ~/golioth/microros/build-44-mcu ~/golioth/microros/app -- \
        -DCONFIG_TIKK_TEMP_PUBLISHER=n

    # AFTER build (default — reports the VERSION-file string):
    west build --sysbuild -b promicro_nrf52840 \
        -d ~/golioth/microros/build-44-mcu-after ~/golioth/microros/app

`west update` WIPES the micro-ROS module patches — re-apply
`0001`–`0004` and `0006`–`0009` from `patches/` afterwards (`0005` is
NCS-only), then `rm -rf` the module's `micro_ros_src/build+install` for
a clean colcon rebuild.

Sysbuild produces MCUboot (64 KB at `0x0`, see `sysbuild/flash-layout.dtsi`)
plus the signed app in slot 0 (`0x10000`); slot 1 (`0x84000`) receives
OTA downloads; the littlefs credentials partition stays at `0xf8000`
so provisioned certs survive a reflash. The OTA artifact to upload to
Golioth is `<build-dir>/app/zephyr/zephyr.signed.bin` (imgtool-signed
with MCUboot's bundled RSA dev key by the sysbuild step).

## Flashing (J-Link jig)

    JLinkExe -device nRF52840_xxAA -if SWD -speed 1000 \
        -SelectEmuBySN 851000760 -CommanderScript app/flash.jlink

`flash.jlink` loads both hexes (MCUboot + signed app) from the default
build dir and resets. Flashing erases only the pages written — the
credentials at `0xf8000` are preserved.

## Provisioning credentials (one-time per device)

Create the device on the Golioth console (download its DER cert/key
pair), then upload them to the board's littlefs via the shell SMP
transport (stop the console daemon first — it owns the port):

    uv pip install smpmgr
    smpmgr --port /dev/tikk-console file upload \
        ~/Downloads/<device>.crt.der /lfs1/credentials/crt.der
    smpmgr --port /dev/tikk-console file upload \
        ~/Downloads/<device>.key.der /lfs1/credentials/key.der

## Cloud setup (Golioth console)

1. **Package** (OTA → Packages): create `main` — the name the firmware
   registers its OTA component under.
2. **Artifacts** (package page → New Version): upload each build's
   `zephyr.signed.bin`. The version string must match what the firmware
   reports — `0.1.2` for the before build, the VERSION-file string for
   the after build — or the device will loop updates (or refuse them).
   Deployed artifacts are immutable: to ship changed binaries, bump the
   pair (VERSION file + the hardcoded before-string) and upload as new
   versions.
3. **Cohort**: create one, assign the device to it.
4. **Deploy**: pick the package version you want the cohort to run.
   The active deployment is the desired state — a device that checks in
   on an older version gets the update automatically.

`tools/ota_deploy.py` wraps the same REST calls (list/upload/deploy)
if you'd rather script it; the API key lives at `~/.golioth/api-key`.

## Running the demo

Press the sync button (P0.11 on the add-on) once. With an active
deployment targeting a different version than the running firmware:

- ~3 s: gateway connects, pouch session, manifest arrives
- the sync flag re-arms itself (auto-resync) — no second press needed —
  and the data session starts; the % text ticks up on the matrix
  (~70–80 s for a ~410 KB image through the RW612 gateway)
- checkmark, reboot, MCUboot swap, new firmware — its version scrolls
  across the matrix at boot
- a BLE drop mid-download no longer hangs the update: the flag re-arms
  and the download retries automatically

Auto-resync is in `src/pouch_setup.c` (re-arms on disconnect while a
download is pending/in flight) and `src/fw_update.c`/`fw_update.h`
(`fw_download_pending`).

### Gateways

- **RW612 running the pouch v0.2.0 gateway sample** (fast path,
  ~5.7 KB/s): the known-good demo gateway. Keep its console on a reader
  for observability (`tools/gateway_reader_v2.py` pattern — resolve the
  port by USB serial `83F4…`, never by ttyACM number).
- **connect-agent snap**: verified working end-to-end on 2026-09-16
  (pairing, device-cert upload accepted, OTA delivered) but ~13x
  slower (~435 B/s — one GATT write per ~550 ms). Pairing hygiene
  applies after every device reset: `bluetoothctl remove <MAC>` +
  restart the gateway, or you get an auth-failure connect loop
  (reason 0x05). A stuck `EALREADY`-on-discovery state (snap/BlueZ
  D-Bus) clears with `sudo systemctl restart bluetooth`.

## Host-side setup

- **Console daemon** (do not open the console CDC with anything else):
  `python3 tools/tikk_console_daemon.py` — holds DTR (the 4.4 CDC
  console goes silent without it), resolves the port by USB serial on
  every re-enumeration, strips the shell's VT100 chatter, highlights
  OTA lines yellow in `tail -f /tmp/tikk-console-live.log`, and takes
  shell commands via `echo "sync" > /tmp/tikk-cmd.fifo`.
- **micro-ROS agent**: `cd host && docker compose up -d` — with the
  CDC-renumber ritual: after ANY board reset/replug use
  `docker compose up -d --force-recreate` (restart reuses the stale
  device bind), and if a burned handshake has killed the device's
  client ("micro-ROS error N" in the console log), reset the board.
  "Waiting for agent connection" in the log means it will wake on the
  agent's DTR — that's the healthy parked state.
- **viz**: `docker run --rm --net=host -e FASTDDS_BUILTIN_TRANSPORTS=UDPv4
  -v ~/golioth/microros/app/host:/host:ro ros:kilted-ros-core python3
  /host/viz.py` — scrolling tilt charts + the temp panel that fills
  when the "after" firmware lands.

## How it works

- LIS2DH polled at 20 Hz; low-pass filtered tilt tilts the water's
  equilibrium plane (unfiltered accel noise keeps the water agitated)
- "Flat tray + meniscus" liquid model: volume-conserving water-fill
  over a tilting floor with wall-cling for the concave corners
- Binary threshold render: depth > SURF_THRESH = LED on
- micro-ROS thread publishes `geometry_msgs/Vector3Stamped` on `/tilt`
  at 10 Hz (plus `std_msgs/Float32` on `/temp` at 1 Hz in the "after"
  build) over USB CDC ACM to the host agent
- pouch v0.2.0 over BLE GATT: the sync button sets the sync-request
  flag in the advertisement; a gateway sees it, connects, and runs a
  session (device cert upload + uplink/downlink blocks) to the cloud

## Tuning knobs (src/main.c)

- `WATER_LEVEL` — total water volume (this × cell count)
- `WALL_CLING` — meniscus strength
- `PLANE_SCALE` / `PLANE_PULL` — tilt travel / settle speed
- `WAVE_DAMP` — velocity retention per tick (lower = settles faster)
- `AX_SIGN` / `AY_SIGN` — flip if water pools the wrong way
- `DISPLAY_BRIGHTNESS` (modules/tikk-led-matrix/tikk_led_matrix.c)

## Future work

- Per-LED PWM for grayscale water — the display is currently binary
  on/off; needs PWM-page burst support in the is31fl3731 driver.

## Troubleshooting

Gateway/BLE/cloud failure modes — the snap's dead-stack cert bug, the
4.00 root cause, pairing hygiene, CDC renumbering — live in
[troubleshooting/connect-agent.md](troubleshooting/connect-agent.md).
The workspace-level `~/golioth/microros/SESSION-NOTES.md` carries the
full session-by-session post-mortems and verified recipes.
