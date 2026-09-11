# Tikk micro-ROS Liquid Tilt Display

Zephyr app for the [Tikk](../tikk-fleet) board (promicro_nrf52840 + add-on):
tilt the board and a "ball" rolls around a 9x16 LED matrix, exciting ripples in
a simple wave simulation — like a shallow pool of liquid. Tilt data is also
published over USB-CDC micro-ROS to a host agent.

No pouch/BLE/mcuboot by design — micro-ROS and the display work independently
first; pouch transport gets glued in later.

## Layout

- `app/` — this repo (Zephyr application + tikk-led-matrix module copy)
- `deps/` — west workspace modules (NCS v3.0.1 / zephyr v4.0.99-ncs1-1,
  is31fl3731 driver, pixel_font, micro_ros_zephyr_module)
- `host/` — host-side micro-ROS agent (docker compose)
- `.venv/` — python venv (west + build deps)

## Setup

    cd ~/golioth/microros
    source .venv/bin/activate
    west update

## Build & flash

    west build -b promicro_nrf52840 app
    west flash   # (board not connected yet — flash method TBD, see Notes)

## Run the micro-ROS agent (host)

    cd host && docker compose up

Then from any ROS 2 machine/container:

    ros2 topic list   # expect /tilt
    ros2 topic echo /tilt

## How it works

- LIS2DH polled at 20 Hz; low-pass filtered tilt sets a ball position target
- Ball movement injects impulses into a height-field wave sim (9x16 floats)
- Surface thresholded to on/off pixels; ball pixel forced on
- micro-ROS thread publishes `geometry_msgs/Vector3Stamped` (filtered accel,
  m/s^2) on `/tilt` at 10 Hz over USB CDC ACM to the agent

## Tuning knobs (app/src/main.c)

- `WAVE_K`, `WAVE_DAMP`, `SURF_THRESH`, `RIPPLE_AMP` — wave look/feel
- `AX_SIGN` / `AY_SIGN` — flip if ball moves the wrong way for board orientation

## Notes / TODO

- Flash method: promicro_nrf52840 default runner (UF2 bootloader vs debug
  probe) — decide at first hardware bring-up
- Per-LED PWM (grayscale waves): driver's write_channels is on/off only; would
  need PWM page burst support in the is31fl3731 driver
- Pouch/BLE transport: later
