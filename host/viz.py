#!/usr/bin/env python3
"""Live terminal view of the Tikk /tilt topic: three scrolling history
charts (x/y/z) plus numeric readout. Pure ANSI, no dependencies.

Run inside a ROS 2 container on the host network:

    docker run --rm --net=host -e FASTDDS_BUILTIN_TRANSPORTS=UDPv4 \
        -v ~/golioth/microros/host:/host:ro ros:kilted-ros-core \
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

HIST = 60          # samples kept (~6 s at 10 Hz)
PLOT_H = 4         # chart rows per axis (keep the whole frame ~28 lines —
                   # taller than the terminal means scroll-jumping every frame)
VMAX = 10.0        # m/s^2 full scale
AXES = ("x", "y", "z")
COLORS = ("96", "93", "92")  # cyan, yellow, green
DASH = "\u2500"    # zero axis
SOLID = "\u2588"
LIGHT = "\u2591"


class TiltViz(Node):
    def __init__(self):
        super().__init__("tikk_viz")
        qos = QoSProfile(
            reliability=ReliabilityPolicy.BEST_EFFORT,
            history=HistoryPolicy.KEEP_LAST,
            depth=50,
        )
        self.sub = self.create_subscription(Vector3Stamped, "/tilt", self.cb, qos)
        self.hist = {a: deque(maxlen=HIST) for a in AXES}
        self.last = {a: 0.0 for a in AXES}
        self.count = 0

    def cb(self, msg):
        self.last = {a: getattr(msg.vector, a) for a in AXES}
        for a in AXES:
            self.hist[a].append(self.last[a])
        self.count += 1
        self.draw()

    def chart(self, a, color):
        """One axis chart: rows from +VMAX to -VMAX, label on the top row."""
        rows = []
        for row in range(PLOT_H, -PLOT_H, -1):
            line = []
            top = VMAX * (row + 0.5) / PLOT_H
            for v in self.hist[a]:
                if row == 0:
                    line.append(DASH if abs(v) < VMAX / PLOT_H / 2 else SOLID)
                elif v >= top:
                    line.append(SOLID)
                else:
                    line.append(" ")
            label = a.upper() if row == PLOT_H else " "
            rows.append(f"\033[{color}m{''.join(line)}\033[0m {label}")
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
