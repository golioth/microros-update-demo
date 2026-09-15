# Tikk micro-ROS Liquid Tilt Display

Zephyr app for the [Tikk](https://github.com/golioth/tikk-fleet) board
(promicro_nrf52840 + add-on): a pool of "liquid" lives on the 7x15 LED display
(IS31FL3731 controller, 7x15 of its 9x16 matrix populated) — calm and covering
the display at rest, draining toward the low side with a concave corner
waterline as you tilt, sloshing briefly on fast moves. Tilt data
is published over USB-CDC micro-ROS to a host agent.

## Progress (updated 2026-09-11)

**Working, verified on hardware:**
- [x] West workspace (NCS v3.0.1 / zephyr v4.0.99-ncs1-1, SDK 0.17.0) mirroring tikk-fleet
- [x] LIS2DH accelerometer + 7x15 LED display (IS31FL3731 controller; 7x15
      of the 9x16 matrix is populated) on the Tikk add-on board
- [x] Liquid display model, converged after six iterations:
      flat water film everywhere at rest + gravity tilt plane + wall-cling
      meniscus (concave corner waterline), calm settle, one honest slosh on
      big moves
- [x] micro-ROS firmware: `geometry_msgs/Vector3Stamped` on `/tilt` at 10 Hz
      over USB CDC ACM (node `tikk_tilt`), independent from the display sim
- [x] micro-ROS agent (docker, `microros/micro-ros-agent:kilted`) bridging the
      board into a live ROS 2 graph
- [x] Live terminal visualization of x/y/z tilt (`app/host/viz.py` — ANSI
      scrolling charts with grayscale shading, no dependencies)
- [x] On-hardware bring-up complete: boot, flashing, USB, display physics all
      sorted (see git history for the full debugging saga)

**Next up:**
- [ ] Pouch/Golioth transport integration — blocked on a libc conflict:
      libmicroros is built against newlib while pouch expects picolibc
      (known, documented; needs a build spike or a patch upstream)
- [ ] Per-LED PWM for grayscale water — needs is31fl3731 driver PWM-page
      burst support (display is currently binary on/off)

## Workspace layout

This repo is the `app/` directory of a west workspace rooted at
`~/golioth/microros`. No pouch/BLE/mcuboot by design — micro-ROS and the
display work independently first; pouch transport gets glued in later.

- `app/` — this repo (Zephyr application + tikk-led-matrix module copy +
  host-side agent & viz in app/host/)
  - `app/host/` — host-side micro-ROS agent (docker compose) + viz.py
- `deps/` — west workspace modules (NCS v3.0.1 / zephyr v4.0.99-ncs1-1,
  is31fl3731 driver, pixel_font, micro_ros_zephyr_module)
- `.venv/` — python venv (west + build deps)

## Setup

    cd ~/golioth/microros
    source .venv/bin/activate
    west update

    # Apply the local module patches (see patches/) — 0001 because colcon
    # caches CMake flags across libc/Kconfig changes and stale flags break
    # the cross-build; 0002 defines __STDC_WANT_LIB_EXT1__=1 for all
    # micro-ROS packages (fixes picolibc/rcutils Annex K __errno_t error);
    # 0003 fixes the transports' RX ring buffer aliasing the TX buffer's
    # storage (random session-establishment corruption/wedges):
    git -C deps/modules/lib/micro_ros_zephyr_module apply ../app/patches/0001-colcon-cmake-clean-cache.patch
    git -C deps/modules/lib/micro_ros_zephyr_module apply ../app/patches/0002-picolibc-annex-k-cflags.patch
    git -C deps/modules/lib/micro_ros_zephyr_module apply ../app/patches/0003-transport-rx-buffer-aliasing.patch

    uv pip install -r deps/zephyr/scripts/requirements.txt -r deps/nrf/scripts/requirements.txt
    uv pip install colcon-common-extensions catkin_pkg empy lark

## Build & flash

SDK 0.17.0 is required (matches tikk-fleet; SDK 0.17.4's picolibc conflicts
with zephyr v4.0.99-ncs1-1). Use the ABSOLUTE path — `~/`-relative paths
silently resolve wrong in non-login shells (e.g. agent sessions where $HOME
is overridden), and Zephyr then auto-detects 0.17.4 instead:

    ZEPHYR_SDK_INSTALL_DIR=/home/chrisg/zephyr-sdk-0.17.0 west build -b promicro_nrf52840 app

First build cross-compiles all of micro-ROS (~25 ROS 2 repos) as part of the
build — expect several minutes. Subsequent builds are incremental.

    west flash   # (board not connected yet — flash method TBD, see Notes)

## Run the micro-ROS agent (host)

Bring-up order matters (the firmware's agent handshake is one-shot and
fail-fast): reset/power the board FIRST (it parks at "Waiting for agent
connection"), THEN start the agent — its port-open is the handshake
trigger. After any board reset/replug, recreate the agent container
(`--force-recreate`, not `restart`) so its device bind re-resolves.
Conversely, after ANY agent restart/recreate, reset the board again —
the firmware's client never re-establishes a dead session and will
publish into the void until rebooted.

One-time host setup — keep ModemManager off the board's CDC ports (it
probes ttyACM1 at every enumeration, asserts DTR, and burns the
handshake; see the rule file for the full story):

    sudo cp app/host/99-microros-zephyr-cdc.rules /etc/udev/rules.d/
    sudo udevadm control --reload-rules
    sudo systemctl stop ModemManager

    cd app/host && docker compose up -d

## Visualizing /tilt

The agent and any ROS 2 containers must set `FASTDDS_BUILTIN_TRANSPORTS=UDPv4`
— Fast DDS's shared-memory transport silently drops data across container
/dev/shm boundaries (symptom: topic lists, but echo gets nothing).

Live scrolling charts (x/y/z) in the terminal:

    docker run --rm --net=host -e FASTDDS_BUILTIN_TRANSPORTS=UDPv4 \
      -v ~/golioth/microros/app/host:/host:ro ros:kilted-ros-core \
      python3 /host/viz.py

Plain message stream:

    docker run --rm --net=host -e FASTDDS_BUILTIN_TRANSPORTS=UDPv4 \
      ros:kilted-ros-core ros2 topic echo /tilt

Then from any ROS 2 machine/container:

    ros2 topic list   # expect /tilt
    ros2 topic echo /tilt

## How it works

- LIS2DH polled at 20 Hz; low-pass filtered tilt tilts the water's
  equilibrium plane (unfiltered accel noise keeps the water agitated)
- "Flat tray + meniscus" liquid model: at rest a water film covers the whole
  display; tilt drains it toward the low side/corners. A wall-cling term
  keeps water hugging edges and corners slightly past the flat waterline,
  giving the concave corner meniscus
- Binary threshold render: depth > SURF_THRESH = LED on
- micro-ROS thread publishes `geometry_msgs/Vector3Stamped` (filtered accel,
  m/s^2) on `/tilt` at 10 Hz over USB CDC ACM to the agent

## Tuning knobs (app/src/main.c)

- `WATER_LEVEL` — TOTAL water volume (this × cell count). Flat => uniform film
  everywhere; resting on the long side => ~2-row pool. Raise/lower to change
  how much water is in the tray
- `WALL_CLING` — how strongly water hugs edges/corners (meniscus strength)
- `PLANE_SCALE` — how far a given tilt moves the water
- `PLANE_PULL` — how fast water chases its equilibrium
- `WAVE_DAMP` — velocity RETENTION per tick; lower = settles faster (0.97 rings ~2s, 0.80 settles in ~0.5s)
- `WAVE_K` — wave propagation stiffness; 0 = no traveling waves
- `TILT_LP` — tilt filter responsiveness; higher = snappier but noisier
- `AX_SIGN` / `AY_SIGN` — flip if water pools the wrong way
- `DISPLAY_BRIGHTNESS` (modules/tikk-led-matrix/tikk_led_matrix.c) — 0-100

## Flashing (J-Link jig)

The board is flashed via a J-Link on a custom jig that clips onto the
component side (which faces the LEDs — unclip to view the display):

    JLinkExe -device nRF52840_xxAA -if SWD -speed 1000 -CommandFile app/flash.jlink

The build now includes MCUboot (sysbuild): mcuboot at 0x0 (64 KB, with
USB-CDC serial recovery — brick insurance) and the app in slot 0 at
0x10000. flash.jlink loads `build/merged.hex` (bootloader + app).

Artifact-name trap (bit us once): **`build/mcuboot_primary.hex` is NOT
"mcuboot + app"** — despite the name it contains ONLY the slot-0 app
image (starts at 0x10000, no bootloader). Flashing it alone leaves 0x0
erased and the CPU faults on boot (IACCVIOL, PC in SCB space). The
historical note below is from the pre-mcuboot era and is now inverted:

(OLD, no-mcuboot builds only: the app linked directly at 0x0 and
build/merged.hex back then was app@0x1000-behind-an-MBR — bricking.
With the current mcuboot sysbuild, merged.hex is exactly the right
thing to flash.)

## Notes / TODO

- Flash method: promicro_nrf52840 default runner (UF2 bootloader vs debug
  probe) — decide at first hardware bring-up
- Per-LED PWM (grayscale waves): driver's write_channels is on/off only; would
  need PWM page burst support in the is31fl3731 driver
- Pouch/BLE transport: later

## Troubleshooting

Gateway/BLE/cloud failure modes and their fixes — including the
Connect Agent rev 28 device-cert bug analysis with captured evidence —
live in [troubleshooting/connect-agent.md](troubleshooting/connect-agent.md).
The capture proxy used to gather the evidence is
[tools/gw-capture-proxy.py](tools/gw-capture-proxy.py).
