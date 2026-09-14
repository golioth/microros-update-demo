#!/usr/bin/env python3
"""Live terminal view of the Tikk /tilt and /temp topics: three scrolling
history charts (x/y/z) plus a temperature panel. Pure ANSI, no dependencies.

The temp panel is the OTA demo's "after" state: current (pre-OTA) firmware
publishes only /tilt, so temp shows a blank placeholder; once the OTA'd
firmware (TMP101 driver) starts publishing /temp (std_msgs/Float32, deg C),
the panel fills in live — a visible before/after effect through the same viz.

Run inside a ROS 2 container on the host network:

    docker run --rm -it --net=host -e FASTDDS_BUILTIN_TRANSPORTS=UDPv4 \
        -v ~/golioth/microros/app/host:/host:ro ros:kilted-ros-core \
        python3 /host/viz.py

(The FASTDDS_BUILTIN_TRANSPORTS env var is required: the agent runs in a
separate container, and Fast DDS's shared-memory transport silently drops
data across container /dev/shm boundaries.)
"""
import sys
import time
from collections import deque

import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy, HistoryPolicy
from geometry_msgs.msg import Vector3Stamped
from std_msgs.msg import Float32

HIST = 60          # samples kept (~6 s at 10 Hz for tilt; 60 s at 1 Hz for temp)
PLOT_H = 5         # chart rows per axis — with the temp panel the total
                   # frame stays ≤ 40 lines, so it fits a fullscreen terminal
TEMP_H = 3         # rows for the temp panel
VMAX = 10.0        # m/s^2 full scale
AXES = ("x", "y", "z")
COLORS = ("96", "93", "92")  # cyan, yellow, green
TEMP_COLOR = "95"            # magenta
DASH = "\u2500"    # (legacy; unused)
AXIS = "="         # zero-row marker: white axis line where the trace is
                   # elsewhere; replaced by a colored block only where the
                   # trace actually sits on zero
SOLID = "\u2588"
SHADES = "\u2591\u2592\u2593"  # light/medium/dark partial fill gradient
TEMP_MIN_SPAN = 0.5  # deg C — chart floor so a steady reading doesn't collapse


class TiltViz(Node):
    def __init__(self):
        super().__init__("tikk_viz")
        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=50,
        )
        self.sub = self.create_subscription(Vector3Stamped, "/tilt", self.cb, qos)
        self.sub_temp = self.create_subscription(Float32, "/temp", self.cb_temp, qos)
        self.hist = {a: deque(maxlen=HIST) for a in AXES}
        self.last = {a: 0.0 for a in AXES}
        self.count = 0
        # temp state — stays empty until the OTA'd firmware publishes /temp
        self.temp_hist = deque(maxlen=HIST)
        self.temp_last = None
        self.temp_count = 0

    def cb(self, msg):
        self.last = {a: getattr(msg.vector, a) for a in AXES}
        for a in AXES:
            self.hist[a].append(self.last[a])
        self.count += 1
        self.draw()

    def cb_temp(self, msg):
        self.temp_last = msg.data
        self.temp_hist.append(msg.data)
        self.temp_count += 1
        self.draw()

    def chart(self, a, color):
        """One axis chart: rows from +VMAX to -VMAX. The row the value
        lands in is shaded by fill fraction (grayscale leading edge);
        fully covered rows are solid. The zero row is a white '=' axis:
        columns where the trace is within half a row of zero show a
        colored block (trace crossing the axis); all other columns show
        the bare axis, never blocks — an off-scale trace must not paint
        phantom data at zero."""
        rows = []
        step = VMAX / PLOT_H
        for row in range(PLOT_H, -PLOT_H, -1):
            top = step * (row + 0.5)
            bot = step * (row - 0.5)
            line = []
            for v in self.hist[a]:
                if row == 0:
                    if abs(v) < step / 2:
                        # trace sitting on the axis: colored block
                        line.append(f"\033[{color}m{SOLID}\033[0m")
                    else:
                        # trace elsewhere: bare white axis
                        line.append(f"\033[97m{AXIS}\033[0m")
                elif v >= top:
                    line.append(SOLID)
                elif v >= bot:
                    frac = (v - bot) / step
                    line.append(SHADES[min(2, int(frac * 3))])
                else:
                    line.append(" ")
            if row == PLOT_H:
                tag = f"{a.upper()} {self.last[a]:+6.2f}"
            else:
                tag = " "
            if row == 0:
                # per-column colors above; no outer wrap
                rows.append(f"{''.join(line)} {tag}")
            else:
                rows.append(f"\033[{color}m{''.join(line)}\033[0m {tag}")
        return rows

    def chart_temp(self):
        """Temp panel: autoscaled to the observed min/max. Blank (dim dots)
        while /temp is absent — the pre-OTA 'before' state."""
        if not self.temp_hist:
            note = " waiting for /temp \u2014 arrives with the OTA update "
            pad = max(0, HIST - len(note))
            blank = [f"\033[90m{'.' * HIST}\033[0m" for _ in range(TEMP_H)]
            blank[TEMP_H // 2] = (
                f"\033[90m{'~' * (pad // 2)}{note}{'~' * (pad - pad // 2)}\033[0m"
            )
            return blank
        lo = min(self.temp_hist)
        hi = max(self.temp_hist)
        span = max(hi - lo, TEMP_MIN_SPAN)
        rows = []
        step = span / TEMP_H
        # rows 1..TEMP_H tile [lo, lo+span] exactly — every value in the
        # observed range lands in a row (the old TEMP_H+1-and-trim version
        # silently dropped the bottom band, hiding the baseline).
        for row in range(TEMP_H, 0, -1):
            top = lo + step * row
            bot = lo + step * (row - 1)
            line = []
            for v in self.temp_hist:
                if v >= top:
                    line.append(SOLID)
                elif v >= bot:
                    frac = (v - bot) / step
                    line.append(SHADES[min(2, int(frac * 3))])
                else:
                    line.append(" ")
            if row == TEMP_H:
                tag = f"\033[90m{hi:5.1f}\033[0m"
            elif row == 1:
                tag = f"\033[90m{lo:5.1f}\033[0m"
            else:
                tag = "     "
            rows.append(f"\033[{TEMP_COLOR}m{''.join(line)}\033[0m {tag}")
        return rows

    def draw(self):
        out = []
        vals = "  ".join(f"{a}={self.last[a]:+6.2f}" for a in AXES)
        rule = " " + "-" * (HIST + 2)
        out.append(f" Tikk /tilt  ({self.count} msgs)  {vals}   m/s^2")
        out.append(rule)
        for a, color in zip(AXES, COLORS):
            for line in self.chart(a, color):
                out.append(f" {line}")
        out.append(rule)
        if self.temp_last is not None:
            head = (f" temp  {self.temp_last:+6.2f} \u00b0C  "
                    f"({self.temp_count} msgs)")
        else:
            head = " temp  \u2014      (no /temp yet \u2014 pre-OTA firmware)"
        out.append(" " + head)
        for line in self.chart_temp():
            out.append(f" {line}")
        out.append(rule)
        out.append(f" {time.strftime('%H:%M:%S')}  ctrl-c to quit")

        # single write + flush: no line-by-line flicker
        sys.stdout.write("\033[H" + "\n".join(out) + "\n")
        sys.stdout.flush()


def main():
    rclpy.init()
    node = TiltViz()
    sys.stdout.write("\033[2J")
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
