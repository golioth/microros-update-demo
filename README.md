# Tikk micro-ROS Liquid Tilt Display

Zephyr app for the [Tikk](https://github.com/golioth/tikk-fleet) board
(promicro_nrf52840 + add-on): a pool of "liquid" lives on the 9x16 LED matrix —
calm and covering the display at rest, draining toward the low side with a
concave corner waterline as you tilt, sloshing briefly on fast moves. Tilt data
is published over USB-CDC micro-ROS to a host agent.

## Progress (updated 2026-09-11)

**Working, verified on hardware:**
- [x] West workspace (NCS v3.0.1 / zephyr v4.0.99-ncs1-1, SDK 0.17.0) mirroring tikk-fleet
- [x] LIS2DH accelerometer + IS31FL3731 9x16 LED matrix on the Tikk add-on board
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

    # Apply the local module patch (see patches/) — needed because colcon
    # caches CMake flags across libc/Kconfig changes and stale flags break
    # the cross-build:
    git -C deps/modules/lib/micro_ros_zephyr_module apply ../app/patches/0001-colcon-cmake-clean-cache.patch

    uv pip install -r deps/zephyr/scripts/requirements.txt -r deps/nrf/scripts/requirements.txt
    uv pip install colcon-common-extensions catkin_pkg empy lark

## Build & flash

SDK 0.17.0 is required (matches tikk-fleet; SDK 0.17.4's picolibc conflicts
with zephyr v4.0.99-ncs1-1):

    ZEPHYR_SDK_INSTALL_DIR=~/zephyr-sdk-0.17.0 west build -b promicro_nrf52840 app

First build cross-compiles all of micro-ROS (~25 ROS 2 repos) as part of the
build — expect several minutes. Subsequent builds are incremental.

    west flash   # (board not connected yet — flash method TBD, see Notes)

## Run the micro-ROS agent (host)

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

- `WATER_LEVEL` — resting film depth; lower = water retreats to corners with less tilt
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

The app links directly at 0x0 (`CONFIG_BOARD_HAS_NRF5_BOOTLOADER=n`); do NOT
flash build/merged.hex — it only contains the app at 0x1000 behind an MBR
that isn't included, which bricks boot (this bit us once).

## Notes / TODO

- Flash method: promicro_nrf52840 default runner (UF2 bootloader vs debug
  probe) — decide at first hardware bring-up
- Per-LED PWM (grayscale waves): driver's write_channels is on/off only; would
  need PWM page burst support in the is31fl3731 driver
- Pouch/BLE transport: later
